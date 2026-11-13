#include "update.h"
#include "games.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

void ui_log(const char *fmt, ...);


/* generazione dell'area di avvio: della scheda (System/area-avvio.txt, scritto dall'initramfs;
   le schede scritte prima della 0.9.17 non lo hanno: 0) e, in *latest, delle immagini di questa
   versione (/etc/area-avvio nel sistema; sul Mac ARCADE_BOOT_GEN, altrimenti quella della scheda) */
static int read_int(const char *p, int dflt) {
    FILE *f = fopen(p, "r");
    int v = dflt;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = dflt; fclose(f); }
    return v;
}
int update_boot_gen(const char *root, int *latest) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/area-avvio.txt", root);
    int card = read_int(p, 0);
    if (latest) {
        const char *e = getenv("ARCADE_BOOT_GEN");
        *latest = e ? atoi(e) : read_int("/etc/area-avvio", card);
    }
    return card;
}

static update_status_t st;
static char current[64];

#if defined(TRIMUI_DEVICE) || defined(UPDATE_REAL)
#define sim_tick() ((void)0)
#else
static void sim_tick(void);
#endif

const update_status_t *update_status(void) { sim_tick(); return &st; }
void update_cancel(void) { st.cancel = true; }

/* Giornale dell'aggiornamento (audit R03, issue #3): System/update-journal.txt
     mosaik-giornale 1 / version <versione> / stato applico|installato / file <percorso> / nuovo <percorso>
   Si scrive prima di sostituire qualsiasi file; "file" ha l'originale in <percorso>.annulla,
   "nuovo" non esisteva. Gli .annulla restano finché il sistema nuovo non arriva al menu con i suoi
   core caricabili: solo allora si cancellano. Se l'installazione si interrompe (stato applico) o
   l'avvio torna al sistema precedente (versione diversa da quella in uso), system-init rimette
   tutti i file di prima: niente versioni miste. */
#define JOURNAL "System/update-journal.txt"
#define JMAX 512
static bool boot_failed;
bool update_boot_failed(void) { return boot_failed; }

typedef struct { char version[64], state[16]; int n; char kind[JMAX]; char path[JMAX][512]; } journal_t;

static bool journal_read(const char *root, journal_t *j) {
    char p[1100], line[600];
    snprintf(p, sizeof p, "%s/" JOURNAL, root);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    memset(j, 0, sizeof *j);
    bool ok = fgets(line, sizeof line, f) && !strncmp(line, "mosaik-giornale 1", 17);
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "version ", 8)) snprintf(j->version, sizeof j->version, "%s", line + 8);
        else if (!strncmp(line, "stato ", 6)) snprintf(j->state, sizeof j->state, "%s", line + 6);
        else if (j->n < JMAX && (!strncmp(line, "file ", 5) || !strncmp(line, "nuovo ", 6))) {
            j->kind[j->n] = line[0];
            snprintf(j->path[j->n], sizeof j->path[0], "%s", line + (line[0] == 'f' ? 5 : 6));
            j->n++;
        }
    }
    fclose(f);
    return ok;
}

#ifdef __linux__
#include <dlfcn.h>
/* i core arrivati con l'aggiornamento si caricano davvero (librerie, simboli): un core che non
   parte non deve passare per buono */
static bool cores_load(const char *root, const journal_t *j) {
    for (int i = 0; i < j->n; i++) {
        const char *q = j->path[i];
        size_t l = strlen(q);
        if (strncmp(q, "Cores/", 6) || l < 4 || strcmp(q + l - 3, ".so")) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", root, q);
        void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (!h) { ui_log("aggiornamento: %s non si carica: %s", q, dlerror()); return false; }
        unsigned (*api)(void) = (unsigned (*)(void))dlsym(h, "retro_api_version");
        bool ok = api && api() == 1;
        dlclose(h);
        if (!ok) { ui_log("aggiornamento: %s non è un core libretro valido", q); return false; }
    }
    return true;
}
#else
static bool cores_load(const char *root, const journal_t *j) { (void)root; (void)j; return true; }
#endif

