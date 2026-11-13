#include "store.h"
#include "zipread.h"
#include "update_pubkey.h"
#include "../common/json.h"
#include "../common/durable.h"
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <dlfcn.h>
#endif

void ui_log(const char *fmt, ...);

#ifndef UPDATE_DEFAULT_URL
#define UPDATE_DEFAULT_URL ""
#endif

typedef struct {
    store_pkg_t p;
    char **files, **emus;
    int nfiles, nemus;
} entry_t;

static store_status_t st;
static char root_dir[1024], base_url[512], fallback_url[512];
static entry_t *cat;
static int ncat;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool thread_on;
static atomic_bool changed;
static int job;   /* pacchetto del lavoro in corso */

const store_status_t *store_status(void) { return &st; }
void store_cancel(void) { st.cancel = true; }
bool store_changed(void) { return atomic_exchange(&changed, false); }

/* ------------------------------------------------------------------ indirizzo */
/* Il catalogo dello Store è uno solo, uguale per i canali Stabile e Sviluppo del sistema: sta
   nella Release "store" (.../releases/download/store), così un pacchetto nuovo arriva a tutti
   senza pubblicare un sistema. Se lì non si trova (rete, Release non ancora creata) si prova
   l'indirizzo del canale, dove resta una copia per i sistemi fino alla 0.9.44.
   System/update-url.txt, per le prove, vale per tutti e due e non ha riserva. */
static void trim_url(char *u) {
    u[strcspn(u, "\r\n")] = 0;
    size_t n = strlen(u);
    while (n && u[n - 1] == '/') u[--n] = 0;
}

static void make_url(int development) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/update-url.txt", root_dir);
    FILE *f = fopen(p, "r");
    bool ok = f && fgets(base_url, sizeof base_url, f) != NULL;
    if (f) fclose(f);
    fallback_url[0] = 0;
    if (!ok) {
        snprintf(fallback_url, sizeof fallback_url, "%s", UPDATE_DEFAULT_URL);
        snprintf(base_url, sizeof base_url, "%s", UPDATE_DEFAULT_URL);
        char *tail = development ? strstr(fallback_url, "/latest/download") : NULL;
        if (tail) strcpy(tail, "/download/testing");
        tail = strstr(base_url, "/latest/download");
        if (tail) strcpy(tail, "/download/store");
        else fallback_url[0] = 0;   /* indirizzo fatto in un altro modo: uno solo */
        trim_url(fallback_url);
    }
    trim_url(base_url);
}

/* ------------------------------------------------------------------ rete */
typedef struct { char *buf; size_t len, cap; } mem_t;

static size_t to_mem(char *p, size_t s, size_t n, void *u) {
    mem_t *m = u;
    size_t k = s * n;
    if (m->len + k + 1 > m->cap) return 0;
    memcpy(m->buf + m->len, p, k);
    m->len += k;
    m->buf[m->len] = 0;
    return k;
}

static size_t to_file(char *p, size_t s, size_t n, void *u) {
    size_t w = fwrite(p, s, n, u);
    if (w == n) st.done += (long long)(s * n);
    return w * s;
}

static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)u; (void)a; (void)b; (void)c; (void)d;
    return st.cancel ? 1 : 0;
}

static CURL *new_curl(const char *name) {
    char url[800];
    snprintf(url, sizeof url, "%s/%s", base_url, name);
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MosaikStore/1");
    char ca[1100];
    struct stat cs;
    snprintf(ca, sizeof ca, "%s/System/cacert.pem", root_dir);
    if (stat(ca, &cs) == 0) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    return c;
}

static bool get_mem(const char *name, mem_t *m) {
    CURL *c = new_curl(name);
    if (!c) return false;
    m->len = 0;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) ui_log("store: %s: %s", name, curl_easy_strerror(rc));
    return rc == CURLE_OK;
}

/* ------------------------------------------------------------- firma e impronte */
static bool signature_ok(const char *data, size_t len, const unsigned char *sig, size_t siglen) {
    BIO *b = BIO_new_mem_buf(UPDATE_PUBKEY, -1);
    EVP_PKEY *k = b ? PEM_read_bio_PUBKEY(b, NULL, NULL, NULL) : NULL;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    bool ok = k && c && EVP_DigestVerifyInit(c, NULL, EVP_sha256(), NULL, k) == 1 &&
              EVP_DigestVerifyUpdate(c, data, len) == 1 && EVP_DigestVerifyFinal(c, sig, siglen) == 1;
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    BIO_free(b);
    return ok;
}

static bool sha256_file(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c) { fclose(f); return false; }
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    unsigned char *buf = malloc(1 << 16);
    size_t n;
    bool ok = buf != NULL;
    while (ok && (n = fread(buf, 1, 1 << 16, f)) > 0 && !st.cancel) EVP_DigestUpdate(c, buf, n);
    ok = ok && !ferror(f) && !st.cancel;
    free(buf);
    fclose(f);
    unsigned char md[32];
    unsigned ml = 0;
    EVP_DigestFinal_ex(c, md, &ml);
    EVP_MD_CTX_free(c);
    for (unsigned i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", md[i]);
    return ok;
}

/* ------------------------------------------------------------------ catalogo */
static void free_entries(entry_t *e, int n) {
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < e[i].nfiles; k++) free(e[i].files[k]);
        for (int k = 0; k < e[i].nemus; k++) free(e[i].emus[k]);
        free(e[i].files);
        free(e[i].emus);
    }
    free(e);
}

/* solo nelle cartelle del sistema, mai fuori da Arcade */
static bool path_ok(const char *p) {
    static const char *dirs[] = { "Cores/", "Emulators/", "Dats/", "Apps/", "Themes/", "Filters/", "Bezels/" };
    if (!*p || *p == '/' || strstr(p, "..") || strchr(p, '\\') || strstr(p, "//")) return false;
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++)
        if (!strncmp(p, dirs[i], strlen(dirs[i])) && p[strlen(dirs[i])]) return true;
    return false;
}

static void free_list(char **l, int n);

/* elenco di stringhe del catalogo, tutto o niente: con la memoria esaurita *fail diventa vero e
   non si restituisce un elenco parziale (che aggiornando farebbe togliere file ancora del
   pacchetto: verifica dell'audit, FIX-R01) */
static char **str_list(jval *arr, int *n, bool *fail) {
    *n = 0;
    int cap = 0;
    char **out = NULL;
    for (jval *v = arr && arr->t == J_ARR ? arr->child : NULL; v; v = v->next) {
        if (v->t != J_STR) continue;
        if (*n == cap) {
            char **no = realloc(out, sizeof(char *) * (size_t)(cap ? cap * 2 : 8));
            if (!no) goto oom;
            out = no;
            cap = cap ? cap * 2 : 8;
        }
        if (!(out[*n] = strdup(v->s))) goto oom;
        (*n)++;
    }
    return out;
oom:
    free_list(out, *n);
    *n = 0;
    if (fail) *fail = true;
    return NULL;
}

static char lang_code[16] = "en";
static char system_version[24];
void store_system(const char *version) { snprintf(system_version, sizeof system_version, "%s", version ? version : ""); }

/* "0.9.37" contro "0.9.5": numero per numero */
static int version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) break;
    }
    return 0;
}

void store_lang(const char *lang) { snprintf(lang_code, sizeof lang_code, "%s", lang && lang[0] ? lang : "en"); }

/* testo di un pacchetto nella lingua della console ("texts": {"it": {"title": ...}}), altrimenti inglese */
static const char *pkg_text(jval *v, const char *field) {
    char k[64];
    snprintf(k, sizeof k, "texts.%s.%s", lang_code, field);
    const char *s = json_str(v, k, NULL);
    if (!s) { snprintf(k, sizeof k, "texts.en.%s", field); s = json_str(v, k, NULL); }
    return s;
}

