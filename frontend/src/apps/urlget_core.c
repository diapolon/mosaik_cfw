/* Scarica file da un indirizzo (vedi urlget_core.h). */
#include "urlget_core.h"
#include "../common/durable.h"
#include "../common/json.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static bool test_mode(void) {
    const char *e = getenv("URLGET_TEST");
    return e && *e == '1';
}

/* --------------------------------------------------------------- indirizzi */

static bool starts_ci(const char *s, const char *p) { return strncasecmp(s, p, strlen(p)) == 0; }

bool ug_url_ok(const char *url) {
    if (!url || strlen(url) > 2000) return false;
    for (const unsigned char *c = (const unsigned char *)url; *c; c++)
        if (*c <= ' ' || *c == 0x7f) return false;
    const char *rest;
    if (starts_ci(url, "https://")) rest = url + 8;
    else if (test_mode() && starts_ci(url, "http://")) rest = url + 7;
    else return false;
    size_t al = strcspn(rest, "/?#");
    if (al == 0 || memchr(rest, '@', al)) return false;   /* senza nome o con utente:password@ */
    return true;
}

static bool id_ok(const char *s, size_t n) {
    if (n == 0 || n > 120) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '_' && s[i] != '-') return false;
    return s[0] != '.';
}

int ug_classify(const char *url, ug_link_t *o) {
    memset(o, 0, sizeof *o);
    if (!ug_url_ok(url)) return UG_BAD;
    bool https = starts_ci(url, "https://");
    const char *auth = url + (https ? 8 : 7);
    size_t al = strcspn(auth, "/?#");
    char host[160];
    if (al >= sizeof host) return UG_BAD;
    snprintf(host, sizeof host, "%.*s", (int)al, auth);
    const char *path = auth + al;
    size_t pl = strcspn(path, "?#");
    o->kind = UG_FILE;
    bool archive = !strcasecmp(host, "archive.org") || !strcasecmp(host, "www.archive.org") || (test_mode() && !https);
    if (!archive) return UG_FILE;
    const char *kinds[] = { "/download/", "/details/", "/metadata/" };
    for (int k = 0; k < 3; k++) {
        size_t kl = strlen(kinds[k]);
        if (pl < kl || strncmp(path, kinds[k], kl)) continue;
        const char *id = path + kl;
        size_t il = (size_t)strcspn(id, "/?#");
        if (!id_ok(id, il)) return o->kind = UG_BAD;
        /* /download/<id>/<file>: un file diretto, come un altro indirizzo */
        bool tail = k == 0 && id[il] == '/' && id + il + 1 < path + pl;
        if (tail) return UG_FILE;
        snprintf(o->origin, sizeof o->origin, "%s://%s", https ? "https" : "http", !strcasecmp(host, "www.archive.org") ? "archive.org" : host);
        snprintf(o->item, sizeof o->item, "%.*s", (int)il, id);
        snprintf(o->meta, sizeof o->meta, "%s/metadata/%s", o->origin, o->item);
        return o->kind = UG_ITEM;
    }
    return UG_FILE;
}

bool ug_safe_name(const char *path, char *out, size_t n) {
    if (!path || n < 2) return false;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char t[UG_NAME_MAX * 2 + 8];
    size_t l = 0;
    for (const unsigned char *c = (const unsigned char *)b; *c && l < sizeof t - 1; c++) {
        unsigned char ch = *c;
        if (ch < 0x20 || ch == 0x7f || strchr("<>:\"|?*\\", ch)) ch = '_';
        t[l++] = (char)ch;
    }
    while (l && (t[l - 1] == ' ' || t[l - 1] == '.')) l--;   /* il FAT non li tiene */
    t[l] = 0;
    if (!l || t[0] == '.') return false;
    if (l > UG_NAME_MAX) {
        /* si taglia il nome, non l'estensione, e mai a metà di un carattere UTF-8 */
        const char *dot = strrchr(t, '.');
        size_t el = dot && l - (size_t)(dot - t) <= 16 ? l - (size_t)(dot - t) : 0;
        size_t keep = UG_NAME_MAX - el;
        while (keep > 0 && ((unsigned char)t[keep] & 0xC0) == 0x80) keep--;
        memmove(t + keep, t + l - el, el);
        l = keep + el;
        t[l] = 0;
    }
    if (l >= n) return false;
    memcpy(out, t, l + 1);
    return true;
}