void update_confirm_boot(const char *root) {
    char p[1100], j_path[1100];
    snprintf(p, sizeof p, "%s/System/update-pending", root);
    snprintf(j_path, sizeof j_path, "%s/" JOURNAL, root);
    static journal_t j;
    if (journal_read(root, &j) && !strcmp(j.state, "installato") && !strcmp(j.version, update_current())) {
        if (!cores_load(root, &j)) {
            /* il prossimo avvio è il terzo di prova: l'initramfs rimette il sistema precedente e
               system-init i file di prima */
            FILE *f = fopen(p, "w");
            if (f) { fputs("2\n", f); fflush(f); fsync(fileno(f)); fclose(f); }
            boot_failed = true;
            ui_log("aggiornamento: al prossimo avvio si torna alla versione precedente");
            return;
        }
        /* prima il segno di prova, poi le copie di sicurezza, per ultimo il giornale: se la corrente
           salta a metà, system-init finisce il lavoro (giornale senza segno di prova) */
        if (unlink(p) == 0) ui_log("aggiornamento: il sistema nuovo funziona");
        int gone = 0;
        for (int i = 0; i < j.n; i++) {
            char b[1100];
            snprintf(b, sizeof b, "%s/%s.annulla", root, j.path[i]);
            if (unlink(b) == 0) gone++;
        }
        unlink(j_path);
        ui_log("aggiornamento: giornale chiuso, %d file nell'elenco, %d copie di sicurezza tolte", j.n, gone);
        sync();
        return;
    }
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

/* novità delle versioni ("note VERSIONE LINGUA testo" nell'elenco firmato) */
typedef struct { char ver[16], lang[4], text[204]; } note_t;
static note_t notes[64];
static int nnotes;

static void vparse(const char *v, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
}
static int vcmp(const char *a, const char *b) {
    int x[3], y[3];
    vparse(a, x);
    vparse(b, y);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

int update_notes(const char *lang, const char **out, int max) {
    const char *cur = update_current();
    int n = 0;
    for (int i = 0; i < nnotes && n < max; ) {
        /* blocco di una versione: le righe nella lingua scelta, altrimenti quelle inglesi */
        int j = i;
        while (j < nnotes && !strcmp(notes[j].ver, notes[i].ver)) j++;
        if (vcmp(notes[i].ver, cur) > 0) {
            bool have = false;
            for (int k = i; k < j; k++) have |= !strcmp(notes[k].lang, lang);
            for (int k = i; k < j && n < max; k++)
                if (!strcmp(notes[k].lang, have ? lang : "en")) out[n++] = notes[k].text;
        }
        i = j;
    }
    return n;
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
#include <dirent.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/statvfs.h>
#include "update_pubkey.h"

#ifndef UPDATE_DEFAULT_URL
#define UPDATE_DEFAULT_URL ""
#endif
#define MAX_FILES 2048
#define SYSFILE "System/arcade-system.squashfs"

static char root_dir[1024];
typedef struct { char sha[65]; long long size; char path[512]; bool need; int index; bool backed, placed; } item_t;
static item_t *items;
static int nitems;
static char base_url[256];
static int channel;
static pthread_t thread;
static bool thread_on;

bool update_supported(void) { const char *e = getenv("ARCADE_STANDALONE"); return e && *e == '1'; }

static bool resumed;        /* UPD_READY da uno scaricamento tenuto da parte (update_resume_start) */
static int probe_pause;   /* "prova-pausa N" nell'elenco firmato: solo per le prove (R03) */
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
    if (rc != CURLE_OK) ui_log("aggiornamento: %s: %s", name, curl_easy_strerror(rc));
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
    if (!c) { fclose(f); return false; }
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
    /* dati scritti dallo scraper sulla console: non sono del sistema (anche gli indici delle
       console e i dati libretro: un aggiornamento non deve sovrascriverli) */
    if (!strcmp(p, "Dats/arcadedb.tsv") || !strncmp(p, "Dats/history/", 13) || !strncmp(p, "Dats/libretro/", 14)) return false;
    if (!strncmp(p, "Dats/", 5)) {
        char id[32];
        snprintf(id, sizeof id, "%.*s", (int)strcspn(p + 5, "."), p + 5);
        const emulator_t *e = emulator_find(id);
        if (e && e->console) return false;
    }
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
    st.reinstall = false;
    probe_pause = 0;
    nnotes = 0;
    for (line = strtok_r(NULL, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "version ", 8)) { snprintf(st.version, sizeof st.version, "%s", line + 8); continue; }
        /* "reinstall N": le schede con l'area di avvio (initramfs, kernel) di generazione più
           bassa di N non hanno cose che il Wi-Fi non può portare: si invita a riscrivere la SD.
           La generazione la scrive l'initramfs in System/area-avvio.txt (manca: 0). */
        if (!strncmp(line, "prova-pausa ", 12)) { probe_pause = atoi(line + 12); if (probe_pause > 60) probe_pause = 60; continue; }
        if (!strncmp(line, "note ", 5)) {
            char v[16], lg[4];
            int off = 0;
            if (nnotes < 64 && sscanf(line + 5, "%15s %3s %n", v, lg, &off) == 2 && off > 0) {
                note_t *nt = &notes[nnotes++];
                snprintf(nt->ver, sizeof nt->ver, "%s", v);
                snprintf(nt->lang, sizeof nt->lang, "%s", lg);
                snprintf(nt->text, sizeof nt->text, "%s", line + 5 + off);
            }
            continue;
        }
        if (!strncmp(line, "reinstall ", 10)) {
            st.reinstall = update_boot_gen(root_dir, NULL) < atoi(line + 10);
            continue;
        }
        /* Una riga che questa versione non conosce si ignora: la firma è già stata controllata, e
           così una versione futura può aggiungere informazioni senza bloccare chi deve aggiornarsi. */
        if (strncmp(line, "file ", 5)) continue;
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

/* elenco firmato della versione in corso: resta con lo scaricamento finito (update-ready) */
static char g_text[1 << 20];
static unsigned char g_sig[512];
static size_t g_text_len, g_sig_len;

static void *check_worker(void *arg) {
    (void)arg;
    st.phase = UPD_SEARCH;
    if (!configured_url()) { st.phase = UPD_NO_SERVER; return NULL; }
    ui_log("aggiornamento: server %s", base_url);
    st.phase = UPD_CHECK;
    mem_t mt = { g_text, 0, sizeof g_text }, ms = { (char *)g_sig, 0, sizeof g_sig };
    if (!get_mem("update.txt", &mt) || !get_mem("update.sig", &ms)) { fail(UPE_NETWORK); return NULL; }
    g_text_len = mt.len;
    g_sig_len = ms.len;
    if (!signature_ok(g_text, mt.len, g_sig, ms.len)) { ui_log("aggiornamento: firma non valida"); fail(UPE_SIGNATURE); return NULL; }
    /* parse_manifest spezza il testo: se ne tiene una copia intera per update-ready */
    static char work[1 << 20];
    memcpy(work, g_text, mt.len + 1);
    if (!parse_manifest(work)) { ui_log("aggiornamento: elenco non valido"); fail(UPE_MANIFEST); return NULL; }
    /* solo versioni più recenti (le date di compilazione si confrontano come testo) */
    if (strcmp(stamp_of(st.version), stamp_of(update_current())) <= 0) { st.phase = UPD_CURRENT; return NULL; }
    /* la versione è nuova: lo si dice subito. Quali file servono davvero si vede solo dopo
       "Scarica" (install_worker), perché confrontarli vuol dire leggere decine di MB dalla SD;
       intanto si mostra il massimo, cioè tutta la versione */
    long long all = 0;
    for (int i = 0; i < nitems; i++) all += items[i].size;
    st.files = nitems;
    st.done = 0;
    st.total = all;
    ui_log("aggiornamento: disponibile %s (al massimo %d file, %lld byte)", st.version, nitems, all);
    st.phase = UPD_AVAILABLE;
    return NULL;
}

/* quali file della versione mancano o sono diversi sulla scheda; false se annullato */
static bool compare_files(void) {
    st.phase = UPD_CHECK;
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
    if (st.cancel) return false;
    st.files = need;
    st.done = 0;
    st.total = need_bytes;
    ui_log("aggiornamento: %s, %d file da scaricare, %lld byte", st.version, need, need_bytes);
    return true;
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
    /* già scaricato in un giro precedente (rete caduta, console spenta): se è giusto si tiene;
       se è a metà si continua da lì */
    struct stat sb;
    long long have = 0;
    if (stat(tmp, &sb) == 0 && sb.st_size == it->size && sha256_file(tmp, sha, false) && !strcmp(sha, it->sha)) {
        st.done += it->size;
        ui_log("aggiornamento: %s già scaricato", it->path);
        return true;
    }
    if (stat(tmp, &sb) == 0 && sb.st_size > 0 && sb.st_size < it->size) have = sb.st_size;
    FILE *f = fopen(tmp, have ? "ab" : "wb");
    if (!f) { *err = UPE_WRITE; return false; }
    CURL *c = new_curl(url);
    if (!c) { fclose(f); *err = UPE_NETWORK; return false; }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    if (have) {
        ui_log("aggiornamento: %s, riprendo da %lld byte", it->path, have);
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have);
        st.done += have;
    }
    if (it->size > 0) curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(it->size - have));   /* non oltre la misura firmata */
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
    /* la rilettura deve venire dalla scheda, non dalla memoria */
    posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
    fclose(f);
    if (st.cancel) { *err = UPE_CANCEL; return false; }
    if (rc == CURLE_FILESIZE_EXCEEDED) { unlink(tmp); *err = UPE_VERIFY; return false; }   /* più grande del file firmato */
    /* il server non riprende a metà: si ricomincia il file da capo (il giro dopo) */
    if (rc == CURLE_RANGE_ERROR) { unlink(tmp); *err = UPE_VERIFY; return false; }
    if (rc != CURLE_OK) { *err = UPE_NETWORK; return false; }
    if (!flushed) { *err = UPE_WRITE; return false; }
    if (!sha256_file(tmp, sha, false) || strcmp(sha, it->sha)) { unlink(tmp); *err = UPE_VERIFY; return false; }
    return true;
}

static void tmp_name(const item_t *it, char *out, size_t n) { snprintf(out, n, "%s/%s.nuovo", root_dir, it->path); }

/* errore a metà: si rimette il sistema di prima. Se non riesce resta il .prec, che l'avvio sa usare */
#ifndef SYS_BLOCK_DIR
#define SYS_BLOCK_DIR "/sys/block"
#endif
#ifndef SYS_ROOT_PATH
#define SYS_ROOT_PATH "/"
#endif

/* La console sta girando sulla copia precedente? Lo dice il kernel: è il file dietro il
   dispositivo loop del sistema. Il solo controllo del file principale non basta, perché può
   esserci ma essere rovinato: in quel caso l'initramfs è ripartito dalla copia precedente. */
enum { RUN_UNKNOWN, RUN_MAIN, RUN_PREV };

static bool read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}