static bool parse_catalog(const char *text, entry_t **out, int *nout) {
    jval *j = json_parse(text);
    if (!j || json_int(j, "format", 0) != 1) { json_free(j); return false; }
    jval *arr = json_get(j, "packages");
    int n = 0, cap = 0;
    entry_t *e = NULL;
    bool ok = true;
    for (jval *v = arr && arr->t == J_ARR ? arr->child : NULL; v && ok; v = v->next) {
        if (n == cap) {
            entry_t *ne = realloc(e, sizeof(entry_t) * (size_t)(cap ? cap * 2 : 16));
            if (!ne) { ok = false; break; }
            e = ne;
            cap = cap ? cap * 2 : 16;
        }
        entry_t *x = &e[n];
        memset(x, 0, sizeof *x);
        snprintf(x->p.id, sizeof x->p.id, "%s", json_str(v, "id", ""));
        snprintf(x->p.name, sizeof x->p.name, "%s", json_str(v, "name", x->p.id));
        snprintf(x->p.label, sizeof x->p.label, "%s", json_str(v, "label", ""));
        snprintf(x->p.version, sizeof x->p.version, "%s", json_str(v, "version", ""));
        snprintf(x->p.file, sizeof x->p.file, "%s", json_str(v, "file", ""));
        snprintf(x->p.sha, sizeof x->p.sha, "%s", json_str(v, "sha256", ""));
        x->p.size = (long long)json_num(v, "size", 0);
        /* cartelle: arcade, console, apps, linux; i cataloghi di prima avevano nintendo, sega, sony e other */
        const char *cg = json_str(v, "category", "console");
        snprintf(x->p.category, sizeof x->p.category, "%s",
                 !strcmp(cg, "arcade") || !strcmp(cg, "apps") || !strcmp(cg, "linux") || !strcmp(cg, "themes") || !strcmp(cg, "computer") || !strcmp(cg, "bezels") ? cg : "console");
        int nc = 0;
        bool coom = false;   /* senza colori il riquadro usa quello di riserva */
        char **cl = str_list(json_get(v, "colors"), &nc, &coom);
        for (int k = 0; k < nc; k++) {
            size_t l = strlen(x->p.colors);
            if (k < 3 && strlen(cl[k]) == 7 && cl[k][0] == '#') snprintf(x->p.colors + l, sizeof x->p.colors - l, "%s%s", l ? " " : "", cl[k] + 1);
            free(cl[k]);
        }
        free(cl);
        snprintf(x->p.title, sizeof x->p.title, "%s", json_str(v, "title", x->p.name));
        snprintf(x->p.emu, sizeof x->p.emu, "%s", json_str(v, "emu", ""));
        snprintf(x->p.art, sizeof x->p.art, "%s", json_str(v, "art", ""));
        snprintf(x->p.shortname, sizeof x->p.shortname, "%s", json_str(v, "short", ""));
        jval *cv = json_get(v, "console");
        x->p.console = cv && cv->t == J_BOOL && cv->n != 0;
        x->p.installed = (long long)json_num(v, "installed", 0);
        x->p.experimental = !strcmp(json_str(v, "status", ""), "experimental");
        snprintf(x->p.min_system, sizeof x->p.min_system, "%s", json_str(v, "min_system", ""));
        x->p.needs_system = x->p.min_system[0] && system_version[0] && version_cmp(system_version, x->p.min_system) < 0;
        {
            int nt = 0;
            bool toom = false;
            char **tl = str_list(json_get(v, "tested"), &nt, &toom);
            for (int k = 0; k < nt; k++) {
                size_t l = strlen(x->p.tested);
                snprintf(x->p.tested + l, sizeof x->p.tested - l, "%s%s", l ? " " : "", tl[k]);
                free(tl[k]);
            }
            free(tl);
        }
        int ns = 0;
        bool oom = false;
        char **sys = str_list(json_get(v, "systems"), &ns, &oom);
        for (int k = 0; k < ns; k++) {
            size_t l = strlen(x->p.systems);
            snprintf(x->p.systems + l, sizeof x->p.systems - l, "%s%s", k ? ", " : "", sys[k]);
            free(sys[k]);
        }
        free(sys);
        /* app: nome e descrizione tradotti (la descrizione prende il posto delle console) */
        const char *lt = pkg_text(v, "title"), *ld = pkg_text(v, "description");
        if (lt) { snprintf(x->p.title, sizeof x->p.title, "%s", lt); snprintf(x->p.name, sizeof x->p.name, "%s", lt); }
        if (ld) snprintf(x->p.systems, sizeof x->p.systems, "%s", ld);
        x->files = str_list(json_get(v, "files"), &x->nfiles, &oom);
        x->emus = str_list(json_get(v, "emulators"), &x->nemus, &oom);
        /* un nome di file o un percorso strani (o la memoria esaurita) rendono falso tutto il catalogo */
        ok = !oom && x->p.id[0] && !strchr(x->p.id, '/') && strlen(x->p.sha) == 64 && x->p.size > 0 && x->nfiles > 0 &&
             !strncmp(x->p.file, "store-", 6) && !strchr(x->p.file, '/');
        for (int k = 0; ok && k < x->nfiles; k++) ok = path_ok(x->files[k]);
        n++;
    }
    json_free(j);
    if (!ok) { free_entries(e, n); return false; }
    *out = e;
    *nout = n;
    return true;
}

static void record_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/System/pacchetti/%s.txt", root_dir, id); }

/* versione installata secondo il registro (System/pacchetti/<id>.txt), "" se non c'è */
static void installed_version(const char *id, char *out, size_t n, char *label, size_t ln) {
    char p[1200], line[600];
    out[0] = 0;
    if (label) label[0] = 0;
    record_path(id, p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "version ", 8)) snprintf(out, n, "%s", line + 8);
        else if (label && !strncmp(line, "label ", 6)) snprintf(label, ln, "%s", line + 6);
    }
    if (f) fclose(f);
}

static bool archived(const char *id) {
    char p[1200], line[64];
    record_path(id, p, sizeof p);
    FILE *f = fopen(p, "r");
    bool a = false;
    while (f && !a && fgets(line, sizeof line, f)) a = !strncmp(line, "archived", 8);
    if (f) fclose(f);
    return a;
}

static void compute_states(entry_t *e, int n) {
    for (int i = 0; i < n; i++) {
        char v[64];
        if (archived(e[i].p.id)) { e[i].p.state = PKG_ARCHIVED; continue; }
        char lab[32];
        installed_version(e[i].p.id, v, sizeof v, lab, sizeof lab);
        /* app e temi: conta il loro numero di versione (app.json, theme.json), non l'impronta dei
           file, che cambia anche solo ricompilando con la libreria comune aggiornata */
        bool by_label = (!strcmp(e[i].p.category, "apps") || !strcmp(e[i].p.category, "themes") || !strcmp(e[i].p.category, "bezels")) && lab[0] && e[i].p.label[0];
        if (v[0]) { e[i].p.state = (by_label ? strcmp(lab, e[i].p.label) : strcmp(v, e[i].p.version)) ? PKG_UPDATE : PKG_INSTALLED; continue; }
        /* arrivato con il sistema, prima dello Store: ci sono tutti i suoi emulatori */
        bool all = e[i].nemus > 0;
        for (int k = 0; all && k < e[i].nemus; k++) {
            char p[1200];
            struct stat sb;
            snprintf(p, sizeof p, "%s/Emulators/%s.json", root_dir, e[i].emus[k]);
            all = stat(p, &sb) == 0;
        }
        e[i].p.state = all ? PKG_OLD : PKG_ABSENT;
    }
}

