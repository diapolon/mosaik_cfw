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
static char root_dir[1024], base_url[512];
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
/* lo stesso degli aggiornamenti: System/update-url.txt se c'è, altrimenti quello del programma,
   con /download/testing al posto di /latest/download per il canale Sviluppo */
static void make_url(int development) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/update-url.txt", root_dir);
    FILE *f = fopen(p, "r");
    bool ok = f && fgets(base_url, sizeof base_url, f) != NULL;
    if (f) fclose(f);
    if (!ok) {
        snprintf(base_url, sizeof base_url, "%s", UPDATE_DEFAULT_URL);
        char *tail = development ? strstr(base_url, "/latest/download") : NULL;
        if (tail) strcpy(tail, "/download/testing");
    }
    base_url[strcspn(base_url, "\r\n")] = 0;
    size_t n = strlen(base_url);
    while (n && base_url[n - 1] == '/') base_url[--n] = 0;
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

static char **str_list(jval *arr, int *n) {
    *n = 0;
    int cap = 0;
    char **out = NULL;
    for (jval *v = arr && arr->t == J_ARR ? arr->child : NULL; v; v = v->next) {
        if (v->t != J_STR) continue;
        if (*n == cap) out = realloc(out, sizeof(char *) * (size_t)(cap = cap ? cap * 2 : 8));
        out[(*n)++] = strdup(v->s);
    }
    return out;
}