/* Quale file di sistema è montato come radice: il dispositivo loop con lo stesso numero della
   radice. Se nessuno corrisponde si accetta il nome del file, ma solo se un unico dispositivo
   loop ne ha uno del sistema: con due o più non si sa quale sia in uso. */
static int running_system(void) {
    struct stat rs;
    char rootdev[32] = "";
    if (stat(SYS_ROOT_PATH, &rs) == 0) snprintf(rootdev, sizeof rootdev, "%u:%u", major(rs.st_dev), minor(rs.st_dev));
    DIR *d = opendir(SYS_BLOCK_DIR);
    struct dirent *e;
    int by_dev = RUN_UNKNOWN, by_name = RUN_UNKNOWN, named = 0;
    char line[1200], used[1200] = "";
    while (d && by_dev == RUN_UNKNOWN && (e = readdir(d))) {
        if (strncmp(e->d_name, "loop", 4)) continue;   /* tutti i dispositivi loop, non solo i primi */
        char p[512], dev[32];
        snprintf(p, sizeof p, SYS_BLOCK_DIR "/%s/loop/backing_file", e->d_name);
        if (!read_line(p, line, sizeof line) || !strstr(line, "arcade-system.squashfs")) continue;
        int kind = strstr(line, ".squashfs.prec") ? RUN_PREV : RUN_MAIN;
        snprintf(p, sizeof p, SYS_BLOCK_DIR "/%s/dev", e->d_name);
        if (rootdev[0] && read_line(p, dev, sizeof dev) && !strcmp(dev, rootdev)) { by_dev = kind; snprintf(used, sizeof used, "%s", line); }
        else if (named++ == 0) { by_name = kind; snprintf(used, sizeof used, "%s", line); }
    }
    if (d) closedir(d);
    int found = by_dev != RUN_UNKNOWN ? by_dev : named == 1 ? by_name : RUN_UNKNOWN;
    if (found == RUN_UNKNOWN) ui_log("aggiornamento: non so quale sistema è in uso (%d candidati)", named);
    else ui_log("aggiornamento: sistema in uso: %s%s", used, by_dev != RUN_UNKNOWN ? "" : " (dal nome)");
    return found;
}