void store_refresh(void) {
    pthread_mutex_lock(&lock);
    compute_states(cat, ncat);
    pthread_mutex_unlock(&lock);
}

static void set_catalog(entry_t *e, int n) {
    compute_states(e, n);
    pthread_mutex_lock(&lock);
    free_entries(cat, ncat);
    cat = e;
    ncat = n;
    pthread_mutex_unlock(&lock);
}

int store_count(void) {
    pthread_mutex_lock(&lock);
    int n = ncat;
    pthread_mutex_unlock(&lock);
    return n;
}

const store_pkg_t *store_pkg(int i) {
    static store_pkg_t copy;
    pthread_mutex_lock(&lock);
    if (i >= 0 && i < ncat) copy = cat[i].p;
    else memset(&copy, 0, sizeof copy);
    pthread_mutex_unlock(&lock);
    return &copy;
}

static char *read_file(const char *p, size_t max, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (!b || n == 0 || n == max) { free(b); return NULL; }
    b[n] = 0;
    if (len) *len = n;
    return b;
}

static bool write_durable(const char *path, const void *data, size_t n) {
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = false;
    if (!ok || durable_replace(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* catalogo tenuto sulla scheda, con la sua firma: si mostra anche senza rete */
static bool load_cache(void) {
    char p[1200], ps[1200];
    snprintf(p, sizeof p, "%s/System/store.json", root_dir);
    snprintf(ps, sizeof ps, "%s/System/store.sig", root_dir);
    size_t tl = 0, sl = 0;
    char *text = read_file(p, 1 << 20, &tl), *sig = read_file(ps, 512, &sl);
    entry_t *e = NULL;
    int n = 0;
    bool ok = text && sig && signature_ok(text, tl, (unsigned char *)sig, sl) && parse_catalog(text, &e, &n);
    if (ok) set_catalog(e, n);
    free(text);
    free(sig);
    return ok;
}

/* Catalogo e firma dalla rete. Prima store.cat, un file solo: "MOSAIK-STORE 1 <byte della firma>",
   a capo, la firma, il catalogo. Così chi pubblica sostituisce un file e la console non può mai
   ricevere il catalogo nuovo con la firma vecchia (13 nov: GitHub li ha serviti spaiati per due
   minuti). Se store.cat non c'è (Release fatte prima, prove) valgono store.json e store.sig. */
static bool fetch_pair(mem_t *t, mem_t *s) {
    mem_t c = { malloc((1 << 20) + 1024), 0, (1 << 20) + 1024 };
    bool got = false;
    if (c.buf && get_mem("store.cat", &c)) {
        const char *nl = memchr(c.buf, '\n', c.len < 64 ? c.len : 64);
        unsigned sl = 0;
        if (nl && sscanf(c.buf, "MOSAIK-STORE 1 %u", &sl) == 1 && sl > 0 && sl <= s->cap) {
            size_t head = (size_t)(nl - c.buf) + 1;
            if (c.len > head + sl && c.len - head - sl < t->cap) {
                memcpy(s->buf, c.buf + head, sl);
                s->len = sl;
                t->len = c.len - head - sl;
                memcpy(t->buf, c.buf + head + sl, t->len);
                t->buf[t->len] = 0;
                got = true;
            }
        }
        if (!got) ui_log("store: store.cat non valido, provo catalogo e firma separati");
    }
    free(c.buf);
    if (got) return true;
    t->len = s->len = 0;
    return get_mem("store.json", t) && get_mem("store.sig", s);
}

static void *fetch_worker(void *arg) {
    (void)arg;
    mem_t t = { malloc(1 << 20), 0, 1 << 20 }, s = { malloc(512), 0, 512 };
    bool got = t.buf && s.buf && fetch_pair(&t, &s);
    if (!got && t.buf && s.buf && fallback_url[0] && !st.cancel) {
        /* niente catalogo comune: quello del canale, da cui si scaricheranno anche i pacchetti */
        ui_log("store: catalogo comune non raggiunto, provo quello del canale");
        snprintf(base_url, sizeof base_url, "%s", fallback_url);
        fallback_url[0] = 0;
        got = fetch_pair(&t, &s);
    }
    entry_t *e = NULL;
    int n = 0;
    if (!got) { st.error = SE_NETWORK; ui_log("store: catalogo non scaricato"); }
    else if (!signature_ok(t.buf, t.len, (unsigned char *)s.buf, s.len)) { st.error = SE_SIGNATURE; got = false; ui_log("store: firma del catalogo non valida"); }
    else if (!parse_catalog(t.buf, &e, &n)) { st.error = SE_CATALOG; got = false; ui_log("store: catalogo non valido"); }
    if (got) {
        set_catalog(e, n);
        char p[1200];
        snprintf(p, sizeof p, "%s/System/store.json", root_dir);
        write_durable(p, t.buf, t.len);
        snprintf(p, sizeof p, "%s/System/store.sig", root_dir);
        write_durable(p, s.buf, s.len);
        st.online = true;
        st.error = SE_NONE;
        ui_log("store: catalogo con %d pacchetti", n);
    }
    free(t.buf);
    free(s.buf);
    st.phase = got || ncat ? ST_READY : ST_FAILED;
    return NULL;
}

void store_join(void) {
    if (!thread_on) return;
    pthread_join(thread, NULL);
    thread_on = false;
}

void store_open(const char *root, int development, bool online) {
    store_join();
    snprintf(root_dir, sizeof root_dir, "%s", root);
    make_url(development);
    memset((void *)&st, 0, sizeof st);
    bool cached = load_cache();
    ui_log("store: aperto %s, catalogo sulla scheda: %s", online ? "in rete" : "senza rete", cached ? "sì" : "no");
    if (!online) { st.phase = cached ? ST_READY : ST_FAILED; st.error = cached ? SE_NONE : SE_NETWORK; return; }
    st.phase = ST_LOADING;
    thread_on = pthread_create(&thread, NULL, fetch_worker, NULL) == 0;
    if (!thread_on) st.phase = cached ? ST_READY : ST_FAILED;
}

/* --------------------------------------------------------------- installazione */
static void mkparents(const char *full) {
    char t[1300];
    snprintf(t, sizeof t, "%s", full);
    for (char *p = t + strlen(root_dir) + 1; (p = strchr(p, '/')); p++) { *p = 0; mkdir(t, 0755); *p = '/'; }
}

/* ".nome.part" accanto al file */
static void part_name(const char *full, char *out, size_t n) {
    const char *b = strrchr(full, '/');
    snprintf(out, n, "%.*s/.%s.part", (int)(b - full), full, b + 1);
}

/* toglie le cartelle rimaste vuote fra il file e la cartella principale (Cores/deadcast/config) */
static void prune_dirs(const char *rel) {
    char t[1300];
    snprintf(t, sizeof t, "%s/%s", root_dir, rel);
    for (char *s = strrchr(t, '/'); s && s > t + strlen(root_dir); s = strrchr(t, '/')) {
        *s = 0;
        if (strchr(t + strlen(root_dir) + 1, '/') == NULL) break;   /* Cores, Emulators... restano */
        if (rmdir(t) != 0) break;
    }
}

#ifdef __linux__
static bool core_loads(const char *full) {
    void *h = dlopen(full, RTLD_NOW | RTLD_LOCAL);
    if (!h) { ui_log("store: %s non si carica: %s", full, dlerror()); return false; }
    unsigned (*api)(void) = (unsigned (*)(void))dlsym(h, "retro_api_version");
    bool ok = api && api() == 1;
    dlclose(h);
    if (!ok) ui_log("store: %s non è un core libretro valido", full);
    return ok;
}
#endif

/* file elencati nel registro di un pacchetto installato */
static char **record_files(const char *id, int *n, bool *fail) {
    char p[1200], line[700];
    record_path(id, p, sizeof p);
    *n = 0;
    int cap = 0;
    char **out = NULL;
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "file ", 5) || !path_ok(line + 5)) continue;
        if (*n == cap) {
            char **no = realloc(out, sizeof(char *) * (size_t)(cap ? cap * 2 : 16));
            if (!no) goto oom;
            out = no;
            cap = cap ? cap * 2 : 16;
        }
        if (!(out[*n] = strdup(line + 5))) goto oom;
        (*n)++;
    }
    if (f && ferror(f)) goto oom;   /* letto a metà per un errore della scheda: non è un inventario */
    if (f) fclose(f);
    return out;
oom:   /* un registro letto a metà non vale come inventario dei file */
    if (f) fclose(f);
    free_list(out, *n);
    *n = 0;
    if (fail) *fail = true;
    return NULL;
}

