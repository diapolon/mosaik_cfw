#include "fileserver.h"
#include "zipread.h"
#include "fileserver_page.h"
#include "update.h"
#include "../common/lang.h"
#include "../common/platform.h"
#include "../common/durable.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

/* cartelle in cui dalla pagina si può scrivere; tutto il resto si può solo scaricare */
static const char *const WRITABLE[] = { "Roms", "Bios", "Saves", "Media", "Music", "Video", "Linux", "Apps", "Bezels", "Themes", NULL };   /* Video: app Video (0.9.62) */
/* testi della pagina, dalla sezione "fileserver" dei file di lingua */
static const char *const PAGE_KEYS[] = {
    "title", "pin_page", "enter", "wrong_pin", "locked", "free", "readonly", "upload_files", "upload_folder",
    "new_folder", "folder_name", "rename", "new_name", "delete", "confirm_delete", "confirm_delete_folder",
    "download", "download_zip", "exists", "empty", "uploading", "done", "failed", "drop",
    "err_space", "err_readonly", "err_exists", "err_path", "err_toolarge", "err_io", "err_auth", "err_notfound",
    "closed", "home", "remember", "backup", "backup_note", "backup_download", "backup_wifi", "backup_media", "backup_restore",
    "restore_console", "err_notbackup", "backup_games", "backup_games_big", NULL
};
/* nome fisso sulla rete (mDNS): http://mosaik.local */
#define MDNS_NAME "mosaik"
/* browser ricordati: un codice per riga; il file non si vede dalla pagina */
#define BROWSERS_FILE "System/fileserver-browsers.txt"
enum { MAX_BROWSERS = 20 };

static pthread_t thr;
static bool thr_on;
static atomic_bool stop_req, any_change;
static int lsock = -1, port_used;
static char root[1024], pin[8], token[40];
static char *page;
static size_t page_len;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static fs_status_t st;
static int bad_pins;
static time_t locked_until;
static int msock = -1;
static char browsers[MAX_BROWSERS][40];
static int nbrowsers;

static void random_bytes(unsigned char *p, size_t n);

/* ------------------------------------------------------------ stato */

static void set_state(int s, const char *name, long long done, long long total) {
    pthread_mutex_lock(&mu);
    st.state = s;
    if (name) snprintf(st.current, sizeof st.current, "%s", name);
    st.done = done;
    st.total = total;
    pthread_mutex_unlock(&mu);
}

static void event(int kind, const char *name) {
    pthread_mutex_lock(&mu);
    for (int i = 2; i > 0; i--) { st.ev_kind[i] = st.ev_kind[i - 1]; memcpy(st.ev_name[i], st.ev_name[i - 1], sizeof st.ev_name[i]); }
    st.ev_kind[0] = kind;
    snprintf(st.ev_name[0], sizeof st.ev_name[0], "%s", name ? name : "");
    if (st.nevents < 3) st.nevents++;
    if (kind == FS_EV_UPLOAD) st.uploads++;
    pthread_mutex_unlock(&mu);
}

void fs_get_status(fs_status_t *out) {
    pthread_mutex_lock(&mu);
    *out = st;
    pthread_mutex_unlock(&mu);
}
const char *fs_pin(void) { return pin; }
int fs_port(void) { return getenv("ARCADE_FS_SHOWPORT") ? atoi(getenv("ARCADE_FS_SHOWPORT")) : port_used; }   /* schermate: la porta della console */
bool fs_changed(void) { return atomic_load(&any_change); }

void fs_local_ip(char *out, int n) {
    out[0] = 0;
    if (getenv("ARCADE_FS_IP")) { snprintf(out, (size_t)n, "%s", getenv("ARCADE_FS_IP")); return; }   /* schermate del README */
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs) != 0) return;
    for (int pass = 0; pass < 2 && !out[0]; pass++)
        for (i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            if (pass == 0 && strcmp(i->ifa_name, "wlan0")) continue;   /* prima il Wi-Fi della console */
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)n);
            break;
        }
    freeifaddrs(ifs);
}

/* ------------------------------------------------------------ testo */

typedef struct { char *p; size_t n, cap; } buf_t;

static void b_add(buf_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) return;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void b_str(buf_t *b, const char *s) { b_add(b, s, strlen(s)); }
static void b_fmt(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void b_fmt(buf_t *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) b_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
/* stringa JSON tra virgolette */
static void b_json(buf_t *b, const char *s) {
    b_str(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; b_add(b, e, 2); }
        else if (*p < 32) b_fmt(b, "\\u%04x", *p);
        else b_add(b, (const char *)p, 1);
    }
    b_str(b, "\"");
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* valore di un parametro "a=1&b=2", decodificato; falso se manca */
static bool qget(const char *q, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t o = 0;
            for (const char *s = p + kl + 1; s < end && o + 1 < n; s++) {
                if (*s == '%' && s + 2 < end && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { out[o++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2; }
                else if (*s == '+') out[o++] = ' ';
                else out[o++] = *s;
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return false;
}

/* ------------------------------------------------------------ percorsi */

/* un nome valido anche sul FAT: niente punto iniziale, niente caratteri vietati */
static bool valid_name(const char *s) {
    size_t l = strlen(s);
    if (!l || l > 255 || s[0] == '.') return false;
    if (s[l - 1] == ' ' || s[l - 1] == '.') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || strchr("\\/:*?\"<>|", *p)) return false;
    return true;
}

/* percorso relativo ad Arcade/: componenti valide separate da '/'; "" è la cartella principale */
static bool rel_ok(const char *rel) {
    if (strlen(rel) > 900) return false;
    if (!*rel) return true;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *save = NULL;
    int n = 0;
    for (char *c = strtok_r(tmp, "/", &save); c; c = strtok_r(NULL, "/", &save), n++)
        if (!valid_name(c)) return false;
    return n > 0 && rel[0] != '/' && rel[strlen(rel) - 1] != '/' && !strstr(rel, "//");
}

static int depth(const char *rel) {
    if (!*rel) return 0;
    int n = 1;
    for (const char *p = rel; *p; p++) n += *p == '/';
    return n;
}

static bool secret(const char *rel) { return !strcasecmp(rel, "System/wifi.conf") || !strcasecmp(rel, BROWSERS_FILE); }

static bool writable(const char *rel) {
    char top[64];
    size_t k = strcspn(rel, "/");
    if (!k || k >= sizeof top) return false;
    memcpy(top, rel, k);
    top[k] = 0;
    for (int i = 0; WRITABLE[i]; i++)
        if (!strcasecmp(top, WRITABLE[i])) return true;
    return false;
}

static void full_path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s%s%s", root, *rel ? "/" : "", rel); }

static const char *base_name(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

/* ------------------------------------------------------------ HTTP */

typedef struct {
    int fd;
    char method[8], path[256], query[4096], cookie[512];
    long long clen;
    char head[16384];
    int body_off, have;   /* byte del corpo già letti insieme all'intestazione */
} req_t;

static bool send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, c, n, 0);
#endif
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        c += w;
        n -= (size_t)w;
    }
    return true;
}

static const char *reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 507: return "Insufficient Storage";
    default: return "Internal Server Error";
    }
}

static void reply(int fd, int code, const char *type, const char *body, size_t len, const char *extra) {
    char h[1024];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nConnection: close\r\n%s\r\n",
                     code, reason(code), type, len, extra ? extra : "");
    if (send_all(fd, h, (size_t)n) && len) send_all(fd, body, len);
}

static void reply_json(int fd, int code, const char *json) { reply(fd, code, "application/json; charset=utf-8", json, strlen(json), NULL); }

static void reply_err(int fd, int code, const char *what) {
    char j[96];
    snprintf(j, sizeof j, "{\"error\":\"%s\"}", what);
    reply_json(fd, code, j);
}

static bool read_request(req_t *r) {
    int got = 0;
    char *end = NULL;
    while (!end) {
        if (got >= (int)sizeof r->head - 1) return false;
        ssize_t n = recv(r->fd, r->head + got, sizeof r->head - 1 - (size_t)got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += (int)n;
        r->head[got] = 0;
        end = strstr(r->head, "\r\n\r\n");
    }
    r->body_off = (int)(end - r->head) + 4;
    r->have = got - r->body_off;
    char target[4352];
    if (sscanf(r->head, "%7s %4351s", r->method, target) != 2) return false;
    char *q = strchr(target, '?');
    if (q) { *q = 0; snprintf(r->query, sizeof r->query, "%s", q + 1); } else r->query[0] = 0;
    snprintf(r->path, sizeof r->path, "%s", target);
    r->clen = -1;
    r->cookie[0] = 0;
    for (char *l = strstr(r->head, "\r\n"); l && l < end; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        if (!strncasecmp(h, "Content-Length:", 15)) r->clen = strtoll(h + 15, NULL, 10);
        else if (!strncasecmp(h, "Cookie:", 7)) {
            char *e = strstr(h, "\r\n");
            size_t k = e ? (size_t)(e - h - 7) : strlen(h + 7);
            if (k >= sizeof r->cookie) k = sizeof r->cookie - 1;
            memcpy(r->cookie, h + 7, k);
            r->cookie[k] = 0;
        }
    }
    return true;
}

/* valore di un cookie (il nome seguito da '=') */
static bool cookie_is(const req_t *r, const char *name, const char *val) {
    size_t nl = strlen(name), vl = strlen(val);
    if (!vl) return false;
    for (const char *c = r->cookie; (c = strstr(c, name)); c += nl) {
        bool start = c == r->cookie || c[-1] == ' ' || c[-1] == ';';
        if (start && c[nl] == '=' && !strncmp(c + nl + 1, val, vl) && (c[nl + 1 + vl] == 0 || c[nl + 1 + vl] == ';' || c[nl + 1 + vl] == ' '))
            return true;
    }
    return false;
}

static bool authed(const req_t *r) {
    if (cookie_is(r, "mosaik", token)) return true;
    bool ok = false;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nbrowsers && !ok; i++) ok = cookie_is(r, "mosaikr", browsers[i]);
    pthread_mutex_unlock(&mu);
    return ok;
}