static bool restore_system(const char *prev, const char *dst) {
    if (rename(prev, dst) == 0) return true;
    ui_log("aggiornamento: sistema precedente NON ripristinato (%s): resta %s", strerror(errno), prev);
    return false;
}

/* Mentre si sostituiscono i file, quello vecchio resta di fianco come .annulla: se un passaggio
   fallisce si rimette tutto com'era, e la scheda non resta con file di due versioni. */
static void remove_tmps(void);
static void journal_clear(void);
static void backup_name(const item_t *it, char *out, size_t n) { snprintf(out, n, "%s/%s.annulla", root_dir, it->path); }

/* rimette gli originali; falso se anche uno solo non torna (il giornale allora resta) */
static bool undo_items(void) {
    char dst[1100], bak[1120];
    bool ok = true;
    for (int i = nitems - 1; i >= 0; i--) {
        item_t *it = &items[i];
        snprintf(dst, sizeof dst, "%s/%s", root_dir, it->path);
        backup_name(it, bak, sizeof bak);
#ifdef UPDATE_TEST   /* prova: un ripristino che non riesce (m2/test-update.sh) */
        const char *tf = getenv("UPDATE_UNDO_FAIL");
        if (it->backed && tf && !strcmp(tf, it->path)) { ui_log("aggiornamento: %s NON ripristinato (prova)", it->path); ok = false; continue; }
#endif
        if (it->backed) {
            if (rename(bak, dst) != 0) { ui_log("aggiornamento: %s NON ripristinato (%s)", it->path, strerror(errno)); ok = false; continue; }
        } else if (it->placed && unlink(dst) != 0 && errno != ENOENT) {
            ui_log("aggiornamento: %s nuovo NON tolto (%s)", it->path, strerror(errno));
            ok = false;
            continue;
        }
        it->backed = it->placed = false;
    }
    return ok;
}

/* Un'installazione non riuscita: si rimette tutto com'era. Il giornale si cancella solo se il
   ripristino è completo; altrimenti resta (stato "applico") e al riavvio system-init rimette i
   file che mancano, e fino ad allora qui non si installa altro (audit A56-001). */
static bool undo_stuck;
/* anche dopo un riavvio del launcher (un gioco in mezzo): un giornale ancora sulla scheda vuol dire
   un'installazione da finire o da disfare all'avvio, e un'altra lo riscriverebbe */
static bool install_blocked(void) {
    char j[1100];
    snprintf(j, sizeof j, "%s/" JOURNAL, root_dir);
    if (!undo_stuck && access(j, F_OK) != 0) return false;
    ui_log("aggiornamento: giornale di un'installazione precedente ancora presente, prima il riavvio");
    return true;
}
static void rollback_apply(bool sys_moved, const char *prev, const char *dst) {
    bool ok = true;
    if (sys_moved) ok = restore_system(prev, dst);
    ok = undo_items() && ok;
    remove_tmps();
    if (ok) { journal_clear(); fail(UPE_WRITE); return; }
    undo_stuck = true;
    ui_log("aggiornamento: ripristino incompleto, il giornale resta: al riavvio system-init rimette i file di prima");
    fail(UPE_UNDO);
}


