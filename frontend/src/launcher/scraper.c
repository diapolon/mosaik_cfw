#include "scraper.h"
#include "../common/json.h"
#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define UA "TrimUI-Arcade-Prototype/0.1"
#define ADB "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&use_parent=1&game_name="
#define META_HEADER "# arcadedb v3"   /* v3: con la storia in Dats/history/<rom>.txt */
#define MAX_SIDE 640

static const char *TYPES[] = { "snap", "title", "marquee" };
static const char *ADB_FIELDS[] = { "url_image_ingame", "url_image_title", "url_image_marquee" };
static const char *LIBRETRO_KINDS[] = { "Named_Snaps", "Named_Titles", NULL };
#define NTYPES 3

typedef struct { char name[64], title[160], core[16]; } item_t;
typedef struct { char name[64], genre[64], players[8]; } meta_t;

static scrape_status_t st;
static SDL_Thread *thread;
static char root_dir[1024];
static item_t *items;
static int nitems;
static meta_t *meta;
static int nmeta, cap_meta;
static char **notfound;
static int nnotfound, cap_notfound;
static Uint32 last_request;

/* ----------------------------------------------------------- rete */

typedef struct { char *data; size_t len; } buf_t;

static size_t on_data(void *p, size_t sz, size_t n, void *ud) {
    buf_t *b = ud;
    size_t add = sz * n;
    if (b->len + add > (32u << 20)) return 0;   /* nessuna risposta o immagine è così grande */
    char *d = realloc(b->data, b->len + add + 1);
    if (!d) return 0;
    memcpy(d + b->len, p, add);
    b->data = d;
    b->len += add;
    d[b->len] = 0;
    return add;
}

/* una richiesta al secondo: sono servizi gratuiti della comunità */
static bool http_get(const char *url, buf_t *out, char *ctype, size_t cn) {
    Uint32 now = SDL_GetTicks();
    if (last_request && now - last_request < 1000) SDL_Delay(1000 - (now - last_request));
    last_request = SDL_GetTicks();
    out->data = NULL;
    out->len = 0;
    CURL *c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    /* certificati per HTTPS: il firmware della console potrebbe non averli, il pacchetto
       porta il proprio elenco (System/cacert.pem, quello di Mozilla distribuito da curl) */
    char ca[1100];
    struct stat cst;
    snprintf(ca, sizeof ca, "%s/System/cacert.pem", root_dir);
    if (stat(ca, &cst) == 0) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    char *ct = NULL;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    if (ctype) snprintf(ctype, cn, "%s", ct ? ct : "");
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200) {
        free(out->data);
        out->data = NULL;
        out->len = 0;
        return false;
    }
    return true;
}

/* ------------------------------------------------------- immagini */

/* salva il PNG, ridotto se supera MAX_SIDE; scrittura atomica con file temporaneo */
static bool save_image(const buf_t *b, const char *dest) {
    SDL_Surface *img = IMG_Load_RW(SDL_RWFromConstMem(b->data, (int)b->len), 1);
    if (!img) return false;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", dest);
    /* PNG già abbastanza piccolo: si salva com'è, senza ricodificarlo (resta più leggero) */
    if (img->w <= MAX_SIDE && img->h <= MAX_SIDE && b->len > 8 && !memcmp(b->data, "\x89PNG", 4)) {
        SDL_FreeSurface(img);
        FILE *f = fopen(tmp, "wb");
        bool ok = f && fwrite(b->data, 1, b->len, f) == b->len;
        if (f) fclose(f);
        return ok && rename(tmp, dest) == 0;
    }
    SDL_Surface *out = img;
    if (img->w > MAX_SIDE || img->h > MAX_SIDE) {
        float s = (float)MAX_SIDE / (img->w > img->h ? img->w : img->h);
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ARGB8888, 0);
        out = SDL_CreateRGBSurfaceWithFormat(0, (int)(img->w * s), (int)(img->h * s), 32, SDL_PIXELFORMAT_ARGB8888);
        if (conv && out) SDL_BlitScaled(conv, NULL, out, NULL);
        SDL_FreeSurface(conv);
        SDL_FreeSurface(img);
        if (!out) return false;
    }
    bool ok = IMG_SavePNG(out, tmp) == 0 && rename(tmp, dest) == 0;
    SDL_FreeSurface(out);
    return ok;
}

static void media_path(char *out, size_t n, int type, const char *name) {
    snprintf(out, n, "%s/Media/%s/%s.png", root_dir, TYPES[type], name);
}

static bool exists(const char *p) {
    struct stat s;
    return stat(p, &s) == 0;
}

/* ------------------------------------------- dati: generi e mancanti */

static meta_t *meta_find(const char *name) {
    for (int i = 0; i < nmeta; i++)
        if (!strcmp(meta[i].name, name)) return &meta[i];
    return NULL;
}

