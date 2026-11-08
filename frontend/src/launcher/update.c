#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ui_log(const char *fmt, ...);

static update_status_t st;
static char current[64];

#if defined(TRIMUI_DEVICE) || defined(UPDATE_REAL)
#define sim_tick() ((void)0)
#else
static void sim_tick(void);
#endif

const update_status_t *update_status(void) { sim_tick(); return &st; }
void update_cancel(void) { st.cancel = true; }

void update_confirm_boot(const char *root) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/update-pending", root);
    if (unlink(p) == 0) ui_log("aggiornamento: il sistema nuovo funziona");
}

const char *update_current(void) {
    if (current[0]) return current;
#ifdef ARCADE_VERSION
    snprintf(current, sizeof current, "%s", ARCADE_VERSION);
#else
    snprintf(current, sizeof current, "?");
#endif
    FILE *f = fopen(getenv("ARCADE_VERSION_FILE") ? getenv("ARCADE_VERSION_FILE") : "/etc/arcade-version", "r");
    if (f) {
        char l[64];
        if (fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (l[0]) snprintf(current, sizeof current, "%s", l); }
        fclose(f);
    }
    return current;
}

#if defined(TRIMUI_DEVICE) || defined(UPDATE_REAL)
/* ------------------------------------------------------------------ console */
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "update_pubkey.h"

#ifndef UPDATE_DEFAULT_URL
#define UPDATE_DEFAULT_URL ""
#endif
#define MAX_FILES 2048
#define SYSFILE "System/arcade-system.squashfs"

static char root_dir[1024];
typedef struct { char sha[65]; long long size; char path[512]; bool need; int index; } item_t;
static item_t *items;
static int nitems;
static char base_url[256];
static int channel;
static pthread_t thread;
static bool thread_on;

bool update_supported(void) { const char *e = getenv("ARCADE_STANDALONE"); return e && *e == '1'; }

static void fail(int err) { st.error = err; st.phase = UPD_FAILED; }

/* indirizzo fisso: System/update-url.txt se c'è, altrimenti quello online scritto nel programma
   (file UPDATE_URL del progetto, per esempio quello delle Release di GitHub) */
static bool configured_url(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/update-url.txt", root_dir);
    FILE *f = fopen(p, "r");
    bool ok = f && fgets(base_url, sizeof base_url, f) != NULL;
    if (f) fclose(f);
    if (!ok) {
        snprintf(base_url, sizeof base_url, "%s", UPDATE_DEFAULT_URL);
        char *tail = channel ? strstr(base_url, "/latest/download") : NULL;
        if (tail) strcpy(tail, "/download/testing");
    }
    base_url[strcspn(base_url, "\r\n")] = 0;
    size_t n = strlen(base_url);
    while (n && base_url[n - 1] == '/') base_url[--n] = 0;
    return n > 0;
}

/* --------------------------------------------------------------------------- rete */
typedef struct { char *buf; size_t len, cap; } mem_t;

static size_t to_mem(char *p, size_t s, size_t n, void *u) {
    mem_t *m = u;
    size_t k = s * n;
    if (m->len + k >= m->cap) return 0;   /* troppo grande: non è un elenco */
    memcpy(m->buf + m->len, p, k);
    m->len += k;
    m->buf[m->len] = 0;
    return k;
}

static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)u; (void)a; (void)b; (void)c; (void)d;
    return st.cancel ? 1 : 0;
}

static CURL *new_curl(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    /* GitHub reindirizza i file delle Release; HTTPS con l'elenco di certificati della SD */
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ArcadeUpdate/1");
    char ca[1100];
    struct stat cs;
    snprintf(ca, sizeof ca, "%s/System/cacert.pem", root_dir);
    if (stat(ca, &cs) == 0) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    return c;
}