static bool parse_catalog(const char *text, entry_t **out, int *nout) {
    jval *j = json_parse(text);
    if (!j || json_int(j, "format", 0) != 1) { json_free(j); return false; }
    jval *arr = json_get(j, "packages");
    int n = 0, cap = 0;
    entry_t *e = NULL;
    bool ok = true;
    for (jval *v = arr && arr->t == J_ARR ? arr->child : NULL; v && ok; v = v->next) {
        if (n == cap) e = realloc(e, sizeof(entry_t) * (size_t)(cap = cap ? cap * 2 : 16));
        entry_t *x = &e[n];
        memset(x, 0, sizeof *x);
        snprintf(x->p.id, sizeof x->p.id, "%s", json_str(v, "id", ""));
        snprintf(x->p.name, sizeof x->p.name, "%s", json_str(v, "name", x->p.id));
        snprintf(x->p.label, sizeof x->p.label, "%s", json_str(v, "label", ""));
        snprintf(x->p.version, sizeof x->p.version, "%s", json_str(v, "version", ""));
        snprintf(x->p.file, sizeof x->p.file, "%s", json_str(v, "file", ""));
        snprintf(x->p.sha, sizeof x->p.sha, "%s", json_str(v, "sha256", ""));
        x->p.size = (long long)json_num(v, "size", 0);
        x->p.installed = (long long)json_num(v, "installed", 0);
        int ns = 0;
        char **sys = str_list(json_get(v, "systems"), &ns);
        for (int k = 0; k < ns; k++) {
            size_t l = strlen(x->p.systems);
            snprintf(x->p.systems + l, sizeof x->p.systems - l, "%s%s", k ? ", " : "", sys[k]);
            free(sys[k]);
        }
        free(sys);
        x->files = str_list(json_get(v, "files"), &x->nfiles);
        x->emus = str_list(json_get(v, "emulators"), &x->nemus);
        /* un nome di file o un percorso strani rendono falso tutto il catalogo */
        ok = x->p.id[0] && !strchr(x->p.id, '/') && strlen(x->p.sha) == 64 && x->p.size > 0 && x->nfiles > 0 &&
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
static void installed_version(const char *id, char *out, size_t n) {
    char p[1200], line[600];
    out[0] = 0;
    record_path(id, p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "version ", 8)) { line[strcspn(line, "\r\n")] = 0; snprintf(out, n, "%s", line + 8); break; }
    if (f) fclose(f);
}

static void compute_states(entry_t *e, int n) {
    for (int i = 0; i < n; i++) {
        char v[64];
        installed_version(e[i].p.id, v, sizeof v);
        if (v[0]) { e[i].p.state = strcmp(v, e[i].p.version) ? PKG_UPDATE : PKG_INSTALLED; continue; }
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

static void *fetch_worker(void *arg) {
    (void)arg;
    mem_t t = { malloc(1 << 20), 0, 1 << 20 }, s = { malloc(512), 0, 512 };
    bool got = t.buf && s.buf && get_mem("store.json", &t) && get_mem("store.sig", &s);
    entry_t *e = NULL;
    int n = 0;
    if (!got) st.error = SE_NETWORK;
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
static char **record_files(const char *id, int *n) {
    char p[1200], line[700];
    record_path(id, p, sizeof p);
    *n = 0;
    int cap = 0;
    char **out = NULL;
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "file ", 5) || !path_ok(line + 5)) continue;
        if (*n == cap) out = realloc(out, sizeof(char *) * (size_t)(cap = cap ? cap * 2 : 16));
        out[(*n)++] = strdup(line + 5);
    }
    if (f) fclose(f);
    return out;
}

static void free_list(char **l, int n) { for (int i = 0; i < n; i++) free(l[i]); free(l); }

/* Installa lo zip di un pacchetto: pacchetto.json dice id, versione e file. Prima si scrivono
   tutti i file come .part (e si controlla il CRC di ognuno), poi prendono il posto dei vecchi; i
   file della versione di prima che non ci sono più si tolgono. Un core che non si carica fa
   togliere il pacchetto. Il registro si scrive per ultimo. */
static int install_zip(const char *zpath, const char *want_id, char *name_out, size_t nn) {
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
    char **files = str_list(json_get(j, "files"), &nf);
    json_free(j);
    if (n < 0 || !id[0] || strchr(id, '/') || (want_id && strcmp(id, want_id)) || nf == 0) err = SE_CATALOG;
    for (int k = 0; !err && k < nf; k++) if (!path_ok(files[k])) err = SE_CATALOG;
    /* misura totale, per l'avanzamento */
    long long total = 0;
    int *idx = calloc(nf > 0 ? (size_t)nf : 1u, sizeof(int));
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
        return err;
    }
    for (int k = 0; k < nf; k++) {
        char full[1300], part[1400];
        snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
        part_name(full, part, sizeof part);
        if (rename(part, full) != 0) err = SE_WRITE;
    }
    sync();
    /* file della versione di prima che non ci sono più */
    int no = 0;
    char **old = record_files(id, &no);
    for (int i = 0; i < no; i++) {
        bool keep = false;
        for (int k = 0; k < nf && !keep; k++) keep = !strcmp(old[i], files[k]);
        if (keep) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", root_dir, old[i]);
        unlink(full);
        prune_dirs(old[i]);
    }
    free_list(old, no);
#ifdef __linux__
    for (int k = 0; !err && k < nf; k++) {
        size_t l = strlen(files[k]);
        if (strncmp(files[k], "Cores/", 6) || l < 4 || strcmp(files[k] + l - 3, ".so")) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
        if (!core_loads(full)) err = SE_CORE;
    }
#endif
    char rp[1200];
    record_path(id, rp, sizeof rp);
    if (err == SE_CORE) {   /* il core non parte: via tutto, meglio niente che un emulatore rotto */
        for (int k = 0; k < nf; k++) {
            char full[1300];
            snprintf(full, sizeof full, "%s/%s", root_dir, files[k]);
            unlink(full);
            prune_dirs(files[k]);
        }
        unlink(rp);
    } else if (!err) {
        size_t cap = 256 + (nf > 0 ? (size_t)nf : 0u) * 140, len = 0;
        char *rec = malloc(cap);
        len += (size_t)snprintf(rec + len, cap - len, "version %s\nlabel %s\n", version, label);
        for (int k = 0; k < nf; k++) len += (size_t)snprintf(rec + len, cap - len, "file %s\n", files[k]);
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/System/pacchetti", root_dir);
        mkdir(dir, 0755);
        if (!write_durable(rp, rec, len)) err = SE_WRITE;
        free(rec);
        ui_log("store: installato %s %s (%s, %d file)", id, label, version, nf);
    }
    atomic_store(&changed, true);
    free(idx);
    free_list(files, nf);
    return err;
}