static void free_list(char **l, int n) { for (int i = 0; i < n; i++) free(l[i]); free(l); }

/* ---------------------------------------------- giornale dell'installazione
   Verifica audit BUG-004: la sostituzione dei file è una transazione, sul modello degli
   aggiornamenti. Prima di toccare qualcosa si scrive System/store-giornale.txt con ogni file
   ("sostituito", "nuovo", "tolto") e se c'era il registro; i file di prima diventano
   ".nome.annulla" accanto, i nuovi prendono il loro posto. Se qualcosa fallisce (un rename,
   il core che non si carica, il registro) o la corrente manca, si rimette tutto com'era:
   subito, o al prossimo avvio del launcher (store_recover). La riga "fatto" è il punto di non
   ritorno: dopo, si tolgono soltanto gli .annulla. */
#ifdef STORE_TEST
/* prove: STORE_FAIL_AT=N fa fallire l'N-esimo spostamento, STORE_STOP_AT=N esce lì (corrente) */
static int tx_step;
static int tx_rename(const char *a, const char *b) {
    tx_step++;
    const char *e = getenv("STORE_STOP_AT");
    if (e && atoi(e) == tx_step) _exit(3);
    e = getenv("STORE_FAIL_AT");
    if (e && atoi(e) == tx_step) { errno = EIO; return -1; }
    return rename(a, b);
}
#else
#define tx_rename rename
#endif

static void journal_path(char *out, size_t n) { snprintf(out, n, "%s/System/store-giornale.txt", root_dir); }

static void annulla_name(const char *full, char *out, size_t n) {
    const char *b = strrchr(full, '/');
    snprintf(out, n, "%.*s/.%s.annulla", (int)(b - full), full, b + 1);
}

static bool exists(const char *p) { struct stat sb; return lstat(p, &sb) == 0; }

/* rimette com'era (done = false) o chiude la transazione (done = true); si può ripetere */
static bool journal_apply(const char *text, bool done) {
    char id[40] = "", rp[1200] = "", rpa[1300] = "";
    bool ok = true, had_record = false, record_line = false;
    const char *l = text;
    while (*l) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[800];
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), l);
        l += len + (e ? 1 : 0);
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *what = line, *arg = sp + 1;
        if (!strcmp(what, "pacchetto")) {
            if (!arg[0] || strchr(arg, '/') || strlen(arg) >= sizeof id) return false;
            snprintf(id, sizeof id, "%s", arg);
            record_path(id, rp, sizeof rp);
            snprintf(rpa, sizeof rpa, "%s.annulla", rp);
            continue;
        }
        if (!strcmp(what, "registro")) { record_line = true; had_record = !strcmp(arg, "si"); continue; }
        if (!path_ok(arg)) continue;
        char full[1300], part[1400], ann[1400];
        snprintf(full, sizeof full, "%s/%s", root_dir, arg);
        part_name(full, part, sizeof part);
        annulla_name(full, ann, sizeof ann);
        bool repl = !strcmp(what, "sostituito"), gone = !strcmp(what, "tolto"), fresh = !strcmp(what, "nuovo");
        if (!repl && !gone && !fresh) continue;
        unlink(part);
        if (done) {
            if (repl || gone) unlink(ann);
            if (gone) prune_dirs(arg);
        } else if (fresh) {
            if (unlink(full) != 0 && errno != ENOENT) ok = false;
            prune_dirs(arg);
        } else if (exists(ann) && rename(ann, full) != 0) {
            ui_log("store: non riesco a rimettere %s", arg);
            ok = false;
        }
    }
    if (!id[0] || !record_line) return false;
    if (done) unlink(rpa);
    else if (exists(rpa)) { if (rename(rpa, rp) != 0) ok = false; }
    else if (!had_record) unlink(rp);
    return ok;
}


/* Il giornale è intero? Intestazione, riga del registro e, da quando c'è, il conto delle righe
   dei file ("righe N"); quelli scritti prima non l'hanno e valgono se hanno il resto. */
static bool journal_valid(const char *t) {
    if (strncmp(t, "pacchetto ", 10)) return false;
    int files = 0, want = -1;
    bool reg = false;
    for (const char *l = t; *l;) {
        const char *e = strchr(l, '\n');
        if (!e) return false;   /* ogni riga finisce con un a capo: l'ultima è tronca */
        if (!strncmp(l, "sostituito ", 11) || !strncmp(l, "nuovo ", 6) || !strncmp(l, "tolto ", 6)) files++;
        else if (!strncmp(l, "registro ", 9)) reg = true;
        else if (!strncmp(l, "righe ", 6)) want = atoi(l + 6);
        l = e + 1;
    }
    return reg && (want < 0 || want == files);
}

/* Lettura del giornale con tre esiti (audit A38-003): assente, letto per intero, errore. Un
   errore di lettura non dice niente sul contenuto: il giornale resta dov'è. */
enum { JR_ABSENT, JR_OK, JR_ERROR };
static int journal_read(const char *p, char **out) {
    *out = NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return errno == ENOENT ? JR_ABSENT : JR_ERROR;
    size_t cap = 1 << 20, n = 0;
    char *b = malloc(cap + 1);
    if (b) n = fread(b, 1, cap, f);
    bool bad = !b || ferror(f) || (n == cap && fgetc(f) != EOF);
    fclose(f);
    if (bad) { free(b); return JR_ERROR; }
    b[n] = 0;
    *out = b;
    return JR_OK;
}

/* Al primo avvio dopo un'installazione interrotta (launcher, prima di leggere i giochi) e prima di
   ogni nuova installazione o eliminazione. true: nessuna transazione in sospeso. Il giornale si
   toglie solo dopo averlo applicato; finché resta, lo Store non scrive altro (le .annulla che
   elenca sono l'unica copia dei file di prima). */