/* mette il file nuovo al suo posto tenendo da parte il vecchio */
static bool place_item(item_t *it, const char *tmp, const char *dst) {
    char bak[1120];
    struct stat dst_st;
    backup_name(it, bak, sizeof bak);
    it->backed = it->placed = false;
    if (lstat(dst, &dst_st) == 0) {
        if (S_ISDIR(dst_st.st_mode)) { errno = EISDIR; return false; }
        if (rename(dst, bak) != 0) return false;
        it->backed = true;
    }
    if (rename(tmp, dst) != 0) return false;
    it->placed = true;
    return true;
}

static void remove_tmps(void) {
    char t[1100];
    for (int i = 0; i < nitems; i++) if (items[i].need) { tmp_name(&items[i], t, sizeof t); unlink(t); }
}

/* scaricamento finito e controllato: l'elenco firmato resta sulla scheda, così dopo uno
   spegnimento la console propone di installare senza riscaricare (update_resume_start) */
#define READY_TXT "System/update-ready.txt"
#define READY_SIG "System/update-ready.sig"
static bool write_file(const char *rel, const void *data, size_t n) {
    char p[1100], t[1120];
    snprintf(p, sizeof p, "%s/%s", root_dir, rel);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(t, p) != 0) { unlink(t); return false; }
    return true;
}
static void save_ready(void) {
    if (!write_file(READY_SIG, g_sig, g_sig_len) || !write_file(READY_TXT, g_text, g_text_len))
        ui_log("aggiornamento: impossibile ricordare lo scaricamento finito");
}
static void drop_ready(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/" READY_TXT, root_dir);
    unlink(p);
    snprintf(p, sizeof p, "%s/" READY_SIG, root_dir);
    unlink(p);
}

static void *install_worker(void *arg) {
    (void)arg;
    if (!compare_files()) { fail(UPE_CANCEL); return NULL; }
    if (!st.files) { st.phase = UPD_CURRENT; return NULL; }   /* versione nuova, file già tutti uguali */
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
        if (!ok) {
            ui_log("aggiornamento: %s non riuscito (%d)", it->path, err);
            /* rete caduta: i file restano per riprendere al prossimo tentativo; se non si
               riprova, li toglie il controllo all'avvio del launcher */
            if (err != UPE_NETWORK) remove_tmps();
            fail(err);
            return NULL;
        }
    }
    /* tutti i file sono giusti: si aspetta la conferma, a schermo */
    save_ready();
    st.phase = UPD_READY;
    return NULL;
}

/* si sostituiscono i file, il sistema per ultimo */
/* scrittura sicura del giornale: file provvisorio, fsync, rename */
static bool journal_write(const char *state) {
    char p[1100], t[1120];
    snprintf(p, sizeof p, "%s/" JOURNAL, root_dir);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    if (!f) return false;
    bool ok = fprintf(f, "mosaik-giornale 1\nversion %s\nstato %s\n", st.version, state) > 0;
    for (int i = 0; ok && i < nitems; i++) {
        item_t *it = &items[i];
        if (!it->need || !strcmp(it->path, SYSFILE)) continue;
        char dst[1100];
        struct stat sb;
        snprintf(dst, sizeof dst, "%s/%s", root_dir, it->path);
        /* "file": c'è un originale da tenere (prima della sostituzione è dst, dopo è .annulla) */
        bool orig = !strcmp(state, "applico") ? stat(dst, &sb) == 0 : it->backed;
        ok = fprintf(f, "%s %s\n", orig ? "file" : "nuovo", it->path) > 0;
    }
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(t, p) != 0) { unlink(t); return false; }
    char d[1100];
    snprintf(d, sizeof d, "%s/System", root_dir);
    int fd = open(d, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
    return true;
}

static void journal_clear(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/" JOURNAL, root_dir);
    unlink(p);
}

static bool write_pending(const char *pend) {
    FILE *f = fopen(pend, "w");
    bool marked = f && fputs("0\n", f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) marked = false;
    return marked;
}