static void pct_encode(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c && o + 4 < n; c++) {
        if (isalnum(*c) || strchr("-._~/", *c)) out[o++] = (char)*c;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", *c);
    }
    out[o] = 0;
}

void ug_item_url(const ug_link_t *l, const char *path, char *out, size_t n) {
    char enc[1600];
    pct_encode(path, enc, sizeof enc);
    snprintf(out, n, "%s/download/%s/%s", l->origin, l->item, enc);
}

bool ug_url_name(const char *url, char *out, size_t n) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    p += strcspn(p, "/?#");
    size_t pl = strcspn(p, "?#");
    char dec[600];
    size_t o = 0;
    for (size_t i = 0; i < pl && o < sizeof dec - 1; i++) {
        if (p[i] == '%' && i + 2 < pl && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
            char h[3] = { p[i + 1], p[i + 2], 0 };
            dec[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else dec[o++] = p[i];
    }
    dec[o] = 0;
    return ug_safe_name(dec, out, n);
}

/* --------------------------------------------------------------- metadati */

static long long jsize(jval *f) {
    jval *s = json_get(f, "size");
    if (!s) return 0;
    if (s->t == J_NUM) return s->n > 0 && s->n < 1e15 ? (long long)s->n : 0;
    if (s->t == J_STR && s->s) { long long v = atoll(s->s); return v > 0 ? v : 0; }
    return 0;
}

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

static bool hex_ok(const char *s, size_t n) {
    if (strlen(s) != n) return false;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return false;
    return true;
}

int ug_parse_meta(const char *json, ug_file_t **out) {
    *out = NULL;
    jval *j = json ? json_parse(json) : NULL;
    if (!j) return 0;
    jval *files = json_get(j, "files");
    int n = 0, cap = 0;
    if (files && files->t == J_ARR) {
        for (jval *f = files->child; f && n < 20000; f = f->next) {
            const char *name = json_str(f, "name", "");
            const char *src = json_str(f, "source", "original");
            const char *fmt = json_str(f, "format", "");
            if (!*name || strlen(name) > 500 || strcmp(src, "original") || !strcmp(fmt, "Metadata")) continue;
            if (!strcmp(json_str(f, "private", ""), "true")) continue;
            if (ends_with(name, "_meta.xml") || ends_with(name, "_files.xml") || ends_with(name, "_meta.sqlite")) continue;
            if (name[0] == '/' || strstr(name, "../") || strstr(name, "//") || ends_with(name, "/..")) continue;
            ug_file_t t;
            memset(&t, 0, sizeof t);
            if (!ug_safe_name(name, t.name, sizeof t.name)) continue;
            snprintf(t.path, sizeof t.path, "%s", name);
            t.size = jsize(f);
            const char *h = json_str(f, "sha1", "");
            if (hex_ok(h, 40)) snprintf(t.sha1, sizeof t.sha1, "%s", h);
            h = json_str(f, "md5", "");
            if (hex_ok(h, 32)) snprintf(t.md5, sizeof t.md5, "%s", h);
            if (n == cap) {
                int nc = cap ? cap * 2 : 64;
                ug_file_t *nv = realloc(*out, sizeof *nv * (size_t)nc);
                if (!nv) { free(*out); *out = NULL; json_free(j); return 0; }
                *out = nv;
                cap = nc;
            }
            (*out)[n++] = t;
        }
    }
    json_free(j);
    return n;
}

/* --------------------------------------------------------------- rete */

static bool v4_blocked(uint32_t a) {   /* a in ordine di rete -> host */
    a = ntohl(a);
    return (a >> 24) == 0 || (a >> 24) == 10 || (a >> 24) == 127 || (a >> 24) >= 224 ||
           (a >> 16) == 0xA9FE || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 22) == (100u << 2 | 1);
}