/* ---- browser ricordati ---- */

static void browsers_save(void) {   /* chiamare con mu preso */
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/" BROWSERS_FILE, root);
    if (!nbrowsers) { remove(path); sync(); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < nbrowsers; i++) fprintf(f, "%s\n", browsers[i]);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || durable_replace(tmp, path) != 0) remove(tmp);
}

static void browsers_load(void) {
    char path[1200], line[80];
    snprintf(path, sizeof path, "%s/" BROWSERS_FILE, root);
    nbrowsers = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (nbrowsers < MAX_BROWSERS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 32) snprintf(browsers[nbrowsers++], sizeof browsers[0], "%s", line);
    }
    fclose(f);
}

int fs_browsers(void) {
    pthread_mutex_lock(&mu);
    int n = nbrowsers;
    pthread_mutex_unlock(&mu);
    return n;
}

void fs_forget_browsers(void) {
    pthread_mutex_lock(&mu);
    nbrowsers = 0;
    browsers_save();
    pthread_mutex_unlock(&mu);
}

/* legge un corpo piccolo (login, comandi) */
static int small_body(req_t *r, char *out, int n) {
    if (r->clen < 0 || r->clen >= n) return -1;
    int got = r->have < r->clen ? r->have : (int)r->clen;
    memcpy(out, r->head + r->body_off, (size_t)got);
    while (got < r->clen) {
        ssize_t k = recv(r->fd, out + got, (size_t)(r->clen - got), 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        got += (int)k;
    }
    out[got] = 0;
    return got;
}

/* ------------------------------------------------------------ azioni */

static void do_page(req_t *r) { reply(r->fd, 200, "text/html; charset=utf-8", page, page_len, NULL); }

static void do_login(req_t *r) {
    time_t now = time(NULL);
    if (now < locked_until) { reply_err(r->fd, 429, "locked"); return; }
    char body[128], given[16];
    if (small_body(r, body, sizeof body) < 0) { reply_err(r->fd, 400, "path"); return; }
    qget(body, "pin", given, sizeof given);
    unsigned diff = strlen(given) != strlen(pin);
    for (size_t i = 0; i < strlen(pin) && i < strlen(given); i++) diff |= (unsigned)(given[i] ^ pin[i]);
    if (diff) {
        if (++bad_pins >= 5) { bad_pins = 0; locked_until = now + 30; }
        event(FS_EV_FAIL, "PIN");
        reply_err(r->fd, 403, "badpin");
        return;
    }
    bad_pins = 0;
    char hdr[400], rem[8];
    int hn = snprintf(hdr, sizeof hdr, "Set-Cookie: mosaik=%s; Path=/; HttpOnly; SameSite=Strict\r\n", token);
    if (qget(body, "remember", rem, sizeof rem) && rem[0] == '1') {   /* le volte dopo senza PIN */
        unsigned char rb[16];
        char tok[40];
        random_bytes(rb, sizeof rb);
        for (int i = 0; i < 16; i++) snprintf(tok + 2 * i, sizeof tok - 2 * (size_t)i, "%02x", rb[i]);
        pthread_mutex_lock(&mu);
        if (nbrowsers == MAX_BROWSERS) { memmove(browsers[0], browsers[1], sizeof browsers[0] * (MAX_BROWSERS - 1)); nbrowsers--; }
        snprintf(browsers[nbrowsers++], sizeof browsers[0], "%s", tok);
        browsers_save();
        pthread_mutex_unlock(&mu);
        snprintf(hdr + hn, sizeof hdr - (size_t)hn, "Set-Cookie: mosaikr=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict\r\n", tok);
    }
    event(FS_EV_LOGIN, "");
    reply(r->fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, hdr);
}

static long long games_size(void);

static void do_info(req_t *r) {
    struct statvfs vs;
    long long fr = 0, tot = 0;
    if (statvfs(root, &vs) == 0) { fr = (long long)vs.f_bavail * (long long)vs.f_frsize; tot = (long long)vs.f_blocks * (long long)vs.f_frsize; }
    const char *m = plat_model();
    buf_t b = { 0 };
    b_str(&b, "{\"version\":");
    b_json(&b, update_current());
    b_str(&b, ",\"model\":");
    b_json(&b, m ? m : "");
    b_fmt(&b, ",\"free\":%lld,\"total\":%lld,\"games\":%lld,\"writable\":[", fr, tot, games_size());
    for (int i = 0; WRITABLE[i]; i++) { if (i) b_str(&b, ","); b_json(&b, WRITABLE[i]); }
    b_str(&b, "]}");
    reply_json(r->fd, 200, b.p ? b.p : "{}");
    free(b.p);
}

typedef struct { char name[256]; bool dir; long long size; long long mtime; } entry_t;

static int cmp_entry(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void do_list(req_t *r) {
    char rel[1024], full[2048];
    qget(r->query, "path", rel, sizeof rel);
    if (!rel_ok(rel) || secret(rel)) { reply_err(r->fd, 400, "path"); return; }
    full_path(full, sizeof full, rel);
    DIR *d = opendir(full);
    if (!d) { reply_err(r->fd, 404, "notfound"); return; }
    int cap = 256, n = 0;
    entry_t *e = malloc(sizeof *e * (size_t)cap);
    struct dirent *de;
    while (e && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char crel[1300], cfull[2400];
        snprintf(crel, sizeof crel, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        if (secret(crel)) continue;
        snprintf(cfull, sizeof cfull, "%s/%s", full, de->d_name);
        struct stat sb;
        if (stat(cfull, &sb) != 0) continue;
        if (n == cap) {
            entry_t *m = realloc(e, sizeof *e * (size_t)(cap *= 2));
            if (!m) break;
            e = m;
        }
        snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
        e[n].dir = S_ISDIR(sb.st_mode);
        e[n].size = (long long)sb.st_size;
        e[n].mtime = (long long)sb.st_mtime;
        n++;
    }
    closedir(d);
    if (!e) { reply_err(r->fd, 500, "io"); return; }
    qsort(e, (size_t)n, sizeof *e, cmp_entry);
    buf_t b = { 0 };
    b_str(&b, "{\"path\":");
    b_json(&b, rel);
    /* nella cartella principale non si scrive; le cartelle principali scrivibili si aprono */
    b_fmt(&b, ",\"writable\":%s,\"entries\":[", *rel && writable(rel) ? "true" : "false");
    for (int i = 0; i < n; i++) {
        if (i) b_str(&b, ",");
        b_str(&b, "{\"n\":");
        b_json(&b, e[i].name);
        b_fmt(&b, ",\"d\":%d,\"s\":%lld,\"t\":%lld}", e[i].dir, e[i].dir ? 0 : e[i].size, e[i].mtime);
    }
    b_str(&b, "]}");
    free(e);
    reply_json(r->fd, 200, b.p ? b.p : "{}");
    free(b.p);
}

/* intestazione per far scaricare un file con il suo nome, anche con lettere accentate */
static void disposition(char *out, size_t n, const char *name) {
    char ascii[300], enc[1000];
    size_t a = 0, e = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && a + 1 < sizeof ascii && e + 4 < sizeof enc; p++) {
        ascii[a++] = (*p < 128 && *p >= 32 && *p != '"' && *p != '\\') ? (char)*p : '_';
        if (isalnum(*p) || strchr("-._~", *p)) enc[e++] = (char)*p;
        else e += (size_t)snprintf(enc + e, sizeof enc - e, "%%%02X", *p);
    }
    ascii[a] = 0;
    enc[e] = 0;
    snprintf(out, n, "Content-Disposition: attachment; filename=\"%s\"; filename*=UTF-8''%s\r\n", ascii, enc);
}

static void do_get(req_t *r) {
    char rel[1024], full[2048];
    qget(r->query, "path", rel, sizeof rel);
    if (!*rel || !rel_ok(rel) || secret(rel)) { reply_err(r->fd, 400, "path"); return; }
    full_path(full, sizeof full, rel);
    FILE *f = fopen(full, "rb");
    struct stat sb;
    if (!f || fstat(fileno(f), &sb) != 0 || !S_ISREG(sb.st_mode)) { if (f) fclose(f); reply_err(r->fd, 404, "notfound"); return; }
    char h[1800], disp[1400];
    disposition(disp, sizeof disp, base_name(rel));
    int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n%s"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (long long)sb.st_size, disp);
    bool ok = send_all(r->fd, h, (size_t)hn);
    char buf[65536];
    long long done = 0;
    size_t k;
    set_state(FS_SENDING, base_name(rel), 0, (long long)sb.st_size);
    while (ok && !atomic_load(&stop_req) && (k = fread(buf, 1, sizeof buf, f)) > 0) {
        ok = send_all(r->fd, buf, k);
        done += (long long)k;
        set_state(FS_SENDING, NULL, done, (long long)sb.st_size);
    }
    fclose(f);
    if (ok && done == (long long)sb.st_size) event(FS_EV_DOWNLOAD, base_name(rel));
}

/* ---- zip, senza compressione (i file dei giochi sono già compressi): una cartella, o il backup ---- */

typedef struct { char rel[1100]; long long size; } zfile_t;
typedef struct { zfile_t *f; int n, cap; long long total; } zlist_t;

static void zadd(zlist_t *z, const char *rel, long long size) {
    if (z->n == z->cap) {   /* la misura nuova solo se la memoria c'è */
        int nc = z->cap ? z->cap * 2 : 256;
        zfile_t *m = realloc(z->f, sizeof *m * (size_t)nc);
        if (!m) return;
        z->f = m;
        z->cap = nc;
    }
    snprintf(z->f[z->n].rel, sizeof z->f[z->n].rel, "%s", rel);
    z->f[z->n].size = size;
    z->total += size + 30 + 46 + 2 * (long long)strlen(rel);
    z->n++;
}

static void zwalk(zlist_t *z, const char *rel, int lvl) {
    char full[2048];
    full_path(full, sizeof full, rel);
    DIR *d = opendir(full);
    if (!d || lvl > 16) { if (d) closedir(d); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char crel[1100], cfull[2400];
        snprintf(crel, sizeof crel, "%s/%s", rel, de->d_name);
        if (secret(crel)) continue;
        snprintf(cfull, sizeof cfull, "%s/%s", full, de->d_name);
        struct stat sb;
        if (stat(cfull, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { zwalk(z, crel, lvl + 1); continue; }
        if (S_ISREG(sb.st_mode)) zadd(z, crel, (long long)sb.st_size);
    }
    closedir(d);
}

/* App: nel backup solo quelle che non vengono dallo Store; quelle dello Store le ricorda
   mosaik-backup.txt e dopo il ripristino si riscaricano (aggiornate e registrate) */
static bool store_record(const char *id) {
    char rel[300], full[1400];
    snprintf(rel, sizeof rel, "System/pacchetti/%s.txt", id);
    full_path(full, sizeof full, rel);
    return access(full, F_OK) == 0;
}

static void zwalk_apps(zlist_t *z) {
    char full[2048];
    full_path(full, sizeof full, "Apps");
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || store_record(de->d_name)) continue;
        char crel[1100], cfull[2400];
        snprintf(crel, sizeof crel, "Apps/%s", de->d_name);
        snprintf(cfull, sizeof cfull, "%s/%s", full, de->d_name);
        struct stat sb;
        if (stat(cfull, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) zwalk(z, crel, 1);
        else if (S_ISREG(sb.st_mode)) zadd(z, crel, (long long)sb.st_size);
    }
    closedir(d);
}

/* pacchetti installati (registro senza "archived"), separati da spazi */
static void installed_packages(char *out, size_t n) {
    out[0] = 0;
    char full[2048];
    full_path(full, sizeof full, "System/pacchetti");
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *de;
    size_t len = 0;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == '.' || l < 5 || l > 35 || strcmp(de->d_name + l - 4, ".txt")) continue;
        char p[2400], line[16] = "";
        snprintf(p, sizeof p, "%s/%s", full, de->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        bool arch = fgets(line, sizeof line, f) && !strncmp(line, "archived", 8);
        fclose(f);
        if (arch || len + l + 1 >= n) continue;
        len += (size_t)snprintf(out + len, n - len, "%s%.*s", len ? " " : "", (int)(l - 4), de->d_name);
    }
    closedir(d);
}

/* dopo un ripristino: i pacchetti del backup, da proporre sulla console (store_restore_pending) */
static void restore_packages(const fs_backup_t *bk) {
    if (!bk->packages[0]) return;
    char full[1400], tmp[1500], list[1100];
    full_path(full, sizeof full, "System/ripristino-pacchetti.txt");
    snprintf(tmp, sizeof tmp, "%s.tmp", full);
    snprintf(list, sizeof list, "%s", bk->packages);
    for (char *c = list; *c; c++) if (*c == ' ') *c = '\n';
    FILE *f = fopen(tmp, "w");
    bool ok = f && fprintf(f, "%s\n", list) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, full) != 0) { remove(tmp); return; }
    sync();
}