bool store_recover(const char *root) {
    if (root != root_dir) snprintf(root_dir, sizeof root_dir, "%s", root);   /* da dentro lo Store: è già lei */
    char jp[1200];
    journal_path(jp, sizeof jp);
    char *t = NULL;
    int r = journal_read(jp, &t);
    if (r == JR_ABSENT) return true;
    if (r == JR_ERROR) {
        ui_log("store: giornale dell'installazione illeggibile (%s): resta, Store fermo finché non si legge", strerror(errno));
        return false;
    }
    /* il giornale si scrive tutto insieme (write_durable): senza la riga "pacchetto" non è uno
       scritto a metà ma un file guasto. Si mette da parte, intero, per capire cos'è successo */
    if (!journal_valid(t)) {
        /* Non si può applicare, e non si può sapere cosa era già stato scritto (un pacchetto nuovo
           non lascia copie dei file di prima): lo Store resta fermo finché non lo decide l'utente
           con store_recover_force (verifica audit R40-02) */
        ui_log("store: giornale dell'installazione guasto: resta, Store fermo");
        free(t);
        return false;
    }
    bool done = strstr(t, "\nfatto\n") != NULL, ok = journal_apply(t, done);
    if (ok) {
        sync();
        ok = unlink(jp) == 0 || errno == ENOENT;
        sync();
        ui_log("store: installazione interrotta %s", done ? "conclusa" : "annullata, rimessi i file di prima");
    } else ui_log("store: giornale dell'installazione non risolto, si riprova al prossimo avvio; Store fermo");
    free(t);
    atomic_store(&changed, true);
    return ok;
}

/* Scelta dell'utente ("Prosegui lo stesso"): il giornale che non si riesce ad applicare si mette
   da parte, intero, in store-giornale-guasto.txt. I file a metà restano come sono: il pacchetto
   coinvolto va installato di nuovo. false se non si riesce nemmeno a spostarlo. */
bool store_recover_force(const char *root) {
    if (root != root_dir) snprintf(root_dir, sizeof root_dir, "%s", root);
    char jp[1200], kept[1300];
    journal_path(jp, sizeof jp);
    snprintf(kept, sizeof kept, "%s/System/store-giornale-guasto.txt", root_dir);
    bool moved = rename(jp, kept) == 0 || errno == ENOENT;
    ui_log("store: giornale messo da parte per scelta dell'utente: %s", moved ? "fatto" : strerror(errno));
    sync();
    return moved;
}

/* Installa lo zip di un pacchetto: pacchetto.json dice id, versione e file. Prima si scrivono
   tutti i file come .part (e si controlla il CRC di ognuno), poi prendono il posto dei vecchi con
   il giornale qui sopra; i file della versione di prima che non ci sono più si tolgono. Un core
   che non si carica fa rimettere la versione di prima (o niente, se non c'era). */
static int install_zip(const char *zpath, const char *want_id, char *name_out, size_t nn) {
    if (!store_recover(root_dir)) return SE_WRITE;   /* un'installazione di prima non è chiusa */
    FILE *z = fopen(zpath, "rb");
    if (!z) return SE_WRITE;
    zent_t *ze = NULL;
    int n = zip_dir(z, &ze), err = SE_NONE;
    char meta[65536];
    int mi = -1;
    for (int i = 0; i < n; i++) if (!strcmp(ze[i].name, "pacchetto.json")) mi = i;
    jval *j = mi >= 0 && zip_entry(z, &ze[mi], NULL, meta, sizeof meta) ? json_parse(meta) : NULL;
    char id[32], version[24], label[32];
    snprintf(id, sizeof id, "%s", json_str(j, "id", ""));
    snprintf(version, sizeof version, "%s", json_str(j, "version", ""));
    snprintf(label, sizeof label, "%s", json_str(j, "label", ""));
    if (name_out) snprintf(name_out, nn, "%s", json_str(j, "name", id));
    int nf = 0;
    bool oom = false;
    char **files = str_list(json_get(j, "files"), &nf, &oom);
    json_free(j);
    /* l'elenco dei file della versione installata, prima di toccare qualcosa */
    int no = 0;
    char **old = oom ? NULL : record_files(id, &no, &oom);
    if (oom) err = SE_WRITE;
    if (n < 0 || !id[0] || strchr(id, '/') || (want_id && strcmp(id, want_id)) || nf == 0) err = SE_CATALOG;
    for (int k = 0; !err && k < nf; k++) if (!path_ok(files[k])) err = SE_CATALOG;
    for (int k = 0; !err && k < nf; k++)   /* lo stesso file due volte rovinerebbe la copia di prima */
        for (int m = k + 1; !err && m < nf; m++) if (!strcmp(files[k], files[m])) err = SE_CATALOG;
    /* misura totale, per l'avanzamento */
    long long total = 0;
    int *idx = calloc(nf > 0 ? (size_t)nf : 1u, sizeof(int));
    if (!idx) err = SE_WRITE;
    for (int k = 0; !err && k < nf; k++) {
        idx[k] = -1;
        for (int i = 0; i < n; i++) if (!strcmp(ze[i].name, files[k])) { idx[k] = i; break; }
        if (idx[k] < 0) err = SE_CATALOG;
        else total += ze[idx[k]].usize;
    }
    st.done = 0;
    st.total = total;
    int written = 0;
    for (int k = 0; !err && k < nf && !st.cancel; k++) {
        char full[1300], part[1400];
        snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
        mkparents(full);
        part_name(full, part, sizeof part);
        FILE *o = fopen(part, "wb");
        bool ok = o && zip_entry(z, &ze[idx[k]], o, NULL, 0) && fflush(o) == 0 && fsync(fileno(o)) == 0;
        if (o && fclose(o) != 0) ok = false;
        if (!ok) { unlink(part); err = o ? SE_VERIFY : SE_WRITE; break; }
        /* script e programmi (Deadcast, app): eseguibili */
        size_t l = strlen(files[k]);
        if (l > 3 && (!strcmp(files[k] + l - 3, ".sh") || !strchr(strrchr(files[k], '/'), '.'))) chmod(part, 0755);
        st.done += ze[idx[k]].usize;
        written = k + 1;
    }
    if (st.cancel && !err) err = SE_CANCEL;
    fclose(z);
    free(ze);
    if (err) {   /* i .part scritti si tolgono: i file di prima restano com'erano */
        for (int k = 0; k < written; k++) {
            char full[1300], part[1400];
            snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
            part_name(full, part, sizeof part);
            unlink(part);
        }
        free(idx);
        free_list(files, nf);
        free_list(old, no);
        return err;
    }
    char rp[1200], rpa[1300], jp[1200];
    record_path(id, rp, sizeof rp);
    snprintf(rpa, sizeof rpa, "%s.annulla", rp);
    journal_path(jp, sizeof jp);
    char sysd[1200];
    snprintf(sysd, sizeof sysd, "%s/System/pacchetti", root_dir);
    mkdir(sysd, 0755);
    /* il giornale: tutto quello che si farà, prima di farlo */
    size_t jcap = 96 + strlen(id), jlen = 0;
    for (int k = 0; k < nf; k++) jcap += strlen(files[k]) + 16;
    for (int i = 0; i < no; i++) jcap += strlen(old[i]) + 16;
    char *jt = malloc(jcap);
    bool *drop = calloc(no > 0 ? (size_t)no : 1u, sizeof(bool));
    if (!jt || !drop) err = SE_WRITE;
    bool had_record = exists(rp);
    if (!err) {
        jlen += (size_t)snprintf(jt + jlen, jcap - jlen, "pacchetto %s\n", id);
        for (int k = 0; k < nf; k++) {
            char full[1300];
            snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
            jlen += (size_t)snprintf(jt + jlen, jcap - jlen, "%s %s\n", exists(full) ? "sostituito" : "nuovo", files[k]);
        }
        for (int i = 0; i < no; i++) {   /* file della versione di prima che non ci sono più */
            bool keep = false;
            for (int k = 0; k < nf && !keep; k++) keep = !strcmp(old[i], files[k]);
            if (keep) continue;
            drop[i] = true;
            jlen += (size_t)snprintf(jt + jlen, jcap - jlen, "tolto %s\n", old[i]);
        }
        jlen += (size_t)snprintf(jt + jlen, jcap - jlen, "registro %s\n", had_record ? "si" : "no");
        /* quante righe di file ha il giornale: uno letto a pezzi non si applica (R39-02) */
        int rows = 0;
        for (const char *c = jt; (c = strchr(c, '\n')); c++) rows++;
        jlen += (size_t)snprintf(jt + jlen, jcap - jlen, "righe %d\n", rows - 2);
        if (!write_durable(jp, jt, jlen)) err = SE_WRITE;
    }
    bool journaled = !err;
    /* i file di prima diventano .annulla, i nuovi prendono il loro posto */
    for (int k = 0; !err && k < nf; k++) {
        char full[1300], part[1400], ann[1400];
        snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
        part_name(full, part, sizeof part);
        annulla_name(full, ann, sizeof ann);
        if (exists(full) && tx_rename(full, ann) != 0) err = SE_WRITE;
        else if (tx_rename(part, full) != 0) err = SE_WRITE;
    }
    for (int i = 0; !err && i < no; i++) {
        if (!drop[i]) continue;
        char full[1300], ann[1400];
        snprintf(full, sizeof full, "%s/%s", root_dir, old[i]);
        annulla_name(full, ann, sizeof ann);
        if (exists(full) && tx_rename(full, ann) != 0) err = SE_WRITE;
    }
    if (!err) sync();
#ifdef __linux__
    for (int k = 0; !err && k < nf; k++) {
        size_t l = strlen(files[k]);
        if (strncmp(files[k], "Cores/", 6) || l < 4 || strcmp(files[k] + l - 3, ".so")) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
        if (!core_loads(full)) err = SE_CORE;
    }
#endif
    if (!err) {
        /* misura vera del registro: ogni percorso intero (audit BUG-002: 140 byte a riga non
           bastavano e il conto andava oltre il buffer) */
        size_t cap = strlen(version) + strlen(label) + 32, len = 0;
        for (int k = 0; k < nf; k++) cap += strlen(files[k]) + 8;
        char *rec = malloc(cap);
        if (!rec) err = SE_WRITE;
        int w = rec ? snprintf(rec, cap, "version %s\nlabel %s\n", version, label) : -1;
        if (w < 0 || (size_t)w >= cap) err = SE_WRITE; else len = (size_t)w;
        for (int k = 0; !err && k < nf; k++) {
            w = snprintf(rec + len, cap - len, "file %s\n", files[k]);
            if (w < 0 || (size_t)w >= cap - len) err = SE_WRITE; else len += (size_t)w;
        }
        if (!err && had_record && tx_rename(rp, rpa) != 0) err = SE_WRITE;
        if (!err && !write_durable(rp, rec, len)) err = SE_WRITE;
        free(rec);
    }
    /* il punto di non ritorno: "fatto" nel giornale */
    if (!err) {
        char *jd = realloc(jt, jlen + 8);
        if (jd) { jt = jd; memcpy(jt + jlen, "fatto\n", 7); }
        if (!jd || !write_durable(jp, jt, jlen + 6)) err = SE_WRITE;
    }
    if (journaled) {
        jt[jlen] = 0;
        if (journal_apply(jt, !err)) { sync(); unlink(jp); sync(); }
        else ui_log("store: %s non rimesso del tutto, si riprova al prossimo avvio", id);
    } else {   /* niente giornale: niente è stato toccato, si tolgono i .part */
        for (int k = 0; k < nf; k++) {
            char full[1300], part[1400];
            snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
            part_name(full, part, sizeof part);
            unlink(part);
        }
    }
    if (err) ui_log("store: %s non installato (errore %d), file di prima rimessi", id, err);
    else ui_log("store: installato %s %s (%s, %d file)", id, label, version, nf);
    free(jt);
    free(drop);
    free_list(old, no);
    atomic_store(&changed, true);
    free(idx);
    free_list(files, nf);
    return err;
}