static bool get_mem(const char *name, mem_t *m) {
    char url[320];
    snprintf(url, sizeof url, "%s/%s", base_url, name);
    CURL *c = new_curl(url);
    if (!c) return false;
    m->len = 0;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

static size_t to_file(char *p, size_t s, size_t n, void *u) {
    size_t w = fwrite(p, s, n, u);
    if (w == n) st.done += (long long)(s * n);
    return w * s;
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

/* impronta SHA-256 di un file; con count i byte letti vanno nell'avanzamento */
static bool sha256_file(const char *path, char out[65], bool count) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    static unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && !st.cancel) {
        EVP_DigestUpdate(c, buf, n);
        if (count) st.done += (long long)n;
    }
    bool ok = !ferror(f) && !st.cancel;
    fclose(f);
    unsigned char md[32];
    unsigned ml = 0;
    EVP_DigestFinal_ex(c, md, &ml);
    EVP_MD_CTX_free(c);
    for (unsigned i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", md[i]);
    return ok;
}

/* ----------------------------------------------------------------- elenco dei file */
/* anche se l'elenco è firmato, si scrive solo nelle cartelle del sistema */
static bool path_ok(const char *p) {
    static const char *dirs[] = { "Lang/", "Emulators/", "Cores/", "Themes/", "Filters/", "Bezels/", "Dats/" };
    if (!*p || *p == '/' || strstr(p, "..") || strchr(p, '\\')) return false;
    /* dati scritti dallo scraper sulla console: non sono del sistema */
    if (!strcmp(p, "Dats/arcadedb.tsv") || !strncmp(p, "Dats/history/", 13)) return false;
    if (!strcmp(p, SYSFILE) || !strcmp(p, "System/cacert.pem")) return true;
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++)
        if (!strncmp(p, dirs[i], strlen(dirs[i]))) return true;
    return false;
}

static bool parse_manifest(char *text) {
    char *save = NULL, *line = strtok_r(text, "\n", &save);
    if (!line || strcmp(line, "arcade-update 1")) return false;
    nitems = 0;
    st.version[0] = 0;
    for (line = strtok_r(NULL, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "version ", 8)) { snprintf(st.version, sizeof st.version, "%s", line + 8); continue; }
        char sha[65], path[512];
        long long size;
        int skip = 0;
        if (sscanf(line, "file %64s %lld %n", sha, &size, &skip) != 2 || !skip) return false;
        snprintf(path, sizeof path, "%s", line + skip);
        if (strlen(sha) != 64 || size < 0 || !path_ok(path) || nitems >= MAX_FILES) return false;
        item_t *it = &items[nitems];
        memset(it, 0, sizeof *it);
        memcpy(it->sha, sha, 65);
        snprintf(it->path, sizeof it->path, "%s", path);
        it->size = size;
        it->index = nitems;
        nitems++;
    }
    return st.version[0] && nitems > 0;
}

static const char *stamp_of(const char *v) { const char *s = strchr(v, ' '); return s ? s + 1 : ""; }

static void *check_worker(void *arg) {
    (void)arg;
    st.phase = UPD_SEARCH;
    if (!configured_url()) { st.phase = UPD_NO_SERVER; return NULL; }
    ui_log("aggiornamento: server %s", base_url);
    st.phase = UPD_CHECK;
    static char text[1 << 20];
    static unsigned char sig[512];
    mem_t mt = { text, 0, sizeof text }, ms = { (char *)sig, 0, sizeof sig };
    if (!get_mem("update.txt", &mt) || !get_mem("update.sig", &ms)) { fail(UPE_NETWORK); return NULL; }
    if (!signature_ok(text, mt.len, sig, ms.len)) { ui_log("aggiornamento: firma non valida"); fail(UPE_SIGNATURE); return NULL; }
    if (!parse_manifest(text)) { ui_log("aggiornamento: elenco non valido"); fail(UPE_MANIFEST); return NULL; }
    /* solo versioni più recenti (le date di compilazione si confrontano come testo) */
    if (strcmp(stamp_of(st.version), stamp_of(update_current())) <= 0) { st.phase = UPD_CURRENT; return NULL; }
    long long hash_total = 0;
    for (int i = 0; i < nitems; i++) hash_total += items[i].size;
    st.done = 0;
    st.total = hash_total;
    long long need_bytes = 0;
    int need = 0;
    for (int i = 0; i < nitems && !st.cancel; i++) {
        item_t *it = &items[i];
        char p[1100], sha[65];
        struct stat s;
        snprintf(p, sizeof p, "%s/%s", root_dir, it->path);
        it->need = stat(p, &s) != 0 || s.st_size != it->size || !sha256_file(p, sha, true) || strcmp(sha, it->sha);
        if (it->need) { need++; need_bytes += it->size; }
    }
    if (st.cancel) { fail(UPE_CANCEL); return NULL; }
    if (!need) { st.phase = UPD_CURRENT; return NULL; }
    st.files = need;
    st.done = 0;
    st.total = need_bytes;
    ui_log("aggiornamento: disponibile %s, %d file, %lld byte", st.version, need, need_bytes);
    st.phase = UPD_AVAILABLE;
    return NULL;
}