static void *apply_worker(void *arg) {
    (void)arg;
    char tmp[1100], dst[1100];
    item_t *sys = NULL;
    /* prima di toccare qualsiasi file: il giornale, così un'interruzione si può sempre disfare */
    if (!journal_write("applico")) { ui_log("aggiornamento: impossibile scrivere il giornale"); remove_tmps(); fail(UPE_WRITE); return NULL; }
    ui_log("aggiornamento: giornale scritto, gli originali restano fino al primo avvio");
    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (!it->need) continue;
        if (!strcmp(it->path, SYSFILE)) { sys = it; continue; }
        tmp_name(it, tmp, sizeof tmp);
        snprintf(dst, sizeof dst, "%s/%s", root_dir, it->path);
        if (!place_item(it, tmp, dst)) { ui_log("aggiornamento: rename %s: %s", it->path, strerror(errno)); rollback_apply(false, NULL, NULL); return NULL; }
    }
    /* prova su console (System/prova-aggiornamento.txt, vale una volta): i dati sono già
       sostituiti, il sistema non ancora. 30 s per spegnere a forza e vedere che all'avvio dopo
       system-init rimette i file di prima */
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/System/prova-aggiornamento.txt", root_dir);
    int pause_s = unlink(probe) == 0 ? 30 : probe_pause;
    if (pause_s > 0) {
        ui_log("aggiornamento: prova, pausa di %d s con i dati nuovi e il sistema vecchio: spegni ora", pause_s);
        sync();
        sleep((unsigned)pause_s);
    }
    char pend_all[1100];
    snprintf(pend_all, sizeof pend_all, "%s/System/update-pending", root_dir);
    /* anche senza sistema nuovo si contano gli avvii di prova: alla terza prova fallita
       system-init rimette i file di prima */
    if (!sys && !write_pending(pend_all)) {
        ui_log("aggiornamento: impossibile scrivere update-pending");
        unlink(pend_all); rollback_apply(false, NULL, NULL); return NULL;
    }
    if (sys) {
        char prev[1100], pend[1100];
        tmp_name(sys, tmp, sizeof tmp);
        snprintf(dst, sizeof dst, "%s/%s", root_dir, SYSFILE);
        snprintf(prev, sizeof prev, "%s.prec", dst);
        snprintf(pend, sizeof pend, "%s/System/update-pending", root_dir);
        /* il sistema in uso resta aperto (è montato) anche con un altro nome */
        /* Se la console sta girando sul .prec, quella è la copia buona e non va toccata: il
           sistema nuovo prende il posto del principale, mancante o rovinato che sia. Se non si
           riesce a sapere quale dei due è in uso, la copia precedente non si cancella mai. */
        struct stat main_st, prev_st;
        bool moved = false;
        int run = running_system();
        bool keep_prev = run == RUN_PREV || (run == RUN_UNKNOWN && stat(prev, &prev_st) == 0);
        if (!keep_prev && stat(dst, &main_st) == 0) {
            unlink(prev);
            if (rename(dst, prev) != 0) { ui_log("aggiornamento: rename del sistema: %s", strerror(errno)); rollback_apply(false, NULL, NULL); return NULL; }
            moved = true;
        }
        /* senza questo file non si contano gli avvii di prova: non si prosegue */
        if (!write_pending(pend)) {
            ui_log("aggiornamento: impossibile scrivere update-pending");
            unlink(pend);
            rollback_apply(moved, prev, dst);
            return NULL;
        }
        if (rename(tmp, dst) != 0) {
            ui_log("aggiornamento: rename del sistema nuovo: %s", strerror(errno));
            unlink(pend);
            rollback_apply(moved, prev, dst);
            return NULL;
        }
    }
    /* le copie .annulla restano fino alla conferma del primo avvio (update_confirm_boot) */
    if (!journal_write("installato")) ui_log("aggiornamento: giornale non aggiornato: al prossimo avvio si torna ai file di prima");
    sync();
    drop_ready();
    ui_log("aggiornamento: installata la versione %s", st.version);
    st.phase = UPD_DONE;
    return NULL;
}

void update_join(void) {
    if (thread_on) { pthread_join(thread, NULL); thread_on = false; }
}

static void notify_join(void);