/* scarica e installa il pacchetto job del catalogo; SE_* */
static int install_job(void) {
    entry_t e;
    pthread_mutex_lock(&lock);
    bool have = job >= 0 && job < ncat;
    if (have) e = cat[job];
    pthread_mutex_unlock(&lock);
    if (!have) return SE_CATALOG;
    if (e.p.needs_system) return SE_SYSTEM;   /* il pacchetto chiede un sistema più nuovo di questo */
    snprintf(st.working, sizeof st.working, "%s", e.p.name);
    /* spazio: lo zip più i file installati, con un margine */
    struct statvfs vs;
    if (statvfs(root_dir, &vs) == 0 && (long long)vs.f_bavail * (long long)vs.f_frsize < e.p.size + e.p.installed + (4 << 20)) {
        return SE_SPACE;
    }
    char dir[1200], zpath[1400], part[1500], sha[65];
    snprintf(dir, sizeof dir, "%s/System/.store", root_dir);
    mkdir(dir, 0755);
    snprintf(zpath, sizeof zpath, "%s/%s", dir, e.p.file);
    snprintf(part, sizeof part, "%s.part", zpath);
    st.total = e.p.size;
    st.done = 0;
    struct stat sb;
    bool ready = stat(zpath, &sb) == 0 && sb.st_size == e.p.size && sha256_file(zpath, sha) && !strcmp(sha, e.p.sha);
    if (!ready) {
        long long have_b = stat(part, &sb) == 0 && sb.st_size < e.p.size ? sb.st_size : 0;
        FILE *f = fopen(part, have_b ? "ab" : "wb");
        CURL *c = f ? new_curl(e.p.file) : NULL;
        if (!c) { if (f) fclose(f); return f ? SE_NETWORK : SE_WRITE; }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
        if (have_b) { curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have_b); st.done = have_b; }
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(e.p.size - have_b));
        CURLcode rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
        fclose(f);
        if (st.cancel) { return SE_CANCEL; }
        if (rc == CURLE_RANGE_ERROR || rc == CURLE_FILESIZE_EXCEEDED) unlink(part);
        /* più grande del pacchetto del catalogo: non è quello */
        if (rc == CURLE_FILESIZE_EXCEEDED) { ui_log("store: %s più grande del catalogo", e.p.file); return SE_VERIFY; }
        if (rc != CURLE_OK) { ui_log("store: %s: %s", e.p.file, curl_easy_strerror(rc)); return SE_NETWORK; }
        if (!flushed) { return SE_WRITE; }
        if (stat(part, &sb) != 0 || sb.st_size != e.p.size || !sha256_file(part, sha) || strcmp(sha, e.p.sha)) {
            unlink(part);
            ui_log("store: %s non corrisponde al catalogo", e.p.file);
            return SE_VERIFY;
        }
        rename(part, zpath);
    }
    int err = install_zip(zpath, e.p.id, NULL, 0);
    unlink(zpath);
    store_refresh();
    return err;
}

static void *install_worker(void *arg) {
    (void)arg;
    int err = install_job();
    st.error = err;
    st.phase = err ? ST_FAILED : ST_DONE;
    return NULL;
}

/* ---------------------------------------------- pacchetti di un backup ripristinato
   Il backup ricorda i pacchetti installati (mosaik-backup.txt); il ripristino li scrive in
   System/ripristino-pacchetti.txt, uno per riga, e il launcher propone di rimetterli dallo Store
   tutti insieme. Restano nel file quelli non riusciti, per la volta dopo. */
#define RESTORE_PKGS "System/ripristino-pacchetti.txt"