static void *install_worker(void *arg) {
    (void)arg;
    entry_t e;
    pthread_mutex_lock(&lock);
    bool have = job >= 0 && job < ncat;
    if (have) e = cat[job];
    pthread_mutex_unlock(&lock);
    if (!have) { st.error = SE_CATALOG; st.phase = ST_FAILED; return NULL; }
    snprintf(st.working, sizeof st.working, "%s", e.p.name);
    /* spazio: lo zip più i file installati, con un margine */
    struct statvfs vs;
    if (statvfs(root_dir, &vs) == 0 && (long long)vs.f_bavail * (long long)vs.f_frsize < e.p.size + e.p.installed + (4 << 20)) {
        st.error = SE_SPACE; st.phase = ST_FAILED; return NULL;
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
        if (!c) { if (f) fclose(f); st.error = f ? SE_NETWORK : SE_WRITE; st.phase = ST_FAILED; return NULL; }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
        if (have_b) { curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have_b); st.done = have_b; }
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(e.p.size - have_b));
        CURLcode rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
        fclose(f);
        if (st.cancel) { st.error = SE_CANCEL; st.phase = ST_FAILED; return NULL; }
        if (rc == CURLE_RANGE_ERROR || rc == CURLE_FILESIZE_EXCEEDED) unlink(part);
        /* più grande del pacchetto del catalogo: non è quello */
        if (rc == CURLE_FILESIZE_EXCEEDED) { ui_log("store: %s più grande del catalogo", e.p.file); st.error = SE_VERIFY; st.phase = ST_FAILED; return NULL; }
        if (rc != CURLE_OK) { ui_log("store: %s: %s", e.p.file, curl_easy_strerror(rc)); st.error = SE_NETWORK; st.phase = ST_FAILED; return NULL; }
        if (!flushed) { st.error = SE_WRITE; st.phase = ST_FAILED; return NULL; }
        if (stat(part, &sb) != 0 || sb.st_size != e.p.size || !sha256_file(part, sha) || strcmp(sha, e.p.sha)) {
            unlink(part);
            ui_log("store: %s non corrisponde al catalogo", e.p.file);
            st.error = SE_VERIFY; st.phase = ST_FAILED; return NULL;
        }
        rename(part, zpath);
    }
    int err = install_zip(zpath, e.p.id, NULL, 0);
    unlink(zpath);
    store_refresh();
    st.error = err;
    st.phase = err ? ST_FAILED : ST_DONE;
    return NULL;
}

static void *remove_worker(void *arg) {
    (void)arg;
    entry_t e;
    pthread_mutex_lock(&lock);
    bool have = job >= 0 && job < ncat;
    if (have) e = cat[job];
    pthread_mutex_unlock(&lock);
    if (!have) { st.error = SE_CATALOG; st.phase = ST_FAILED; return NULL; }
    snprintf(st.working, sizeof st.working, "%s", e.p.name);
    int n = 0;
    char **files = record_files(e.p.id, &n);
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
    unlink(rp);
    sync();
    free_list(files, n);
    ui_log("store: tolto %s", e.p.id);
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
    st.phase = ST_WORKING;
    thread_on = pthread_create(&thread, NULL, fn, NULL) == 0;
    if (!thread_on) { st.error = SE_WRITE; st.phase = ST_FAILED; }
}

void store_install(int i) { start(i, install_worker); }
void store_remove(int i) { start(i, remove_worker); }

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
            installed_version(json_str(j, "id", ""), v, sizeof v);
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

/* Senza rete: se il catalogo sulla scheda conosce questa versione, lo zip deve avere la sua
   impronta (una copia rovinata non si installa); altrimenti vale la scelta di chi l'ha copiato. */
bool store_offline_install(const char *root, const char *path) {
    snprintf(root_dir, sizeof root_dir, "%s", root);
    mark_seen(path);
    load_cache();
    char sha[65];
    bool known_bad = false;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < ncat; i++)
        if (!strcmp(cat[i].p.file, b)) { known_bad = !sha256_file(path, sha) || strcmp(sha, cat[i].p.sha); break; }
    pthread_mutex_unlock(&lock);
    if (known_bad) { ui_log("store: %s non corrisponde al catalogo: non installato", b); return false; }
    st.cancel = false;
    int err = install_zip(path, NULL, NULL, 0);
    ui_log("store: %s dalla scheda: %s", b, err ? "non installato" : "installato");
    return err == SE_NONE;
}