void update_check(const char *root) {
    update_join();
    notify_join();   /* il controllo silenzioso usa gli stessi dati */
    memset(&st, 0, sizeof st);
    snprintf(root_dir, sizeof root_dir, "%s", root);
    if (!items) items = malloc(sizeof(item_t) * MAX_FILES);
    if (!items) { fail(UPE_MANIFEST); return; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    st.phase = UPD_SEARCH;
    if (pthread_create(&thread, NULL, check_worker, NULL) == 0) thread_on = true;
    else fail(UPE_NETWORK);
}

void update_set_channel(int development) { channel = development; }

/* ---------------------------------------------------- controllo silenzioso */
static pthread_t notify_thread;
static bool notify_on;
static char notify_ver[64];
static pthread_mutex_t notify_lock = PTHREAD_MUTEX_INITIALIZER;   /* il menu lo legge a ogni fotogramma */

static void notify_path(char *p, size_t n, const char *root) { snprintf(p, n, "%s/System/update-notice.txt", root); }

static void *notify_worker(void *arg) {
    (void)arg;
    static char text[1 << 20];
    static unsigned char sig[512];
    mem_t mt = { text, 0, sizeof text }, ms = { (char *)sig, 0, sizeof sig };
    char p[1100];
    notify_path(p, sizeof p, root_dir);
    if (!configured_url() || !get_mem("update.txt", &mt) || !get_mem("update.sig", &ms)) return NULL;
    if (!signature_ok(text, mt.len, sig, ms.len)) return NULL;
    text[mt.len < sizeof text ? mt.len : sizeof text - 1] = 0;
    char *v = strstr(text, "\nversion ");
    char found[64] = "";
    if (v && sscanf(v + 9, " %47[^ \n] %15[0-9.]", found, found + 48) == 2) {
        char ver[64];
        snprintf(ver, sizeof ver, "%s %s", found, found + 48);
        if (strcmp(stamp_of(ver), stamp_of(update_current())) > 0) {
            pthread_mutex_lock(&notify_lock);
            snprintf(notify_ver, sizeof notify_ver, "%s", ver);
            pthread_mutex_unlock(&notify_lock);
        }
    }
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%lld\n%s\n", (long long)time(NULL), notify_ver); fclose(f); }
    return NULL;
}

void update_notify_start(const char *root, int development) {
    if (notify_on || thread_on) return;
    channel = development;
    snprintf(root_dir, sizeof root_dir, "%s", root);
    char p[1100];
    long long last = 0;
    char ver[64] = "";
    notify_path(p, sizeof p, root);
    FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%lld\n%63[^\n]", &last, ver) < 1) last = 0; fclose(f); }
    /* avviso di un controllo precedente, se quella versione è ancora più recente di questa */
    if (ver[0] && strcmp(stamp_of(ver), stamp_of(update_current())) > 0) snprintf(notify_ver, sizeof notify_ver, "%s", ver);
    time_t now = time(NULL);
    if (now > 1600000000 && last > 0 && now - last < 24 * 3600 && now >= last) return;   /* orologio valido: una volta al giorno */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (pthread_create(&notify_thread, NULL, notify_worker, NULL) == 0) notify_on = true;
}

const char *update_notify_version(void) {
    static char copy[sizeof notify_ver];
    pthread_mutex_lock(&notify_lock);
    memcpy(copy, notify_ver, sizeof copy);
    pthread_mutex_unlock(&notify_lock);
    return copy;
}

static void notify_join(void) {
    if (notify_on) { pthread_join(notify_thread, NULL); notify_on = false; }
}

void update_install(void) {
    update_join();
    if (install_blocked()) { fail(UPE_UNDO); return; }   /* prima il riavvio che rimette i file */
    st.cancel = false;
    st.phase = UPD_CHECK;
    if (pthread_create(&thread, NULL, install_worker, NULL) == 0) thread_on = true;
    else fail(UPE_WRITE);
}

void update_apply(void) {
    update_join();
    if (install_blocked()) { remove_tmps(); fail(UPE_UNDO); return; }
    st.phase = UPD_INSTALL;
    if (pthread_create(&thread, NULL, apply_worker, NULL) == 0) thread_on = true;
    else { remove_tmps(); fail(UPE_WRITE); }
}

void update_discard(void) {
    update_join();
    remove_tmps();
    drop_ready();
    resumed = false;
    fail(UPE_CANCEL);
}

/* ------------------------------------------- scaricato e non installato */
/* file .nuovo nelle cartelle del sistema che non servono all'aggiornamento tenuto da parte */
static bool kept(const char *path) {
    for (int i = 0; i < nitems; i++) {
        if (!items[i].need) continue;
        char t[1100];
        tmp_name(&items[i], t, sizeof t);
        if (!strcmp(t, path)) return true;
    }
    return false;
}
static int sweep(const char *dir, bool keep) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (lstat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { n += sweep(p, keep); continue; }
        size_t l = strlen(e->d_name);
        if (S_ISREG(sb.st_mode) && l > 6 && !strcmp(e->d_name + l - 6, ".nuovo") && !(keep && kept(p)) && unlink(p) == 0) n++;
    }
    closedir(d);
    return n;
}
static void sweep_all(bool keep) {
    static const char *const dirs[] = { "System", "Lang", "Cores", "Emulators", "Themes", "Filters", "Bezels", "Dats" };
    int n = 0;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", root_dir, dirs[i]);
        n += sweep(p, keep);
    }
    if (n) ui_log("aggiornamento: tolti %d file scaricati e non più utili", n);
}

static bool read_all(const char *rel, void *buf, size_t cap, size_t *len) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", root_dir, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    *len = fread(buf, 1, cap - 1, f);
    bool ok = !ferror(f) && feof(f);
    fclose(f);
    return ok;
}