/* ROM e giochi Linux: quanto pesano, per la casella "Anche i giochi" del backup */
static long long games_size(void) {
    zlist_t g = { 0 };
    zwalk(&g, "Roms", 0);
    zwalk(&g, "Linux", 0);
    zwalk_apps(&g);
    free(g.f);
    return g.total;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

typedef struct { int fd; buf_t cd; uint32_t off; int count; long long sent, total; bool ok; } zout_t;

static void zdos(time_t mt, unsigned *dtime, unsigned *ddate) {
    struct tm tmv;
    localtime_r(&mt, &tmv);
    *dtime = (unsigned)(tmv.tm_hour << 11 | tmv.tm_min << 5 | tmv.tm_sec / 2);
    *ddate = (unsigned)((tmv.tm_year > 80 ? tmv.tm_year - 80 : 0) << 9 | (tmv.tm_mon + 1) << 5 | tmv.tm_mday);
}

/* una voce: intestazione locale, dati, voce della directory centrale */
static void zentry(zout_t *zo, const char *name, uint32_t crc, uint32_t sz, time_t mt, FILE *f, const char *mem) {
    unsigned dtime, ddate;
    zdos(mt, &dtime, &ddate);
    size_t nl = strlen(name);
    unsigned char h[30] = { 0 };
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 6, 0x0800);   /* nomi in UTF-8 */
    put16(h + 10, dtime);
    put16(h + 12, ddate);
    put32(h + 14, crc);
    put32(h + 18, sz);
    put32(h + 22, sz);
    put16(h + 26, (unsigned)nl);
    zo->ok = zo->ok && send_all(zo->fd, h, 30) && send_all(zo->fd, name, nl);
    if (mem) zo->ok = zo->ok && send_all(zo->fd, mem, sz);
    else {
        static char buf[65536];
        size_t k;
        while (zo->ok && (k = fread(buf, 1, sizeof buf, f)) > 0 && !atomic_load(&stop_req)) {
            zo->ok = send_all(zo->fd, buf, k);
            zo->sent += (long long)k;
            set_state(FS_SENDING, NULL, zo->sent, zo->total);
        }
    }
    unsigned char c[46] = { 0 };
    put32(c, 0x02014b50);
    put16(c + 4, 20);
    put16(c + 6, 20);
    put16(c + 8, 0x0800);
    put16(c + 12, dtime);
    put16(c + 14, ddate);
    put32(c + 16, crc);
    put32(c + 20, sz);
    put32(c + 24, sz);
    put16(c + 28, (unsigned)nl);
    put32(c + 42, zo->off);
    b_add(&zo->cd, (const char *)c, 46);
    b_add(&zo->cd, name, nl);
    zo->off += (uint32_t)(30 + nl + sz);
    zo->count++;
}

/* manda lo zip dei file in z; i nomi tolgono i primi cut caratteri; extra: una voce in più
   scritta qui (il riepilogo del backup) */
static bool send_zip(int fd, zlist_t *z, size_t cut, const char *zname, const char *extra_name, const char *extra) {
    char h[1800], disp[1400];
    disposition(disp, sizeof disp, zname);
    int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n%sCache-Control: no-store\r\nConnection: close\r\n\r\n", disp);
    zout_t zo = { .fd = fd, .ok = send_all(fd, h, (size_t)hn), .total = z->total };
    set_state(FS_SENDING, zname, 0, z->total);
    time_t now = time(NULL);
    if (extra) zentry(&zo, extra_name, (uint32_t)crc32(0L, (const Bytef *)extra, (uInt)strlen(extra)), (uint32_t)strlen(extra), now, NULL, extra);
    static char buf[65536];
    for (int i = 0; zo.ok && i < z->n && !atomic_load(&stop_req); i++) {
        char full[2048];
        full_path(full, sizeof full, z->f[i].rel);
        FILE *f = fopen(full, "rb");
        if (!f) continue;
        uLong crc = crc32(0L, Z_NULL, 0);
        size_t k;
        long long sz = 0;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) { crc = crc32(crc, (const Bytef *)buf, (uInt)k); sz += (long long)k; }
        if (sz != z->f[i].size) { fclose(f); continue; }   /* cambiato nel frattempo: si salta */
        rewind(f);
        struct stat sb;
        zentry(&zo, z->f[i].rel + cut, (uint32_t)crc, (uint32_t)sz, stat(full, &sb) == 0 ? sb.st_mtime : now, f, NULL);
        fclose(f);
    }
    if (zo.ok && !atomic_load(&stop_req)) {
        unsigned char e[22] = { 0 };
        put32(e, 0x06054b50);
        put16(e + 8, (unsigned)zo.count);
        put16(e + 10, (unsigned)zo.count);
        put32(e + 12, (uint32_t)zo.cd.n);
        put32(e + 16, zo.off);
        zo.ok = (zo.cd.n == 0 || send_all(fd, zo.cd.p, zo.cd.n)) && send_all(fd, e, 22);
    } else zo.ok = false;
    free(zo.cd.p);
    return zo.ok;
}