bool ug_addr_blocked(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
        if (test_mode() && (ntohl(s->sin_addr.s_addr) >> 24) == 127) return false;
        return v4_blocked(s->sin_addr.s_addr);
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
        const unsigned char *b = s->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) { uint32_t v; memcpy(&v, b + 12, 4); return v4_blocked(v); }
        if (IN6_IS_ADDR_LOOPBACK(&s->sin6_addr)) return !test_mode();
        return IN6_IS_ADDR_UNSPECIFIED(&s->sin6_addr) || (b[0] & 0xFE) == 0xFC || (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) || b[0] == 0xFF;
    }
    return true;   /* famiglie sconosciute: no */
}

static curl_socket_t open_sock(void *u, curlsocktype purpose, struct curl_sockaddr *a) {
    (void)u; (void)purpose;
    if (ug_addr_blocked((const struct sockaddr *)&a->addr)) return CURL_SOCKET_BAD;
    return socket(a->family, a->socktype, a->protocol);
}

typedef struct {
    ug_progress_fn cb;
    void *ud;
    bool cancelled;
    /* a memoria */
    char *mem;
    size_t len, max;
    bool toobig;
    /* su file */
    FILE *f;
    long long got;
    EVP_MD_CTX *sha1, *md5;
    bool werr;
    /* spazio: si scrive al massimo budget byte, e ogni 4 MB si ricontrolla lo spazio libero, che
       può calare mentre si scarica; expect > 0 è la misura dichiarata (audit Codex 0.9.100, A100-05) */
    const char *dir;
    long long budget, next_check, expect;
    bool nospace, toolong;
} xfer_t;

static int on_xfer(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ut; (void)un;
    xfer_t *x = u;
    if (x->cb && x->cb(x->ud, dn, dt)) { x->cancelled = true; return 1; }
    return 0;
}

static CURL *new_curl(const char *url, const char *cacert, xfer_t *x) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    const char *protos = test_mode() ? "http,https" : "https";
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mosaik-urlget/1");
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, protos);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, protos);   /* nemmeno un reindirizzamento scende a HTTP */
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_PROXY, "");   /* niente proxy dell'ambiente: il controllo degli indirizzi vale per tutti */
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, open_sock);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_xfer);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, x);
    struct stat sb;
    if (cacert && stat(cacert, &sb) == 0) curl_easy_setopt(c, CURLOPT_CAINFO, cacert);
    return c;
}

static size_t to_mem(char *p, size_t s, size_t n, void *u) {
    xfer_t *x = u;
    size_t k = s * n;
    if (x->len + k > x->max) { x->toobig = true; return 0; }
    char *nm = realloc(x->mem, x->len + k + 1);
    if (!nm) return 0;
    x->mem = nm;
    memcpy(x->mem + x->len, p, k);
    x->len += k;
    x->mem[x->len] = 0;
    return k;
}

static int net_result(CURLcode rc, xfer_t *x) {
    if (rc == CURLE_OK) return UG_OK;
    if (x->nospace) return UG_E_SPACE;
    if (x->toolong) return UG_E_SIZE;
    if (x->cancelled || rc == CURLE_ABORTED_BY_CALLBACK) return UG_E_CANCEL;
    if (x->toobig || rc == CURLE_FILESIZE_EXCEEDED) return UG_E_BIG;
    if (rc == CURLE_UNSUPPORTED_PROTOCOL || rc == CURLE_URL_MALFORMAT) return UG_E_URL;
    return UG_E_NET;
}

int ug_fetch(const char *url, const char *cacert, size_t max, char **out, ug_progress_fn cb, void *ud) {
    *out = NULL;
    if (!ug_url_ok(url)) return UG_E_URL;
    xfer_t x = { .cb = cb, .ud = ud, .max = max };
    CURL *c = new_curl(url, cacert, &x);
    if (!c) return UG_E_NET;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    int r = net_result(rc, &x);
    if (r != UG_OK) { free(x.mem); return r; }
    if (!x.mem) { x.mem = calloc(1, 1); if (!x.mem) return UG_E_IO; }
    *out = x.mem;
    return UG_OK;
}

/* --------------------------------------------------------------- file */