static void *resume_worker(void *arg) {
    (void)arg;
    char j[1100];
    snprintf(j, sizeof j, "%s/" JOURNAL, root_dir);
    if (access(j, F_OK) == 0) { st.phase = UPD_IDLE; return NULL; }   /* un'installazione ancora da confermare */
    static char work[1 << 20];
    bool have = read_all(READY_TXT, g_text, sizeof g_text, &g_text_len) && read_all(READY_SIG, g_sig, sizeof g_sig, &g_sig_len);
    bool ok = false;
    if (have) {
        g_text[g_text_len] = 0;
        memcpy(work, g_text, g_text_len + 1);
        ok = signature_ok(g_text, g_text_len, g_sig, g_sig_len) && parse_manifest(work)
             && strcmp(stamp_of(st.version), stamp_of(update_current())) > 0;
        if (ok) ok = compare_files() && st.files > 0;
        /* ogni file da installare c'è ed è quello firmato */
        for (int i = 0; ok && i < nitems; i++) {
            if (!items[i].need) continue;
            char t[1100], sha[65];
            struct stat sb;
            tmp_name(&items[i], t, sizeof t);
            ok = stat(t, &sb) == 0 && sb.st_size == items[i].size && sha256_file(t, sha, false) && !strcmp(sha, items[i].sha);
            if (!ok) ui_log("aggiornamento: %s scaricato ma non integro", items[i].path);
        }
        if (!ok) { ui_log("aggiornamento: lo scaricamento tenuto da parte non vale più: lo tolgo"); drop_ready(); }
    }
    sweep_all(ok);
    if (ok) {
        resumed = true;
        ui_log("aggiornamento: %s già scaricato, da installare", st.version);
        st.phase = UPD_READY;
    } else st.phase = UPD_IDLE;
    return NULL;
}

void update_resume_start(const char *root) {
    update_join();
    memset(&st, 0, sizeof st);
    resumed = false;
    snprintf(root_dir, sizeof root_dir, "%s", root);
    if (!items) items = malloc(sizeof(item_t) * MAX_FILES);
    if (!items) return;
    nitems = 0;
    if (pthread_create(&thread, NULL, resume_worker, NULL) == 0) thread_on = true;
}

bool update_resume_ready(void) { return resumed && st.phase == UPD_READY; }

#else
/* ------------------------------------------------------- Mac: percorso simulato */
#include <time.h>
static double sim_t0;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
bool update_supported(void) { return true; }
void update_join(void) {}
void update_set_channel(int development) { (void)development; }
void update_notify_start(const char *root, int development) { (void)root; (void)development; }
const char *update_notify_version(void) { const char *e = getenv("ARCADE_TEST_NOTICE"); return e ? e : ""; }
void update_check(const char *root) {
    (void)root;
    memset(&st, 0, sizeof st);
    snprintf(st.version, sizeof st.version, "9.9.9 20991231.2359");
    /* novità di prova: le righe di ../CHANGELOG.txt (ARCADE_CHANGELOG per un altro file) */
    nnotes = 0;
    FILE *cf = fopen(getenv("ARCADE_CHANGELOG") ? getenv("ARCADE_CHANGELOG") : "../CHANGELOG.txt", "r");
    char cl[300], cv[16] = "";
    while (cf && fgets(cl, sizeof cl, cf) && nnotes < 64) {
        cl[strcspn(cl, "\r\n")] = 0;
        if (!cl[0] || cl[0] == '#') continue;
        if (cl[0] >= '0' && cl[0] <= '9') { snprintf(cv, sizeof cv, "%s", cl); continue; }
        if (!cv[0] || strlen(cl) < 4 || cl[2] != ' ') continue;
        note_t *nt = &notes[nnotes++];
        snprintf(nt->ver, sizeof nt->ver, "%s", cv);
        snprintf(nt->lang, sizeof nt->lang, "%.2s", cl);
        snprintf(nt->text, sizeof nt->text, "%s", cl + 3);
    }
    if (cf) fclose(cf);
    st.reinstall = getenv("TRIMUI_FAKE_REINSTALL") != NULL;   /* prova dell'invito a riscrivere la SD */
    st.files = 12;
    st.total = 64LL << 20;
    st.phase = UPD_AVAILABLE;
}
void update_install(void) { sim_t0 = now_s(); st.phase = UPD_CHECK; }
void update_apply(void) { st.phase = UPD_DONE; }
void update_discard(void) { st.error = UPE_CANCEL; st.phase = UPD_FAILED; }
/* ARCADE_TEST_READY: prova della finestra "aggiornamento scaricato" */
void update_resume_start(const char *root) {
    (void)root;
    if (!getenv("ARCADE_TEST_READY")) return;
    memset(&st, 0, sizeof st);
    snprintf(st.version, sizeof st.version, "9.9.9 20991231.2359");
    st.phase = UPD_READY;
}
bool update_resume_ready(void) { return getenv("ARCADE_TEST_READY") && st.phase == UPD_READY; }
static void sim_tick(void) {
    if (st.phase == UPD_CHECK) {   /* confronto dei file, poi lo scaricamento */
        double c = now_s() - sim_t0;
        st.done = (long long)(st.total * (c > 1 ? 1 : c));
        if (st.cancel) { st.error = UPE_CANCEL; st.phase = UPD_FAILED; }
        else if (c > 1.0) { st.files = 5; st.total = 24LL << 20; st.done = 0; sim_t0 = now_s(); st.phase = UPD_DOWNLOAD; }
        return;
    }
    if (st.phase != UPD_DOWNLOAD) return;
    double t = now_s() - sim_t0;
    st.done = (long long)(st.total * (t / 2.0 > 1 ? 1 : t / 2.0));
    if (st.cancel) { st.error = UPE_CANCEL; st.phase = UPD_FAILED; }
    else if (t > 2.0) st.phase = UPD_READY;
}
#endif