static void mkdirs(char *path) {
    for (char *c = strchr(path + 1, '/'); c; c = strchr(c + 1, '/')) {
        *c = 0;
        mkdir(path, 0755);
        *c = '/';
    }
}

/* scarica un file con il nome provvisorio e lo rilegge dalla SD; true se l'impronta è giusta */
static bool fetch_one(const item_t *it, const char *tmp, int *err) {
    char url[320], sha[65];
    snprintf(url, sizeof url, "%s/file-%d", base_url, it->index);
    FILE *f = fopen(tmp, "wb");
    if (!f) { *err = UPE_WRITE; return false; }
    CURL *c = new_curl(url);
    if (!c) { fclose(f); *err = UPE_NETWORK; return false; }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    if (it->size > 0) curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)it->size);   /* non oltre la misura firmata */
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
    /* la rilettura deve venire dalla scheda, non dalla memoria */
    posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
    fclose(f);
    if (st.cancel) { *err = UPE_CANCEL; return false; }
    if (rc == CURLE_FILESIZE_EXCEEDED) { *err = UPE_VERIFY; return false; }   /* più grande del file firmato */
    if (rc != CURLE_OK) { *err = UPE_NETWORK; return false; }
    if (!flushed) { *err = UPE_WRITE; return false; }
    if (!sha256_file(tmp, sha, false) || strcmp(sha, it->sha)) { *err = UPE_VERIFY; return false; }
    return true;
}

static void tmp_name(const item_t *it, char *out, size_t n) { snprintf(out, n, "%s/%s.nuovo", root_dir, it->path); }

/* errore a metà: si rimette il sistema di prima. Se non riesce resta il .prec, che l'avvio sa usare */
static void restore_system(const char *prev, const char *dst) {
    if (rename(prev, dst) != 0) ui_log("aggiornamento: sistema precedente NON ripristinato (%s): resta %s", strerror(errno), prev);
}

static void remove_tmps(void) {
    char t[1100];
    for (int i = 0; i < nitems; i++) if (items[i].need) { tmp_name(&items[i], t, sizeof t); unlink(t); }
}

static void *install_worker(void *arg) {
    (void)arg;
    st.phase = UPD_DOWNLOAD;
    st.done = 0;
    struct statvfs vs;
    if (statvfs(root_dir, &vs) == 0 && (long long)vs.f_bavail * (long long)vs.f_frsize < st.total + (1 << 20)) { fail(UPE_SPACE); return NULL; }
    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (!it->need) continue;
        char tmp[1100];
        tmp_name(it, tmp, sizeof tmp);
        mkdirs(tmp);
        int err = UPE_NONE;
        long long start = st.done;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            st.done = start;
            ok = fetch_one(it, tmp, &err);
            /* solo una copia sbagliata si riprova: rete e spazio non migliorano subito */
            if (!ok && err != UPE_VERIFY) break;
            if (!ok) ui_log("aggiornamento: %s diverso dopo la scrittura, riprovo", it->path);
        }
        if (!ok) { ui_log("aggiornamento: %s non riuscito (%d)", it->path, err); remove_tmps(); fail(err); return NULL; }
    }
    /* tutti i file sono giusti: si aspetta la conferma, a schermo */
    st.phase = UPD_READY;
    return NULL;
}