static int restore_ids(char ids[][32], int max) {
    char p[1200], line[64];
    snprintf(p, sizeof p, "%s/" RESTORE_PKGS, root_dir);
    FILE *f = fopen(p, "r");
    int n = 0;
    while (f && n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n ")] = 0;
        if (!line[0] || strchr(line, '/') || strlen(line) >= 32) continue;
        char rp[1200];
        record_path(line, rp, sizeof rp);
        if (!access(rp, F_OK) && !archived(line)) continue;   /* già installato */
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(ids[k], line);
        if (!dup) snprintf(ids[n++], 32, "%s", line);
    }
    if (f) fclose(f);
    return n;
}

static void restore_save(char ids[][32], int n) {
    char p[1200];
    snprintf(p, sizeof p, "%s/" RESTORE_PKGS, root_dir);
    if (!n) { unlink(p); return; }
    char buf[64 * 33];
    size_t len = 0;
    for (int k = 0; k < n; k++) len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", ids[k]);
    write_durable(p, buf, len);
}

int store_restore_pending(const char *root) {
    snprintf(root_dir, sizeof root_dir, "%s", root);
    char ids[64][32];
    int n = restore_ids(ids, 64);
    if (!n) restore_save(ids, 0);
    return n;
}

static void *restore_worker(void *arg) {
    (void)arg;
    char ids[64][32], left[64][32];
    int n = restore_ids(ids, 64), nl = 0, last = SE_NONE;
    st.batch_total = n;
    for (int k = 0; k < n; k++) {
        st.batch_done = k;
        int i = -1;
        pthread_mutex_lock(&lock);
        for (int c = 0; c < ncat && i < 0; c++) if (!strcmp(cat[c].p.id, ids[k])) i = c;
        pthread_mutex_unlock(&lock);
        if (i < 0) { ui_log("store: %s del backup non è nel catalogo di questo canale", ids[k]); continue; }
        if (st.cancel) { snprintf(left[nl++], 32, "%s", ids[k]); last = SE_CANCEL; continue; }
        job = i;
        st.done = st.total = 0;
        int err = install_job();
        ui_log("store: dal backup %s: %s", ids[k], err ? "non riuscito" : "installato");
        if (err) { snprintf(left[nl++], 32, "%s", ids[k]); last = err; }
    }
    restore_save(left, nl);
    st.batch_total = 0;
    st.error = last;
    st.phase = last ? ST_FAILED : ST_DONE;
    return NULL;
}


/* toglie una cartella e tutto quello che c'è dentro; un collegamento non si segue */
static void rm_tree(const char *dir) {
    struct stat ds;
    if (lstat(dir, &ds) != 0) return;
    if (!S_ISDIR(ds.st_mode)) { unlink(dir); return; }
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1500];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        rm_tree(p);
    }
    if (d) closedir(d);
    rmdir(dir);
}

/* file e byte sotto una cartella (senza seguire i collegamenti), saltando i nascosti */
static void count_tree(const char *dir, int *files, long long *bytes, int depth) {
    DIR *d = depth < 8 ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1500];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) count_tree(p, files, bytes, depth + 1);
        else if (S_ISREG(sb.st_mode)) { (*files)++; if (bytes) *bytes += sb.st_size; }
    }
    if (d) closedir(d);
}

/* cartella dei giochi di una console (solo dentro Roms) e se è una console */
static bool emu_roms(const char *id, char *roms, size_t n, bool *console) {
    char p[1300], up[32], def[48];
    size_t k = 0;
    for (; id[k] && k < sizeof up - 1; k++) up[k] = (char)(id[k] >= 'a' && id[k] <= 'z' ? id[k] - 32 : id[k]);
    up[k] = 0;
    snprintf(def, sizeof def, "Roms/%s", up);
    snprintf(p, sizeof p, "%s/Emulators/%s.json", root_dir, id);
    jval *j = json_load(p);
    snprintf(roms, n, "%s", json_str(j, "roms", def));
    /* archiviato: il file dell'emulatore non c'è più, si deduce dal nome */
    *console = j ? !strcmp(json_str(j, "kind", ""), "console") : strcmp(id, "fbneo") && strncmp(id, "mame", 4);
    json_free(j);
    return !strncmp(roms, "Roms/", 5) && roms[5] && !strstr(roms, "..") && !strchr(roms + 5, '/');
}

void store_usage(int i, store_usage_t *u) {
    memset(u, 0, sizeof *u);
    pthread_mutex_lock(&lock);
    entry_t e = i >= 0 && i < ncat ? cat[i] : (entry_t){ 0 };
    pthread_mutex_unlock(&lock);
    static const char *types[] = { "snap", "title", "marquee", "box" };
    for (int k = 0; k < e.nemus; k++) {
        char roms[300], p[1300];
        bool console;
        if (emu_roms(e.emus[k], roms, sizeof roms, &console)) {
            snprintf(p, sizeof p, "%s/%s", root_dir, roms);
            count_tree(p, &u->games, &u->games_bytes, 0);
        }
        snprintf(p, sizeof p, "%s/Saves/%s", root_dir, e.emus[k]);
        count_tree(p, &u->saves, &u->saves_bytes, 0);
        for (int t = 0; console && t < 4; t++) { snprintf(p, sizeof p, "%s/Media/%s/%s", root_dir, types[t], e.emus[k]); count_tree(p, &u->media, NULL, 0); }
    }
}

static bool remove_data;

/* giochi, salvataggi e immagini di una console del pacchetto: prima di togliere il suo
   Emulators/<id>.json, che dice dov'è la cartella dei giochi */
static void remove_emu_data(const char *id) {
    char roms[300], p[1300];
    bool console;
    /* solo una cartella dentro Roms: un percorso assoluto scelto dall'utente non si tocca */
    if (emu_roms(id, roms, sizeof roms, &console)) {
        snprintf(p, sizeof p, "%s/%s", root_dir, roms);
        rm_tree(p);
        mkdir(p, 0755);   /* la cartella vuota resta, pronta per altri giochi */
    }
    snprintf(p, sizeof p, "%s/Saves/%s", root_dir, id);
    rm_tree(p);
    snprintf(p, sizeof p, "%s/System/chiavi/%s.tsv", root_dir, id);   /* e il registro di chi usava quei salvataggi */
    unlink(p);
    if (console) {   /* le immagini degli arcade sono per nome del gioco, comuni a più emulatori */
        static const char *types[] = { "snap", "title", "marquee", "box" };
        for (int t = 0; t < 4; t++) { snprintf(p, sizeof p, "%s/Media/%s/%s", root_dir, types[t], id); rm_tree(p); }
    }
    ui_log("store: tolti giochi, salvataggi e immagini di %s (%s)", id, roms);
}