static void do_zip(req_t *r) {
    char rel[1024], zname[300];
    qget(r->query, "path", rel, sizeof rel);
    if (!*rel || !rel_ok(rel)) { reply_err(r->fd, 400, "path"); return; }
    zlist_t z = { 0 };
    zwalk(&z, rel, 0);
    if (z.total >= 0xFFFF0000LL || z.n > 65000) { free(z.f); reply_err(r->fd, 413, "toolarge"); return; }
    const char *top = base_name(rel);
    snprintf(zname, sizeof zname, "%s.zip", top);
    if (send_zip(r->fd, &z, strlen(rel) - strlen(top), zname, NULL, NULL)) event(FS_EV_ZIP, zname);
    free(z.f);
}

/* ---- backup: salvataggi, impostazioni, preferiti, collezioni, tempo di gioco, BIOS ---- */

/* file di System che vanno nel backup e si possono ripristinare (non la calibrazione delle
   levette, che è dell'hardware, né i file degli aggiornamenti o dei giochi in corso) */
static const char *const BACKUP_SYSTEM[] = { "settings.json", "favorites.txt", "recent.txt", "stats.tsv", "hidden.txt",
                                             "core-options.txt", "fuso-orario.txt", "genre-custom.tsv", "genre-groups.tsv", NULL };
#define BACKUP_INFO "mosaik-backup.txt"

/* cornici: nel backup quelle messe dall'utente, non le due predefinite che porta il sistema né
   le _mosaik dei pacchetti Cornici, che dopo un ripristino si rimettono dallo Store */
static bool bezel_default(const char *rel) {
    if (!strcmp(rel, "Bezels/_default.png") || !strcmp(rel, "Bezels/_default_vertical.png")) return true;
    const char *b = strrchr(rel, '/');
    return !strncmp(rel, "Bezels/", 7) && b && !strncmp(b + 1, "_mosaik", 7);
}

/* ROM, giochi Linux e app: nel backup solo se scelti, perché possono essere tanti */
static bool backup_game(const char *rel) { return !strncmp(rel, "Roms/", 5) || !strncmp(rel, "Linux/", 6) || !strncmp(rel, "Apps/", 5); }

static bool backup_path_ok(const char *rel, bool wifi) {
    if (!rel_ok(rel) || !*rel) return false;
    if (bezel_default(rel)) return false;
    if (!strncmp(rel, "Saves/", 6) || !strncmp(rel, "Bios/", 5) || !strncmp(rel, "Media/", 6) || !strncmp(rel, "System/collections/", 19)
        || !strncmp(rel, "System/chiavi/", 14) || !strncmp(rel, "System/console/", 15) || !strncmp(rel, "Music/", 6)
        || !strncmp(rel, "Video/", 6) || !strncmp(rel, "Bezels/", 7) || backup_game(rel))
        return depth(rel) >= 2;
    if (wifi && !strcmp(rel, "System/wifi.conf")) return true;
    for (int i = 0; BACKUP_SYSTEM[i]; i++) {
        char p[64];
        snprintf(p, sizeof p, "System/%s", BACKUP_SYSTEM[i]);
        if (!strcmp(rel, p)) return true;
    }
    return false;
}

static void do_backup(req_t *r) {
    char v[8];
    bool wifi = qget(r->query, "wifi", v, sizeof v) && v[0] == '1', media = qget(r->query, "media", v, sizeof v) && v[0] == '1',
         games = qget(r->query, "games", v, sizeof v) && v[0] == '1';
    zlist_t z = { 0 };
    zwalk(&z, "Saves", 0);
    zwalk(&z, "Bios", 0);
    zwalk(&z, "System/collections", 0);
    zwalk(&z, "System/chiavi", 0);   /* quale ROM usa quale salvataggio (registro delle identità) */
    zwalk(&z, "System/console", 0);   /* impostazioni di ogni console, scelte nel gioco o in Piattaforme (A62-002) */
    for (int i = 0; BACKUP_SYSTEM[i]; i++) {
        char rel[64], full[1200];
        struct stat sb;
        snprintf(rel, sizeof rel, "System/%s", BACKUP_SYSTEM[i]);
        full_path(full, sizeof full, rel);
        if (stat(full, &sb) == 0 && S_ISREG(sb.st_mode)) zadd(&z, rel, (long long)sb.st_size);
    }
    if (wifi) {   /* scelto da chi scarica: zwalk non lo prende mai */
        char full[1200];
        struct stat sb;
        full_path(full, sizeof full, "System/wifi.conf");
        if (stat(full, &sb) == 0 && S_ISREG(sb.st_mode)) zadd(&z, "System/wifi.conf", (long long)sb.st_size);
    }
    if (media) { zwalk(&z, "Media", 0); zwalk(&z, "Music", 0); zwalk(&z, "Video", 0); }   /* Video: A62-004 */
    int nb = z.n;   /* cornici sempre (pesano poco), senza le due del sistema */
    zwalk(&z, "Bezels", 0);
    int w = nb;
    for (int i = nb; i < z.n; i++) {
        if (bezel_default(z.f[i].rel)) { z.total -= z.f[i].size + 30 + 46 + 2 * (long long)strlen(z.f[i].rel); continue; }
        z.f[w++] = z.f[i];
    }
    z.n = w;
    if (games) { zwalk(&z, "Roms", 0); zwalk(&z, "Linux", 0); zwalk_apps(&z); }
    if (z.total >= 0xFFFF0000LL || z.n > 65000) { free(z.f); reply_err(r->fd, 413, "toolarge"); return; }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char date[32], zname[80], info[1500], pkgs[1024];
    strftime(date, sizeof date, "%d/%m/%Y %H:%M", &tmv);
    strftime(zname, sizeof zname, "mosaik-backup-%Y%m%d-%H%M.zip", &tmv);
    const char *m = plat_model();
    installed_packages(pkgs, sizeof pkgs);
    snprintf(info, sizeof info, "Mosaik backup\nversion=%s\nmodel=%s\ndate=%s\nfiles=%d\npackages=%s\n", update_current(), m ? m : "",
             date, z.n, pkgs);
    if (send_zip(r->fd, &z, 0, zname, BACKUP_INFO, info)) event(FS_EV_BACKUP, zname);
    free(z.f);
}

/* crea le cartelle intermedie di rel (non l'ultima componente) */
static bool make_parents(const char *rel) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *p = tmp; (p = strchr(p, '/')); p++) {
        *p = 0;
        char full[2048];
        full_path(full, sizeof full, tmp);
        struct stat sb;
        if (stat(full, &sb) != 0) { if (mkdir(full, 0755) != 0) return false; atomic_store(&any_change, true); }
        else if (!S_ISDIR(sb.st_mode)) return false;
        *p = '/';
    }
    return true;
}