/* si sostituiscono i file, il sistema per ultimo */
static void *apply_worker(void *arg) {
    (void)arg;
    char tmp[1100], dst[1100];
    item_t *sys = NULL;
    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (!it->need) continue;
        if (!strcmp(it->path, SYSFILE)) { sys = it; continue; }
        tmp_name(it, tmp, sizeof tmp);
        snprintf(dst, sizeof dst, "%s/%s", root_dir, it->path);
        if (rename(tmp, dst) != 0) { ui_log("aggiornamento: rename %s: %s", it->path, strerror(errno)); remove_tmps(); fail(UPE_WRITE); return NULL; }
    }
    if (sys) {
        char prev[1100], pend[1100];
        tmp_name(sys, tmp, sizeof tmp);
        snprintf(dst, sizeof dst, "%s/%s", root_dir, SYSFILE);
        snprintf(prev, sizeof prev, "%s.prec", dst);
        snprintf(pend, sizeof pend, "%s/System/update-pending", root_dir);
        /* il sistema in uso resta aperto (è montato) anche con un altro nome */
        unlink(prev);
        if (rename(dst, prev) != 0) { ui_log("aggiornamento: rename del sistema: %s", strerror(errno)); remove_tmps(); fail(UPE_WRITE); return NULL; }
        /* senza questo file non si contano gli avvii di prova: non si prosegue */
        FILE *f = fopen(pend, "w");
        bool marked = f && fputs("0\n", f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (f && fclose(f) != 0) marked = false;
        if (!marked) {
            ui_log("aggiornamento: impossibile scrivere update-pending");
            unlink(pend);
            restore_system(prev, dst);
            remove_tmps();
            fail(UPE_WRITE);
            return NULL;
        }
        if (rename(tmp, dst) != 0) {
            ui_log("aggiornamento: rename del sistema nuovo: %s", strerror(errno));
            restore_system(prev, dst);
            unlink(pend);
            remove_tmps();
            fail(UPE_WRITE);
            return NULL;
        }
    }
    sync();
    ui_log("aggiornamento: installata la versione %s", st.version);
    st.phase = UPD_DONE;
    return NULL;
}

void update_join(void) {
    if (thread_on) { pthread_join(thread, NULL); thread_on = false; }
}

void update_check(const char *root) {
    update_join();
    memset(&st, 0, sizeof st);
    snprintf(root_dir, sizeof root_dir, "%s", root);
    if (!items) items = malloc(sizeof(item_t) * MAX_FILES);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    st.phase = UPD_SEARCH;
    if (pthread_create(&thread, NULL, check_worker, NULL) == 0) thread_on = true;
    else fail(UPE_NETWORK);
}

void update_set_channel(int development) { channel = development; }

void update_install(void) {
    update_join();
    st.cancel = false;
    st.phase = UPD_DOWNLOAD;
    if (pthread_create(&thread, NULL, install_worker, NULL) == 0) thread_on = true;
    else fail(UPE_WRITE);
}

void update_apply(void) {
    update_join();
    st.phase = UPD_INSTALL;
    if (pthread_create(&thread, NULL, apply_worker, NULL) == 0) thread_on = true;
    else { remove_tmps(); fail(UPE_WRITE); }
}

void update_discard(void) {
    update_join();
    remove_tmps();
    fail(UPE_CANCEL);
}

#else
/* ------------------------------------------------------- Mac: percorso simulato */
#include <time.h>
static double sim_t0;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
bool update_supported(void) { return true; }
void update_join(void) {}
void update_set_channel(int development) { (void)development; }
void update_check(const char *root) {
    (void)root;
    memset(&st, 0, sizeof st);
    snprintf(st.version, sizeof st.version, "9.9.9 20991231.2359");
    st.files = 12;
    st.total = 64LL << 20;
    st.phase = UPD_AVAILABLE;
}
void update_install(void) { sim_t0 = now_s(); st.phase = UPD_DOWNLOAD; }
void update_apply(void) { st.phase = UPD_DONE; }
void update_discard(void) { st.error = UPE_CANCEL; st.phase = UPD_FAILED; }
static void sim_tick(void) {
    if (st.phase != UPD_DOWNLOAD) return;
    double t = now_s() - sim_t0;
    st.done = (long long)(st.total * (t / 2.0 > 1 ? 1 : t / 2.0));
    if (st.cancel) { st.error = UPE_CANCEL; st.phase = UPD_FAILED; }
    else if (t > 2.0) st.phase = UPD_READY;
}
#endif