#define UG_RESERVE (16LL << 20)   /* spazio che resta sempre libero sulla scheda */
static size_t to_file(char *p, size_t s, size_t n, void *u) {
    xfer_t *x = u;
    size_t k = s * n;
    if (x->got + (long long)k > UG_FILE_MAX) { x->toobig = true; return 0; }
    if (x->expect > 0 && x->got + (long long)k > x->expect) { x->toolong = true; return 0; }   /* più grande del dichiarato */
    if (x->got + (long long)k > x->budget) { x->nospace = true; return 0; }
    if (x->got + (long long)k >= x->next_check) {
        struct statvfs vs;
        if (statvfs(x->dir, &vs) != 0 || (long long)vs.f_bavail * (long long)vs.f_frsize < (long long)k + UG_RESERVE) { x->nospace = true; return 0; }
        x->next_check = x->got + (4LL << 20);
    }
    if (fwrite(p, 1, k, x->f) != k) { x->werr = true; return 0; }
    EVP_DigestUpdate(x->sha1, p, k);
    EVP_DigestUpdate(x->md5, p, k);
    x->got += (long long)k;
    return k;
}

static void hex(const unsigned char *d, unsigned n, char *out) {
    for (unsigned i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

int ug_download(const char *url, const char *dir, const char *name, long long size,
                const char *sha1, const char *md5, const char *cacert, ug_progress_fn cb, void *ud) {
    if (!ug_url_ok(url)) return UG_E_URL;
    if (!name || !*name || name[0] == '.' || strchr(name, '/') || strlen(name) > UG_NAME_MAX) return UG_E_IO;
    if (size > UG_FILE_MAX) return UG_E_BIG;
    /* spazio: se non si può misurare non si scarica; con la misura ignota (size 0) vale il budget, non
       solo il controllo iniziale, che vedeva 0 byte e lasciava consumare anche la riserva */
    struct statvfs vs;
    if (statvfs(dir, &vs) != 0) return UG_E_SPACE;
    long long avail = (long long)vs.f_bavail * (long long)vs.f_frsize;
    if (avail < size + UG_RESERVE) return UG_E_SPACE;
    char full[1400], part[1500];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    snprintf(part, sizeof part, "%s/.%s.part", dir, name);
    struct stat sb;
    if (stat(full, &sb) == 0) return UG_E_EXISTS;
    xfer_t x = { .cb = cb, .ud = ud, .dir = dir, .budget = avail - UG_RESERVE, .next_check = 4LL << 20, .expect = size };
    x.f = fopen(part, "wb");
    if (!x.f) return UG_E_IO;
    x.sha1 = EVP_MD_CTX_new();
    x.md5 = EVP_MD_CTX_new();
    int res = UG_E_IO;
    CURL *c = NULL;
    if (x.sha1 && x.md5 && EVP_DigestInit_ex(x.sha1, EVP_sha1(), NULL) == 1 && EVP_DigestInit_ex(x.md5, EVP_md5(), NULL) == 1 &&
        (c = new_curl(url, cacert, &x))) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)UG_FILE_MAX);
        CURLcode rc = curl_easy_perform(c);
        res = x.werr ? UG_E_IO : net_result(rc, &x);
    }
    if (c) curl_easy_cleanup(c);
    if (res == UG_OK) {
        if (size > 0 && x.got != size) res = UG_E_SIZE;
        else {
            unsigned char d[32];
            unsigned dl = 0;
            char h[80];
            if (sha1 && *sha1) { EVP_DigestFinal_ex(x.sha1, d, &dl); hex(d, dl, h); if (strcasecmp(h, sha1)) res = UG_E_HASH; }
            if (res == UG_OK && md5 && *md5) { EVP_DigestFinal_ex(x.md5, d, &dl); hex(d, dl, h); if (strcasecmp(h, md5)) res = UG_E_HASH; }
        }
    }
    if (res == UG_OK && (fflush(x.f) != 0 || fsync(fileno(x.f)) != 0)) res = UG_E_IO;
    if (fclose(x.f) != 0 && res == UG_OK) res = UG_E_IO;
    EVP_MD_CTX_free(x.sha1);
    EVP_MD_CTX_free(x.md5);
    if (res == UG_OK && durable_replace(part, full) != 0) res = UG_E_IO;
    if (res != UG_OK) remove(part);
    return res;
}