/* scarta il resto del corpo, così il browser riceve la risposta invece di un errore di rete */
static void drain(req_t *r, long long left) {
    char buf[16384];
    while (left > 0 && !atomic_load(&stop_req)) {
        ssize_t k = recv(r->fd, buf, left < (long long)sizeof buf ? (size_t)left : sizeof buf, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        left -= k;
    }
}

static void do_put(req_t *r) {
    char dir[1024], name[1024], ow[4], rel[2100];
    qget(r->query, "path", dir, sizeof dir);
    qget(r->query, "name", name, sizeof name);
    bool overwrite = qget(r->query, "overwrite", ow, sizeof ow) && ow[0] == '1';
    long long body = r->clen - (r->have > 0 ? r->have : 0);
    snprintf(rel, sizeof rel, "%s/%s", dir, name);
    if (r->clen < 0) { reply_err(r->fd, 400, "path"); return; }
    if (!*dir || !rel_ok(dir) || !rel_ok(name) || !*name || !rel_ok(rel) || secret(rel)) { drain(r, body); reply_err(r->fd, 400, "path"); return; }
    if (!writable(rel)) { drain(r, body); reply_err(r->fd, 403, "readonly"); return; }
    if (r->clen > 0xFFFFFFFFLL - 1) { drain(r, body); reply_err(r->fd, 413, "toolarge"); return; }   /* limite del FAT */
    struct statvfs vs;
    if (statvfs(root, &vs) == 0 && (long long)vs.f_bavail * (long long)vs.f_frsize < r->clen + (1 << 20)) { drain(r, body); reply_err(r->fd, 507, "space"); return; }
    char full[2400], part[2500];
    full_path(full, sizeof full, rel);
    struct stat sb;
    if (stat(full, &sb) == 0 && (S_ISDIR(sb.st_mode) || !overwrite)) { drain(r, body); reply_err(r->fd, 409, "exists"); return; }
    if (!make_parents(rel)) { drain(r, body); reply_err(r->fd, 500, "io"); return; }
    /* ".nome.part" nella stessa cartella: il nome vero arriva solo a file completo */
    const char *bn = base_name(rel);
    snprintf(part, sizeof part, "%.*s.%s.part", (int)(strlen(full) - strlen(bn)), full, bn);
    FILE *f = fopen(part, "wb");
    if (!f) { drain(r, body); reply_err(r->fd, 500, "io"); return; }
    long long got = 0;
    bool ok = true;
    set_state(FS_RECEIVING, bn, 0, r->clen);
    if (r->have > 0) {
        int k = r->have < r->clen ? r->have : (int)r->clen;
        ok = fwrite(r->head + r->body_off, 1, (size_t)k, f) == (size_t)k;
        got += k;
    }
    static char buf[131072];
    while (ok && got < r->clen) {
        if (atomic_load(&stop_req)) { ok = false; break; }
        long long want = r->clen - got;
        ssize_t k = recv(r->fd, buf, want < (long long)sizeof buf ? (size_t)want : sizeof buf, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { ok = false; break; }
        ok = fwrite(buf, 1, (size_t)k, f) == (size_t)k;
        got += k;
        set_state(FS_RECEIVING, NULL, got, r->clen);
    }
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || durable_replace(part, full) != 0) {
        remove(part);
        event(FS_EV_FAIL, bn);
        reply_err(r->fd, 500, "io");
        return;
    }
    atomic_store(&any_change, true);
    event(FS_EV_UPLOAD, bn);
    reply_json(r->fd, 200, "{\"ok\":true}");
}

static void do_mkdir(req_t *r) {
    char dir[1024], name[300], rel[1400], full[2400];
    qget(r->query, "path", dir, sizeof dir);
    qget(r->query, "name", name, sizeof name);
    snprintf(rel, sizeof rel, "%s/%s", dir, name);
    if (!*dir || !rel_ok(dir) || !valid_name(name)) { reply_err(r->fd, 400, "path"); return; }
    if (!writable(rel)) { reply_err(r->fd, 403, "readonly"); return; }
    full_path(full, sizeof full, rel);
    if (mkdir(full, 0755) != 0) { reply_err(r->fd, errno == EEXIST ? 409 : 500, errno == EEXIST ? "exists" : "io"); return; }
    sync();
    atomic_store(&any_change, true);
    event(FS_EV_MKDIR, name);
    reply_json(r->fd, 200, "{\"ok\":true}");
}

static void do_rename(req_t *r) {
    char rel[1024], to[300], dst[1400], full[2400], fdst[2400];
    qget(r->query, "path", rel, sizeof rel);
    qget(r->query, "to", to, sizeof to);
    if (!rel_ok(rel) || depth(rel) < 2 || !valid_name(to) || secret(rel)) { reply_err(r->fd, 400, "path"); return; }
    if (!writable(rel)) { reply_err(r->fd, 403, "readonly"); return; }
    snprintf(dst, sizeof dst, "%.*s%s", (int)(strlen(rel) - strlen(base_name(rel))), rel, to);
    full_path(full, sizeof full, rel);
    full_path(fdst, sizeof fdst, dst);
    struct stat sb;
    /* sul FAT "Nome" e "nome" sono lo stesso file: cambiare solo le maiuscole è permesso */
    if (stat(fdst, &sb) == 0 && strcasecmp(base_name(rel), to)) { reply_err(r->fd, 409, "exists"); return; }
    if (rename(full, fdst) != 0) { reply_err(r->fd, errno == ENOENT ? 404 : 500, errno == ENOENT ? "notfound" : "io"); return; }
    sync();
    atomic_store(&any_change, true);
    event(FS_EV_RENAME, to);
    reply_json(r->fd, 200, "{\"ok\":true}");
}

static bool remove_tree(const char *full, int lvl) {
    struct stat sb;
    if (lstat(full, &sb) != 0) return false;
    if (!S_ISDIR(sb.st_mode)) return unlink(full) == 0;
    if (lvl > 32) return false;
    DIR *d = opendir(full);
    if (!d) return false;
    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char c[2400];
        snprintf(c, sizeof c, "%s/%s", full, de->d_name);
        ok = remove_tree(c, lvl + 1) && ok;
    }
    closedir(d);
    return rmdir(full) == 0 && ok;
}

static void do_delete(req_t *r) {
    char rel[1024], full[2048];
    qget(r->query, "path", rel, sizeof rel);
    if (!rel_ok(rel) || depth(rel) < 2 || secret(rel)) { reply_err(r->fd, 400, "path"); return; }
    if (!writable(rel)) { reply_err(r->fd, 403, "readonly"); return; }
    full_path(full, sizeof full, rel);
    struct stat sb;
    if (stat(full, &sb) != 0) { reply_err(r->fd, 404, "notfound"); return; }
    bool ok = remove_tree(full, 0);
    sync();
    atomic_store(&any_change, true);
    if (!ok) { reply_err(r->fd, 500, "io"); return; }
    event(FS_EV_DELETE, base_name(rel));
    reply_json(r->fd, 200, "{\"ok\":true}");
}

/* ---- ripristino di un backup: lo zip arriva dalla pagina, si controlla, e i file si scrivono
   solo dopo la conferma sulla console ---- */

#define RESTORE_ZIP "System/.ripristino.zip"

static atomic_int restore_state;   /* FS_RS_* */
static atomic_bool restore_go, restore_drop;
static atomic_int restore_parts;   /* parti scelte sulla console, 1 << FS_PART_* */
static fs_backup_t restore_bk;     /* il backup arrivato: data, file per parte */

static void restore_set(int s) {
    atomic_store(&restore_state, s);
    pthread_mutex_lock(&mu);
    st.restore = s;
    st.restore_bk = restore_bk;
    pthread_mutex_unlock(&mu);
}

static void restore_remove_zip(void) {
    char full[1200];
    full_path(full, sizeof full, RESTORE_ZIP);
    remove(full);
}

/* parte del backup a cui appartiene un file (per scegliere cosa ripristinare) */
static int backup_part(const char *rel) {
    if (!strncmp(rel, "Saves/", 6) || !strncmp(rel, "System/chiavi/", 14)) return FS_PART_SAVES;   /* il registro va con i salvataggi */
    if (!strncmp(rel, "Bios/", 5)) return FS_PART_BIOS;
    if (!strncmp(rel, "Media/", 6) || !strncmp(rel, "Music/", 6) || !strncmp(rel, "Video/", 6) || !strncmp(rel, "Bezels/", 7))
        return FS_PART_MEDIA;
    if (backup_game(rel)) return FS_PART_GAMES;
    if (!strcmp(rel, "System/wifi.conf")) return FS_PART_WIFI;
    return FS_PART_SETTINGS;   /* impostazioni, liste, collezioni, tempo di gioco */
}

/* lo zip (percorso completo) è un backup di Mosaik? data e file che si ripristinerebbero, per parte */
static bool backup_read(const char *zipfull, fs_backup_t *bk) {
    char info[2048];
    memset(bk, 0, sizeof *bk);
    FILE *f = fopen(zipfull, "rb");
    if (!f) return false;
    zent_t *z;
    int n = zip_dir(f, &z);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (!strcmp(z[i].name, BACKUP_INFO)) found = zip_entry(f, &z[i], NULL, info, sizeof info) && !strncmp(info, "Mosaik backup", 13);
        else if (backup_path_ok(z[i].name, true)) { bk->files++; bk->count[backup_part(z[i].name)]++; }
    }
    free(z);
    fclose(f);
    if (!found) return false;
    const char *d = strstr(info, "date=");
    snprintf(bk->date, sizeof bk->date, "%.*s", d ? (int)strcspn(d + 5, "\n") : 0, d ? d + 5 : "");
    const char *pk = strstr(info, "\npackages=");   /* dalla 0.9.29 */
    snprintf(bk->packages, sizeof bk->packages, "%.*s", pk ? (int)strcspn(pk + 10, "\n") : 0, pk ? pk + 10 : "");
    return true;
}

static bool restore_check(void) {
    char full[1200];
    full_path(full, sizeof full, RESTORE_ZIP);
    return backup_read(full, &restore_bk);
}

static void do_restore_upload(req_t *r) {
    long long body = r->clen - (r->have > 0 ? r->have : 0);
    if (r->clen <= 0 || r->clen > 0xFFFFFFFFLL - 1) { drain(r, body); reply_err(r->fd, 413, "toolarge"); return; }   /* limite del FAT */
    if (atomic_load(&restore_state) == FS_RS_RUNNING) { drain(r, body); reply_err(r->fd, 409, "exists"); return; }
    struct statvfs vs;
    if (statvfs(root, &vs) == 0 && (long long)vs.f_bavail * (long long)vs.f_frsize < 2 * r->clen + (1 << 20)) { drain(r, body); reply_err(r->fd, 507, "space"); return; }
    char full[1200], part[1300];
    full_path(full, sizeof full, RESTORE_ZIP);
    snprintf(part, sizeof part, "%s.part", full);
    FILE *f = fopen(part, "wb");
    if (!f) { drain(r, body); reply_err(r->fd, 500, "io"); return; }
    long long got = 0;
    bool ok = true;
    set_state(FS_RECEIVING, "backup", 0, r->clen);
    if (r->have > 0) {
        int k = r->have < r->clen ? r->have : (int)r->clen;
        ok = fwrite(r->head + r->body_off, 1, (size_t)k, f) == (size_t)k;
        got += k;
    }
    static char buf[131072];
    while (ok && got < r->clen && !atomic_load(&stop_req)) {
        long long want = r->clen - got;
        ssize_t k = recv(r->fd, buf, want < (long long)sizeof buf ? (size_t)want : sizeof buf, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { ok = false; break; }
        ok = fwrite(buf, 1, (size_t)k, f) == (size_t)k;
        got += k;
        set_state(FS_RECEIVING, NULL, got, r->clen);
    }
    ok = ok && got == r->clen && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(part, full) != 0) { remove(part); reply_err(r->fd, 500, "io"); return; }
    if (!restore_check()) { restore_remove_zip(); restore_set(FS_RS_NONE); reply_err(r->fd, 400, "notbackup"); return; }
    restore_set(FS_RS_PENDING);   /* ora la conferma sulla console */
    event(FS_EV_RESTORE, restore_bk.date);
    char j[160];
    snprintf(j, sizeof j, "{\"ok\":true,\"date\":\"%s\",\"files\":%d}", restore_bk.date, restore_bk.files);
    reply_json(r->fd, 200, j);
}