static void meta_set(const char *name, const char *genre, const char *players) {
    meta_t *m = meta_find(name);
    if (!m) {
        if (nmeta == cap_meta) meta = realloc(meta, sizeof(meta_t) * (size_t)(cap_meta = cap_meta ? cap_meta * 2 : 512));
        m = &meta[nmeta++];
        snprintf(m->name, sizeof m->name, "%s", name);
    }
    snprintf(m->genre, sizeof m->genre, "%s", genre);
    snprintf(m->players, sizeof m->players, "%s", players);
}

static void meta_load(void) {
    char p[1100], line[256];
    snprintf(p, sizeof p, "%s/Dats/arcadedb.tsv", root_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    bool v2 = fgets(line, sizeof line, f) && !strncmp(line, META_HEADER, strlen(META_HEADER));
    while (v2 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *g = strchr(line, '\t');
        if (!g) continue;
        *g++ = 0;
        char *pl = strchr(g, '\t');
        if (pl) *pl++ = 0;
        meta_set(line, g, pl ? pl : "");
    }
    fclose(f);
}

static int meta_cmp(const void *a, const void *b) { return strcmp(((const meta_t *)a)->name, ((const meta_t *)b)->name); }

static void meta_save(void) {
    char p[1100], tmp[1110];
    snprintf(p, sizeof p, "%s/Dats/arcadedb.tsv", root_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    qsort(meta, (size_t)nmeta, sizeof(meta_t), meta_cmp);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%s\n", META_HEADER);
    for (int i = 0; i < nmeta; i++) fprintf(f, "%s\t%s\t%s\n", meta[i].name, meta[i].genre, meta[i].players);
    fclose(f);
    rename(tmp, p);
}

static bool nf_has(const char *key) {
    for (int i = 0; i < nnotfound; i++)
        if (!strcmp(notfound[i], key)) return true;
    return false;
}

static void nf_add(const char *key) {
    if (nf_has(key)) return;
    if (nnotfound == cap_notfound) notfound = realloc(notfound, sizeof(char *) * (size_t)(cap_notfound = cap_notfound ? cap_notfound * 2 : 256));
    notfound[nnotfound++] = strdup(key);
}

static void nf_load(void) {
    char p[1100], line[256];
    snprintf(p, sizeof p, "%s/Media/non-trovate.txt", root_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (*line) nf_add(line);
    }
    fclose(f);
}

static void nf_save(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/Media/non-trovate.txt", root_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nnotfound; i++) fprintf(f, "%s\n", notfound[i]);
    fclose(f);
}

static bool missing(const item_t *it, int type) {
    char p[1200], key[96];
    snprintf(key, sizeof key, "%s/%s", TYPES[type], it->name);
    if (nf_has(key)) return false;
    media_path(p, sizeof p, type, it->name);
    return !exists(p);
}

/* -------------------------------------------------------- lavoro */

typedef struct { char name[64], urls[NTYPES][512]; } adb_t;

static int worker(void *arg) {
    (void)arg;
    char p[1200];
    for (int t = 0; t < NTYPES; t++) {
        snprintf(p, sizeof p, "%s/Media/%s", root_dir, TYPES[t]);
        mkdir(p, 0755);
    }
    meta_load();
    nf_load();

    /* giochi da completare: immagini mancanti o informazioni assenti */
    int *todo = malloc(sizeof(int) * (size_t)(nitems + 1)), ntodo = 0;
    for (int i = 0; i < nitems; i++) {
        bool need = !meta_find(items[i].name);
        for (int t = 0; t < NTYPES && !need; t++) need = missing(&items[i], t);
        if (need) todo[ntodo++] = i;
    }
    st.total = ntodo;
    adb_t *adb = calloc((size_t)ntodo + 1, sizeof(adb_t));

    /* 1. informazioni ArcadeDB, 20 giochi per richiesta */
    st.phase = SCRAPE_INFO;
    for (int i = 0; i < ntodo && !st.cancel; i += 20) {
        char url[4096] = ADB;
        for (int k = i; k < ntodo && k < i + 20; k++) {
            size_t len = strlen(url);
            char *esc = curl_easy_escape(NULL, items[todo[k]].name, 0);
            snprintf(url + len, sizeof url - len, "%s%s", k > i ? "%3B" : "", esc ? esc : "");
            curl_free(esc);
        }
        buf_t b;
        if (!http_get(url, &b, NULL, 0)) {
            if (i == 0) { st.phase = SCRAPE_FAILED; goto out; }   /* nessuna rete o servizio fuori uso */
            continue;
        }
        jval *j = json_parse(b.data);
        jval *res = json_get(j, "result");
        for (jval *r = res && res->t == J_ARR ? res->child : NULL; r; r = r->next) {
            const char *name = json_str(r, "game_name", "");
            /* solo i giochi chiesti: il nome arriva dalla rete e finisce nel percorso di un file */
            bool asked = false;
            for (int k = i; k < ntodo && k < i + 20 && !asked; k++) asked = !strcmp(items[todo[k]].name, name);
            if (!asked) continue;
            char players[8];
            snprintf(players, sizeof players, "%d", (int)json_num(r, "players", 0));
            meta_set(name, json_str(r, "genre", ""), json_num(r, "players", 0) > 0 ? players : "");
            const char *hist = json_str(r, "history", NULL);
            if (hist && *hist) {
                char hp[1200];
                snprintf(hp, sizeof hp, "%s/Dats/history", root_dir);
                mkdir(hp, 0755);
                snprintf(hp, sizeof hp, "%s/Dats/history/%s.txt", root_dir, name);
                FILE *hf = fopen(hp, "w");
                if (hf) {
                    for (const char *c = hist; *c; c++)
                        if (*c != '\r') fputc(*c, hf);
                    fputc('\n', hf);
                    fclose(hf);
                }
            }
            for (int k = i; k < ntodo && k < i + 20; k++)
                if (!strcmp(items[todo[k]].name, name)) {
                    snprintf(adb[k].name, sizeof adb[k].name, "%s", name);
                    for (int t = 0; t < NTYPES; t++) snprintf(adb[k].urls[t], sizeof adb[k].urls[t], "%s", json_str(r, ADB_FIELDS[t], ""));
                }
        }
        json_free(j);
        free(b.data);
    }
    meta_save();

    /* 2. immagini: ArcadeDB, poi miniature libretro per titolo */
    st.phase = SCRAPE_IMAGES;
    for (int k = 0; k < ntodo && !st.cancel; k++) {
        item_t *it = &items[todo[k]];
        for (int t = 0; t < NTYPES && !st.cancel; t++) {
            if (!missing(it, t)) continue;
            media_path(p, sizeof p, t, it->name);
            bool ok = false;
            buf_t b;
            char ctype[128];
            if (adb[k].urls[t][0] && http_get(adb[k].urls[t], &b, ctype, sizeof ctype)) {
                ok = !strncmp(ctype, "image/", 6) && b.len > 200 && save_image(&b, p);
                free(b.data);
            }
            for (int repo = 0; !ok && LIBRETRO_KINDS[t] && repo < 2; repo++) {
                /* convenzione delle miniature libretro: questi caratteri diventano "_" */
                char safe[160];
                snprintf(safe, sizeof safe, "%s", it->title);
                for (char *c = safe; *c; c++)
                    if (strchr("&*/:`<>?\\|\"", *c)) *c = '_';
                const char *repos[2] = { "FBNeo - Arcade Games", "MAME" };
                if (!strcmp(it->core, "mame2003p")) { repos[0] = "MAME"; repos[1] = "FBNeo - Arcade Games"; }
                CURL *c = curl_easy_init();
                char *er = curl_easy_escape(c, repos[repo], 0), *en = curl_easy_escape(c, safe, 0);
                char url[1024];
                snprintf(url, sizeof url, "https://thumbnails.libretro.com/%s/%s/%s.png", er, LIBRETRO_KINDS[t], en);
                curl_free(er);
                curl_free(en);
                curl_easy_cleanup(c);
                if (http_get(url, &b, NULL, 0)) {
                    ok = b.len > 200 && save_image(&b, p);
                    free(b.data);
                }
            }
            if (ok) st.saved++;
            else {
                char key[96];
                snprintf(key, sizeof key, "%s/%s", TYPES[t], it->name);
                nf_add(key);
                st.missing++;
            }
        }
        st.done = k + 1;
        if ((k + 1) % 10 == 0) nf_save();
    }
    nf_save();
    st.phase = SCRAPE_DONE;
out:
    free(adb);
    free(todo);
    return 0;
}

bool scraper_start(const char *root, const game_t *games, int n) {
    if (thread) return false;
    memset(&st, 0, sizeof st);
    snprintf(root_dir, sizeof root_dir, "%s", root);
    free(items);
    items = malloc(sizeof(item_t) * (size_t)(n + 1));
    nitems = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(games[i].core, "linux") || games[i].missing) continue;
        bool dup = false;
        for (int k = 0; k < nitems && !dup; k++) dup = !strcmp(items[k].name, games[i].name);
        if (dup) continue;
        item_t *it = &items[nitems++];
        snprintf(it->name, sizeof it->name, "%s", games[i].name);
        snprintf(it->title, sizeof it->title, "%s", games[i].title);
        snprintf(it->core, sizeof it->core, "%s", games[i].core);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    st.phase = SCRAPE_INFO;
    thread = SDL_CreateThread(worker, "scraper", NULL);
    return thread != NULL;
}

void scraper_cancel(void) { st.cancel = true; }

const scrape_status_t *scraper_status(void) { return &st; }

void scraper_join(void) {
    if (!thread) return;
    SDL_WaitThread(thread, NULL);
    thread = NULL;
    for (int i = 0; i < nnotfound; i++) free(notfound[i]);
    free(notfound);
    notfound = NULL;
    nnotfound = cap_notfound = 0;
    free(meta);
    meta = NULL;
    nmeta = cap_meta = 0;
}