static void *remove_worker(void *arg) {
    (void)arg;
    entry_t e;
    pthread_mutex_lock(&lock);
    bool have = job >= 0 && job < ncat;
    if (have) e = cat[job];
    pthread_mutex_unlock(&lock);
    if (!have) { st.error = SE_CATALOG; st.phase = ST_FAILED; return NULL; }
    if (!store_recover(root_dir)) { st.error = SE_WRITE; st.phase = ST_FAILED; return NULL; }
    snprintf(st.working, sizeof st.working, "%s", e.p.name);
    int n = 0;
    bool oom = false;
    char **files = record_files(e.p.id, &n, &oom);
    if (oom) {   /* senza l'elenco intero non si toglie niente, nemmeno i dati */
        st.error = SE_WRITE;
        st.phase = ST_FAILED;
        return NULL;
    }
    if (remove_data)
        for (int k = 0; k < e.nemus; k++) remove_emu_data(e.emus[k]);
    /* senza registro (arrivato con il sistema): i file del catalogo */
    char **list = n ? files : e.files;
    int ln = n ? n : e.nfiles;
    for (int i = 0; i < ln; i++) {
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", root_dir, list[i]);
        if (unlink(full) == 0 || errno == ENOENT) prune_dirs(list[i]);
    }
    char rp[1200];
    record_path(e.p.id, rp, sizeof rp);
    if (remove_data) unlink(rp);
    else {   /* archiviato: lo Store lo ricorda, giochi e salvataggi restano */
        char dir[1200], rec[200];
        snprintf(dir, sizeof dir, "%s/System/pacchetti", root_dir);
        mkdir(dir, 0755);
        int len = snprintf(rec, sizeof rec, "archived\nlabel %s\n", e.p.label);
        write_durable(rp, rec, (size_t)len);
    }
    sync();
    free_list(files, n);
    ui_log("store: %s %s", remove_data ? "eliminato" : "archiviato", e.p.id);
    atomic_store(&changed, true);
    store_refresh();
    st.phase = ST_DONE;
    return NULL;
}

static void start(int i, void *(*fn)(void *)) {
    store_join();
    job = i;
    st.cancel = false;
    st.error = SE_NONE;
    st.done = st.total = 0;
    st.working[0] = 0;
    st.batch_done = st.batch_total = 0;
    st.phase = ST_WORKING;
    thread_on = pthread_create(&thread, NULL, fn, NULL) == 0;
    if (!thread_on) { st.error = SE_WRITE; st.phase = ST_FAILED; }
}

void store_install(int i) { start(i, install_worker); }
void store_install_restore(void) { start(-1, restore_worker); }
void store_remove(int i, bool with_data) { remove_data = with_data; start(i, remove_worker); }

/* ------------------------------------------------------- pacchetti copiati sulla scheda */
static void seen_key(const char *path, char *out, size_t n) {
    struct stat sb;
    const char *b = strrchr(path, '/');
    snprintf(out, n, "%s\t%lld", b ? b + 1 : path, stat(path, &sb) == 0 ? (long long)sb.st_size : 0LL);
}

static bool seen(const char *key) {
    char p[1200], line[600];
    snprintf(p, sizeof p, "%s/System/store-visti.txt", root_dir);
    FILE *f = fopen(p, "r");
    bool found = false;
    while (f && !found && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; found = !strcmp(line, key); }
    if (f) fclose(f);
    return found;
}

static void mark_seen(const char *path) {
    char p[1200], key[600];
    seen_key(path, key, sizeof key);
    snprintf(p, sizeof p, "%s/System/store-visti.txt", root_dir);
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%s\n", key); fflush(f); fsync(fileno(f)); fclose(f); }
}

bool store_offline_find(const char *root, char *path, int pn, char *name, int nn, char *label, int ln) {
    snprintf(root_dir, sizeof root_dir, "%s", root);
    char dirs[2][1100];
    snprintf(dirs[0], sizeof dirs[0], "%s", root);
    snprintf(dirs[1], sizeof dirs[1], "%s", root);
    char *sl = strrchr(dirs[1], '/');
    if (sl && sl != dirs[1]) *sl = 0;
    else snprintf(dirs[1], sizeof dirs[1], ".");
    for (int d = 0; d < 2; d++) {
        DIR *dp = opendir(dirs[d]);
        struct dirent *de;
        while (dp && (de = readdir(dp))) {
            size_t l = strlen(de->d_name);
            if (strncmp(de->d_name, "store-", 6) || l < 11 || strcasecmp(de->d_name + l - 4, ".zip")) continue;
            char full[1300], key[600];
            snprintf(full, sizeof full, "%s/%s", dirs[d], de->d_name);
            seen_key(full, key, sizeof key);
            if (seen(key)) continue;
            FILE *z = fopen(full, "rb");
            zent_t *ze = NULL;
            int n = z ? zip_dir(z, &ze) : -1;
            char meta[65536];
            jval *j = NULL;
            for (int i = 0; i < n && !j; i++)
                if (!strcmp(ze[i].name, "pacchetto.json") && zip_entry(z, &ze[i], NULL, meta, sizeof meta)) j = json_parse(meta);
            free(ze);
            if (z) fclose(z);
            char v[64];
            installed_version(json_str(j, "id", ""), v, sizeof v, NULL, 0);
            bool fresh = j && json_str(j, "id", "")[0] && strcmp(v, json_str(j, "version", ""));
            if (fresh) {
                snprintf(path, (size_t)pn, "%s", full);
                snprintf(name, (size_t)nn, "%s", json_str(j, "name", json_str(j, "id", "")));
                snprintf(label, (size_t)ln, "%s", json_str(j, "label", ""));
            } else mark_seen(full);   /* non è un pacchetto, o è già installato così */
            json_free(j);
            if (fresh) { closedir(dp); return true; }
        }
        if (dp) closedir(dp);
    }
    return false;
}

/* impronta di un pacchetto in un catalogo firmato: quello tenuto sulla scheda, oppure
   store.json e store.sig copiati nella stessa cartella dello zip (scheda nuova senza rete).
   Falso se nessun catalogo firmato elenca quel file. */
static bool signed_sha(const char *zip_path, char *sha_out) {
    const char *b = strrchr(zip_path, '/');
    char dir[1100];
    snprintf(dir, sizeof dir, "%.*s", b ? (int)(b - zip_path) : 1, b ? zip_path : ".");
    b = b ? b + 1 : zip_path;
    char cands[2][2][1300];
    snprintf(cands[0][0], sizeof cands[0][0], "%s/System/store.json", root_dir);
    snprintf(cands[0][1], sizeof cands[0][1], "%s/System/store.sig", root_dir);
    snprintf(cands[1][0], sizeof cands[1][0], "%s/store.json", dir);
    snprintf(cands[1][1], sizeof cands[1][1], "%s/store.sig", dir);
    for (int c = 0; c < 2; c++) {
        size_t tl = 0, sl = 0;
        char *text = read_file(cands[c][0], 1 << 20, &tl), *sig = read_file(cands[c][1], 512, &sl);
        entry_t *e = NULL;
        int n = 0;
        bool ok = text && sig && signature_ok(text, tl, (unsigned char *)sig, sl) && parse_catalog(text, &e, &n);
        bool found = false;
        for (int i = 0; ok && i < n && !found; i++)
            if (!strcmp(e[i].p.file, b)) { snprintf(sha_out, 65, "%s", e[i].p.sha); found = true; }
        if (ok) free_entries(e, n);
        free(text);
        free(sig);
        if (found) return true;
    }
    return false;
}

/* Senza rete: si installa solo un pacchetto elencato da un catalogo firmato e con la stessa
   impronta (verifica audit: prima uno sconosciuto si installava senza controlli, e un'app o un
   core girano come root) */
int store_offline_install(const char *root, const char *path) {
    snprintf(root_dir, sizeof root_dir, "%s", root);
    char want[65], sha[65];
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    /* rifiutato per mancanza del catalogo: non segnato come visto, così copiando accanto
       store.json e store.sig all'avvio dopo si propone di nuovo */
    if (!signed_sha(path, want)) { ui_log("store: %s non è in un catalogo firmato: rifiutato", b); return SE_UNSIGNED; }
    mark_seen(path);
    if (!sha256_file(path, sha) || strcmp(sha, want)) { ui_log("store: %s non corrisponde al catalogo: non installato", b); return SE_VERIFY; }
    st.cancel = false;
    int err = install_zip(path, NULL, NULL, 0);
    ui_log("store: %s dalla scheda: %s", b, err ? "non installato" : "installato");
    return err;
}