/* dopo la conferma, nel thread del server: ogni file passa da ".nome.part" e prende il suo nome
   solo se è completo e con il CRC giusto */
/* scrive i file permessi dello zip sotto root: ognuno passa da ".nome.part" e prende il suo
   nome solo se è completo e con il CRC giusto */
/* Ripristino in due fasi (verifica audit BUG-007): prima tutti i file scelti si scrivono come
   temporanei, con il CRC controllato e dopo aver visto che lo spazio basta; se uno non va, i
   temporanei si tolgono e i file attuali restano com'erano. Solo quando sono arrivati tutti
   prendono il posto dei vecchi (rinomine, veloci). */
static void backup_part_name(const char *rel, char *part, size_t n) {
    char dst[2400];
    full_path(dst, sizeof dst, rel);
    const char *bn = base_name(rel);
    snprintf(part, n, "%.*s.%s.part", (int)(strlen(dst) - strlen(bn)), dst, bn);
}

/* Sostituzione annullabile (audit A38-004): prima di toccare un file si scrive
   System/ripristino-giornale.txt con tutti quelli che cambiano ("sostituito" o "nuovo"); il file
   di prima diventa ".nome.annulla" accanto, quello del backup prende il suo posto. Se una
   rinomina fallisce o manca la corrente, tutto torna com'era: subito, o al prossimo avvio
   (fs_restore_recover). La riga "fatto" è il punto di non ritorno: dopo si tolgono solo gli
   .annulla. Finché il giornale resta, non parte nessun altro ripristino. */
#define RESTORE_JOURNAL "System/ripristino-giornale.txt"

static void backup_ann_name(const char *rel, char *ann, size_t n) {
    char dst[2400];
    full_path(dst, sizeof dst, rel);
    const char *bn = base_name(rel);
    snprintf(ann, n, "%.*s.%s.annulla", (int)(strlen(dst) - strlen(bn)), dst, bn);
}

#ifdef FS_TEST
/* prove: RESTORE_FAIL_AT=N fa fallire l'N-esimo spostamento, RESTORE_STOP_AT=N esce lì (corrente) */
static int rs_step;
static int rs_rename(const char *a, const char *b) {
    rs_step++;
    const char *e = getenv("RESTORE_STOP_AT");
    if (e && atoi(e) == rs_step) _exit(3);
    e = getenv("RESTORE_FAIL_AT");
    if (e && atoi(e) == rs_step) { errno = EIO; return -1; }
    return rename(a, b);
}
#else
#define rs_rename rename
#endif

static bool rs_exists(const char *p) { struct stat sb; return lstat(p, &sb) == 0; }

static bool rs_write_durable(const char *path, const char *data, size_t n) {
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = false;
    if (!ok || durable_replace(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}


/* il giornale è intero? prima riga "ripristino N", poi proprio N righe di file (R39-02) */
static bool restore_journal_valid(const char *t) {
    if (strncmp(t, "ripristino ", 11)) return false;
    int want = atoi(t + 11), files = 0;
    for (const char *l = t; *l;) {
        const char *e = strchr(l, '\n');
        if (!e) return false;
        if (!strncmp(l, "sostituito ", 11) || !strncmp(l, "nuovo ", 6)) files++;
        l = e + 1;
    }
    return want > 0 && want == files;
}

/* rimette com'era (done = false) o chiude (done = true); si può ripetere */
static bool restore_journal_apply(const char *text, bool done) {
    bool ok = true;
    for (const char *l = text; *l;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1300];
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), l);
        l += len + (e ? 1 : 0);
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *rel = sp + 1;
        bool repl = !strcmp(line, "sostituito"), fresh = !strcmp(line, "nuovo");
        if ((!repl && !fresh) || !backup_path_ok(rel, true)) continue;
        char part[2500], ann[2500], dst[2400];
        backup_part_name(rel, part, sizeof part);
        backup_ann_name(rel, ann, sizeof ann);
        full_path(dst, sizeof dst, rel);
        remove(part);
        if (done) { if (repl) remove(ann); }
        else if (fresh) { if (remove(dst) != 0 && errno != ENOENT) ok = false; }
        else if (rs_exists(ann) && rename(ann, dst) != 0) {
            fprintf(stderr, "ripristino: non riesco a rimettere %s\n", rel);
            ok = false;
        }
    }
    return ok;
}

/* true: nessun ripristino in sospeso. Un giornale che non si legge resta dov'è. */
static bool restore_recover(void) {
    char jp[1200];
    full_path(jp, sizeof jp, RESTORE_JOURNAL);
    FILE *f = fopen(jp, "rb");
    if (!f) return errno == ENOENT;
    size_t cap = 4u << 20, n = 0;
    char *t = malloc(cap + 1);
    if (t) n = fread(t, 1, cap, f);
    bool bad = !t || ferror(f) || (n == cap && fgetc(f) != EOF);
    fclose(f);
    if (bad) { free(t); fprintf(stderr, "ripristino: giornale illeggibile, resta\n"); return false; }
    t[n] = 0;
    if (!restore_journal_valid(t)) {
        /* non si può applicare né sapere cosa era già stato scritto (un file nuovo non lascia una
           copia di quello di prima): resta fermo finché non lo decide l'utente (R40-02) */
        fprintf(stderr, "ripristino: giornale guasto: resta, fermo\n");
        free(t);
        return false;
    }
    bool done = strstr(t, "\nfatto\n") != NULL, ok = restore_journal_apply(t, done);
    free(t);
    if (ok) {
        sync();
        ok = remove(jp) == 0 || errno == ENOENT;
        sync();
        fprintf(stderr, "ripristino interrotto: %s\n", done ? "concluso" : "annullato, rimessi i file di prima");
        atomic_store(&any_change, true);
    } else fprintf(stderr, "ripristino: giornale non risolto, si riprova al prossimo avvio\n");
    return ok;
}

bool fs_restore_recover(const char *dir) {
    if (thr_on) return true;   /* il server è acceso: la cartella è già la sua */
    snprintf(root, sizeof root, "%s", dir);
    return restore_recover();
}

/* scelta dell'utente: il giornale irrisolto si mette da parte in ripristino-giornale-guasto.txt */
bool fs_restore_force(const char *dir) {
    if (!thr_on) snprintf(root, sizeof root, "%s", dir);
    char jp[1200], kept[1300];
    full_path(jp, sizeof jp, RESTORE_JOURNAL);
    full_path(kept, sizeof kept, "System/ripristino-giornale-guasto.txt");
    bool moved = rename(jp, kept) == 0 || errno == ENOENT;
    fprintf(stderr, "ripristino: giornale messo da parte per scelta dell'utente: %s\n", moved ? "fatto" : strerror(errno));
    sync();
    return moved;
}

static const zent_t *dup_z;
static int dup_cmp(const void *a, const void *b) { return strcasecmp(dup_z[*(const int *)a].name, dup_z[*(const int *)b].name); }

static bool backup_extract(const char *zipfull, int parts, int total) {
    if (!restore_recover()) return false;   /* un ripristino di prima non è chiuso */
    FILE *f = fopen(zipfull, "rb");
    zent_t *z = NULL;
    int n = f ? zip_dir(f, &z) : -1, done = 0, staged = 0, nsel = 0;
    bool ok = n >= 0;
    int *idx = ok ? malloc(sizeof(int) * (size_t)(n ? n : 1)) : NULL;
    int *sel = ok ? malloc(sizeof(int) * (size_t)(n ? n : 1)) : NULL;
    if (!idx || !sel) ok = false;
    long long need = 0;
    for (int i = 0; ok && i < n; i++)
        if (backup_path_ok(z[i].name, true) && (parts & (1 << backup_part(z[i].name)))) { need += z[i].usize; sel[nsel++] = i; }
    /* due voci con lo stesso nome, anche solo per le maiuscole (sulla FAT è lo stesso file):
       finirebbero nello stesso temporaneo. Un archivio così non si ripristina */
    if (ok && nsel > 1) {
        dup_z = z;
        qsort(sel, (size_t)nsel, sizeof(int), dup_cmp);
        for (int k = 1; ok && k < nsel; k++)
            if (!strcasecmp(z[sel[k - 1]].name, z[sel[k]].name)) {
                fprintf(stderr, "ripristino: %s compare due volte nel backup\n", z[sel[k]].name);
                ok = false;
            }
    }
    struct statvfs vs;   /* temporanei e vecchi file insieme: deve starci tutto, più un margine */
    if (ok && (statvfs(root, &vs) != 0 || need + (32LL << 20) > (long long)vs.f_bavail * (long long)vs.f_frsize)) {
        fprintf(stderr, "ripristino: spazio insufficiente (%lld MB da scrivere)\n", need >> 20);
        ok = false;
    }
    for (int i = 0; ok && i < n; i++) {
        const char *rel = z[i].name;
        if (!backup_path_ok(rel, true) || !(parts & (1 << backup_part(rel)))) continue;   /* altro o non scelto: si ignora */
        set_state(FS_RECEIVING, base_name(rel), done, total);
        char part[2500];
        backup_part_name(rel, part, sizeof part);
        if (!make_parents(rel)) { ok = false; break; }
        FILE *o = fopen(part, "wb");
        if (!o) { ok = false; break; }
        bool w = zip_entry(f, &z[i], o, NULL, 0);
        w = w && fflush(o) == 0 && fsync(fileno(o)) == 0;
        if (fclose(o) != 0) w = false;
        idx[staged++] = i;
        if (!w) { ok = false; break; }
        done++;
    }
    if (!ok) {   /* niente è stato toccato: via i temporanei */
        for (int k = 0; k < staged; k++) {
            char part[2500];
            backup_part_name(z[idx[k]].name, part, sizeof part);
            remove(part);
        }
    } else if (staged) {
        /* il giornale: tutto quello che si farà, prima di farlo */
        char jp[1200];
        full_path(jp, sizeof jp, RESTORE_JOURNAL);
        size_t cap = 48, len = 0;
        for (int k = 0; k < staged; k++) cap += strlen(z[idx[k]].name) + 16;
        char *jt = malloc(cap);
        if (!jt) ok = false;
        else len = (size_t)snprintf(jt, cap, "ripristino %d\n", staged);
        for (int k = 0; ok && k < staged; k++) {
            char dst[2400];
            full_path(dst, sizeof dst, z[idx[k]].name);
            len += (size_t)snprintf(jt + len, cap - len, "%s %s\n", rs_exists(dst) ? "sostituito" : "nuovo", z[idx[k]].name);
        }
        bool journaled = ok && rs_write_durable(jp, jt, len);
        if (!journaled) ok = false;
        for (int k = 0; ok && k < staged; k++) {   /* i file di prima da parte, quelli del backup al loro posto */
            char part[2500], ann[2500], dst[2400];
            backup_part_name(z[idx[k]].name, part, sizeof part);
            backup_ann_name(z[idx[k]].name, ann, sizeof ann);
            full_path(dst, sizeof dst, z[idx[k]].name);
            if (rs_exists(dst) && rs_rename(dst, ann) != 0) ok = false;
            else if (rs_rename(part, dst) != 0) ok = false;
        }
        if (ok) {   /* il punto di non ritorno */
            memcpy(jt + len, "fatto\n", 7);
            if (!rs_write_durable(jp, jt, len + 6)) ok = false;
        }
        if (journaled) {
            jt[len] = 0;
            if (restore_journal_apply(jt, ok)) { sync(); remove(jp); }
            else fprintf(stderr, "ripristino: non rimesso del tutto, si riprova al prossimo avvio\n");
        } else {
            for (int k = 0; k < staged; k++) {
                char part[2500];
                backup_part_name(z[idx[k]].name, part, sizeof part);
                remove(part);
            }
        }
        if (!ok) fprintf(stderr, "ripristino non riuscito: rimessi i file di prima\n");
        free(jt);
    }
    free(idx);
    free(sel);
    free(z);
    if (f) fclose(f);
    sync();
    if (staged) atomic_store(&any_change, true);
    set_state(FS_IDLE, "", 0, 0);
    return ok;
}

static void restore_run(void) {
    restore_set(FS_RS_RUNNING);
    char full[1200];
    full_path(full, sizeof full, RESTORE_ZIP);
    int parts = atomic_load(&restore_parts), total = 0;
    for (int p = 0; p < FS_NPARTS; p++) if (parts & (1 << p)) total += restore_bk.count[p];
    bool ok = backup_extract(full, parts, total);
    if (ok) restore_packages(&restore_bk);
    /* non riuscito: lo zip resta fino alla prossima apertura della finestra (fs_start lo toglie) */
    if (ok) restore_remove_zip();
    restore_set(ok ? FS_RS_DONE : FS_RS_FAILED);
}

bool fs_backup_info(const char *zip, fs_backup_t *out) {
    return backup_read(zip, out);
}

bool fs_backup_import(const char *dir, const char *zip, int parts) {
    if (thr_on) return false;   /* il server ha già la sua cartella */
    snprintf(root, sizeof root, "%s", dir);
    fs_backup_t bk;
    if (!backup_read(zip, &bk)) return false;
    int total = 0;
    for (int p = 0; p < FS_NPARTS; p++) if (parts & (1 << p)) total += bk.count[p];
    bool ok = backup_extract(zip, parts, total);
    if (ok) restore_packages(&bk);
    return ok;
}

void fs_restore_answer(int parts) {
    if (atomic_load(&restore_state) != FS_RS_PENDING) return;
    atomic_store(&restore_parts, parts);
    if (parts) atomic_store(&restore_go, true);
    else atomic_store(&restore_drop, true);
}

void fs_restore_clear(void) { restore_set(FS_RS_NONE); }

static void handle(int fd) {
    req_t *r = calloc(1, sizeof *r);
    if (!r) return;
    r->fd = fd;
    if (!read_request(r)) { free(r); return; }
    bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST"), put = !strcmp(r->method, "PUT");
    if (get && (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html"))) do_page(r);
    else if (post && !strcmp(r->path, "/api/login")) do_login(r);
    else if (!strncmp(r->path, "/api/", 5) && !authed(r)) { if (put) drain(r, r->clen - r->have); reply_err(fd, 401, "auth"); }
    else if (get && !strcmp(r->path, "/api/info")) do_info(r);
    else if (get && !strcmp(r->path, "/api/list")) do_list(r);
    else if (get && !strcmp(r->path, "/api/get")) do_get(r);
    else if (get && !strcmp(r->path, "/api/zip")) do_zip(r);
    else if (get && !strcmp(r->path, "/api/backup")) do_backup(r);
    else if (put && !strcmp(r->path, "/api/restore")) do_restore_upload(r);
    else if (put && !strcmp(r->path, "/api/put")) do_put(r);
    else if (post && !strcmp(r->path, "/api/mkdir")) do_mkdir(r);
    else if (post && !strcmp(r->path, "/api/rename")) do_rename(r);
    else if (post && !strcmp(r->path, "/api/delete")) do_delete(r);
    else reply_err(fd, 404, "notfound");
    set_state(FS_IDLE, "", 0, 0);
    free(r);
}

/* ---- nome fisso sulla rete: risponde alle domande mDNS per "mosaik.local" ---- */

static size_t mdns_name(unsigned char *p) {
    static const char lab[] = "\x06" MDNS_NAME "\x05local";
    memcpy(p, lab, sizeof lab);   /* con lo zero finale */
    return sizeof lab;
}

/* risposta con l'indirizzo IPv4; ttl 0 = "non c'è più" (alla chiusura) */
static void mdns_send(const struct sockaddr_in *to, uint16_t id, bool legacy, const unsigned char *q, size_t qlen, uint32_t ttl) {
    char ip[48];
    struct in_addr a;
    fs_local_ip(ip, sizeof ip);
    if (!ip[0] || inet_pton(AF_INET, ip, &a) != 1) return;
    unsigned char pk[512];
    size_t n = 0;
    pk[n++] = (unsigned char)(id >> 8); pk[n++] = (unsigned char)id;
    pk[n++] = 0x84; pk[n++] = 0;                         /* risposta, con autorità */
    pk[n++] = 0; pk[n++] = legacy && q ? 1 : 0;          /* domande */
    pk[n++] = 0; pk[n++] = 1;                            /* risposte */
    memset(pk + n, 0, 4); n += 4;
    if (legacy && q && qlen < 300) { memcpy(pk + n, q, qlen); n += qlen; }
    n += mdns_name(pk + n);
    pk[n++] = 0; pk[n++] = 1;                            /* tipo A */
    pk[n++] = legacy ? 0 : 0x80; pk[n++] = 1;            /* classe IN, "sostituisci in cache" */
    pk[n++] = (unsigned char)(ttl >> 24); pk[n++] = (unsigned char)(ttl >> 16); pk[n++] = (unsigned char)(ttl >> 8); pk[n++] = (unsigned char)ttl;
    pk[n++] = 0; pk[n++] = 4;
    memcpy(pk + n, &a.s_addr, 4); n += 4;
    sendto(msock, pk, n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void mdns_group(struct sockaddr_in *g) {
    memset(g, 0, sizeof *g);
    g->sin_family = AF_INET;
    g->sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &g->sin_addr);
}

static void mdns_open(void) {
    msock = socket(AF_INET, SOCK_DGRAM, 0);
    if (msock < 0) return;
    int one = 1;
    unsigned char ttl = 255;
    setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(msock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);   /* sul Mac c'è già mDNSResponder */
#endif
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(5353);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    struct ip_mreq mr;
    memset(&mr, 0, sizeof mr);
    inet_pton(AF_INET, "224.0.0.251", &mr.imr_multiaddr);
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    if (bind(msock, (struct sockaddr *)&a, sizeof a) != 0 || setsockopt(msock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) != 0) {
        close(msock);
        msock = -1;
        return;
    }
    setsockopt(msock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in g;
    mdns_group(&g);
    mdns_send(&g, 0, false, NULL, 0, 120);   /* annuncio */
}

static void mdns_close(void) {
    if (msock < 0) return;
    struct sockaddr_in g;
    mdns_group(&g);
    mdns_send(&g, 0, false, NULL, 0, 0);   /* il nome non vale più */
    close(msock);
    msock = -1;
}

/* nome di una domanda, in minuscolo con i punti; restituisce dove finisce, 0 se rovinato */
static size_t mdns_read_name(const unsigned char *pk, size_t len, size_t off, char *out, size_t n) {
    size_t o = 0, end = 0;
    int jumps = 0;
    while (off < len) {
        unsigned l = pk[off];
        if (l == 0) { if (!end) end = off + 1; out[o] = 0; return end; }
        if ((l & 0xC0) == 0xC0) {
            if (off + 1 >= len || ++jumps > 8) return 0;
            if (!end) end = off + 2;
            off = (size_t)((l & 0x3F) << 8 | pk[off + 1]);
            continue;
        }
        if (off + 1 + l > len || o + l + 2 > n) return 0;
        if (o) out[o++] = '.';
        for (unsigned i = 0; i < l; i++) out[o++] = (char)tolower(pk[off + 1 + i]);
        off += 1 + l;
    }
    return 0;
}

static void mdns_handle(void) {
    unsigned char pk[1500];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t len = recvfrom(msock, pk, sizeof pk, 0, (struct sockaddr *)&from, &fl);
    if (len < 12 || (pk[2] & 0x80)) return;   /* solo domande */
    unsigned qd = (unsigned)(pk[4] << 8 | pk[5]);
    size_t off = 12;
    for (unsigned i = 0; i < qd && i < 16; i++) {
        char name[256];
        size_t start = off, e = mdns_read_name(pk, (size_t)len, off, name, sizeof name);
        if (!e || e + 4 > (size_t)len) return;
        unsigned type = (unsigned)(pk[e] << 8 | pk[e + 1]), cls = (unsigned)(pk[e + 2] << 8 | pk[e + 3]);
        off = e + 4;
        if (strcmp(name, MDNS_NAME ".local") || (type != 1 && type != 255)) continue;
        bool legacy = ntohs(from.sin_port) != 5353;   /* chi chiede da un'altra porta vuole la risposta a sé */
        if (legacy || (cls & 0x8000)) mdns_send(&from, (uint16_t)(pk[0] << 8 | pk[1]), legacy, pk + start, off - start, 120);
        else { struct sockaddr_in g; mdns_group(&g); mdns_send(&g, 0, false, NULL, 0, 120); }
        return;
    }
}

/* avanzi di caricamenti interrotti (console spenta a metà): ".nome.part" nelle cartelle
   scrivibili, tolti all'apertura */
static void clean_parts(const char *full, int lvl) {
    DIR *d = opendir(full);
    if (!d || lvl > 8) { if (d) closedir(d); return; }
    struct dirent *de;
    while ((de = readdir(d)) && !atomic_load(&stop_req)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char c[2400];
        snprintf(c, sizeof c, "%s/%s", full, de->d_name);
        size_t l = strlen(de->d_name);
        struct stat sb;
        if (lstat(c, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { if (de->d_name[0] != '.') clean_parts(c, lvl + 1); }
        else if (de->d_name[0] == '.' && l > 6 && !strcmp(de->d_name + l - 5, ".part")) remove(c);
    }
    closedir(d);
}

static void *serve(void *arg) {
    (void)arg;
    for (int i = 0; WRITABLE[i]; i++) {
        char full[1200];
        snprintf(full, sizeof full, "%s/%s", root, WRITABLE[i]);
        clean_parts(full, 0);
    }
    mdns_open();
    while (!atomic_load(&stop_req)) {
        struct pollfd p[2] = { { lsock, POLLIN, 0 }, { msock, POLLIN, 0 } };
        /* le risposte della console al ripristino: anche senza richieste in arrivo */
        if (atomic_exchange(&restore_go, false)) restore_run();
        if (atomic_exchange(&restore_drop, false)) { restore_remove_zip(); restore_set(FS_RS_NONE); }
        if (poll(p, msock >= 0 ? 2 : 1, 250) <= 0) continue;
        if (msock >= 0 && (p[1].revents & POLLIN)) mdns_handle();
        if (!(p[0].revents & POLLIN)) continue;
        int c = accept(lsock, NULL, NULL);
        if (c < 0) continue;
        struct timeval tv = { 20, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        handle(c);
        close(c);
    }
    mdns_close();
    return NULL;
}

/* ------------------------------------------------------------ avvio */

static void random_bytes(unsigned char *p, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t k = read(fd, p + got, n - got);
            if (k <= 0) break;
            got += (size_t)k;
        }
        close(fd);
    }
    for (; got < n; got++) p[got] = (unsigned char)(rand() ^ (int)time(NULL));
}

static void build_page(void) {
    buf_t s = { 0 };
    b_str(&s, "{");
    for (int i = 0; PAGE_KEYS[i]; i++) {
        char k[64];
        snprintf(k, sizeof k, "fileserver.%s", PAGE_KEYS[i]);
        if (i) b_str(&s, ",");
        b_json(&s, PAGE_KEYS[i]);
        b_str(&s, ":");
        b_json(&s, T(k));
    }
    b_str(&s, "}");
    /* nella pagina "</" non può comparire dentro lo script */
    buf_t esc = { 0 };
    for (size_t i = 0; s.p && i < s.n; i++) {
        if (s.p[i] == '<') b_str(&esc, "\\u003c");
        else b_add(&esc, s.p + i, 1);
    }
    free(s.p);
    const char *mark = strstr(FS_PAGE, "__STRINGS__");
    buf_t p = { 0 };
    if (mark) {
        b_add(&p, FS_PAGE, (size_t)(mark - FS_PAGE));
        b_str(&p, esc.p ? esc.p : "{}");
        b_str(&p, mark + 11);
    } else b_str(&p, FS_PAGE);
    free(esc.p);
    free(page);
    page = p.p;
    page_len = p.n;
}

bool fs_start(const char *dir) {
    if (thr_on) return true;
    snprintf(root, sizeof root, "%s", dir);
    signal(SIGPIPE, SIG_IGN);
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) return false;
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* la porta 80 dà un indirizzo più corto da scrivere; senza permessi (sul Mac) la 8080 */
    int ports[] = { 80, 8080, 8081, 8088 }, np = 4;
    if (getenv("ARCADE_FS_PORT")) { ports[0] = atoi(getenv("ARCADE_FS_PORT")); np = 1; }   /* prove sul Mac */
    port_used = 0;
    for (int i = 0; i < np && !port_used; i++) {
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)ports[i]);
        if (bind(lsock, (struct sockaddr *)&a, sizeof a) == 0) port_used = ports[i];
    }
    if (!port_used || listen(lsock, 8) != 0) { close(lsock); lsock = -1; return false; }
    unsigned char rb[20];
    random_bytes(rb, sizeof rb);
    snprintf(pin, sizeof pin, "%06u", (unsigned)((rb[0] << 16 | rb[1] << 8 | rb[2]) % 1000000));
    for (int i = 0; i < 16; i++) snprintf(token + 2 * i, sizeof token - 2 * (size_t)i, "%02x", rb[4 + i]);
    if (getenv("ARCADE_FS_PIN")) snprintf(pin, sizeof pin, "%.6s", getenv("ARCADE_FS_PIN"));   /* prove automatiche */
    bad_pins = 0;
    locked_until = 0;
    browsers_load();
    restore_remove_zip();   /* un ripristino lasciato a metà non si riprende: si ricarica dalla pagina */
    { char pz[1300]; full_path(pz, sizeof pz, RESTORE_ZIP ".part"); remove(pz); }
    atomic_store(&restore_go, false);
    atomic_store(&restore_drop, false);
    atomic_store(&restore_state, FS_RS_NONE);
    atomic_store(&stop_req, false);
    atomic_store(&any_change, false);
    memset(&st, 0, sizeof st);
    st.running = true;
    build_page();
    if (pthread_create(&thr, NULL, serve, NULL) != 0) { close(lsock); lsock = -1; st.running = false; return false; }
    thr_on = true;
    return true;
}

void fs_stop(void) {
    if (!thr_on) return;
    atomic_store(&stop_req, true);
    pthread_join(thr, NULL);
    thr_on = false;
    close(lsock);
    lsock = -1;
    token[0] = 0;
    pthread_mutex_lock(&mu);
    st.running = false;
    st.state = FS_IDLE;
    pthread_mutex_unlock(&mu);
}
