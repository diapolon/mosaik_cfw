#include "scraper.h"
#include "../common/json.h"
#include "games.h"
#include "../common/durable.h"
#include "../runner/romzip.h"
#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

void ui_log(const char *fmt, ...);

#define UA "TrimUI-Arcade-Prototype/0.1"
#define ADB "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&use_parent=1&game_name="
#define META_HEADER "# arcadedb v3"   /* v3: con la storia in Dats/history/<rom>.txt */
#define MAX_SIDE 640

/* "box": la scatola, solo per le console (miniature libretro Named_Boxarts) */
static const char *TYPES[] = { "snap", "title", "marquee", "box" };
static const char *ADB_FIELDS[] = { "url_image_ingame", "url_image_title", "url_image_marquee", "" };
static const char *LIBRETRO_KINDS[] = { "Named_Snaps", "Named_Titles", NULL, "Named_Boxarts" };
#define NTYPES 4
#define T_BOX 3

/* exts: le estensioni delle ROM dell'emulatore senza zip ("nes|fds"), per leggere gli zip;
   canon: il nome No-Intro trovato col CRC nel database libretro */
typedef struct {
    char name[ROM_NAME], title[160], core[16], thumbs[64], exts[64], sysname[48], index[256], canon[160];
    char *path;
    bool console;
} item_t;
typedef struct { char name[ROM_NAME], genre[64], players[8]; } meta_t;

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

static long last_code;   /* risposta dell'ultima richiesta: 0 = nessuna risposta */

/* B durante una richiesta lenta: si interrompe subito, senza aspettare il timeout */
static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ud; (void)a; (void)b; (void)c; (void)d;
    return st.cancel ? 1 : 0;
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
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    char *ct = NULL;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    last_code = code;
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
    if (type == T_BOX && !it->console) return false;
    char p[1200], key[LIST_KEY];
    snprintf(key, sizeof key, "%s/%s", TYPES[type], it->name);
    if (nf_has(key)) return false;
    media_path(p, sizeof p, type, it->name);
    return !exists(p);
}

/* -------------------------------------------------------- lavoro */

typedef struct { char name[ROM_NAME], urls[NTYPES][512]; } adb_t;

/* Console: nomi da provare nelle miniature libretro, che usano i nomi No-Intro. Il titolo è il
   nome del file: così com'è, senza le note fra parentesi quadre dei set GoodTools ("[!]",
   "[T+Ita]"), e con le regioni GoodTools scritte come in No-Intro ("(E)" -> "(Europe)"). */
static int console_names(const char *title, char out[][160]) {
    int n = 0;
    snprintf(out[n++], 160, "%s", title);
    char b[160];
    size_t k = 0;
    int depth = 0;
    for (const char *c = title; *c && k < sizeof b - 1; c++) {
        if (*c == '[') depth++;
        else if (*c == ']') { if (depth) depth--; }
        else if (!depth) b[k++] = *c;
    }
    b[k] = 0;
    while (k && b[k - 1] == ' ') b[--k] = 0;
    if (strcmp(b, out[0])) snprintf(out[n++], 160, "%s", b);
    static const char *reg[][2] = { { "(U)", "(USA)" }, { "(E)", "(Europe)" }, { "(J)", "(Japan)" }, { "(W)", "(World)" },
        { "(UE)", "(USA, Europe)" }, { "(EU)", "(USA, Europe)" }, { "(JU)", "(Japan, USA)" }, { "(UJ)", "(Japan, USA)" },
        { "(JUE)", "(World)" }, { "(UEJ)", "(World)" }, { "(F)", "(France)" }, { "(G)", "(Germany)" }, { "(K)", "(Korea)" },
        { "(B)", "(Brazil)" }, { "(A)", "(Australia)" } };
    for (unsigned i = 0; i < sizeof reg / sizeof *reg; i++) {
        char *at = strstr(b, reg[i][0]);
        if (!at) continue;
        char r[160];
        snprintf(r, sizeof r, "%.*s%s%s", (int)(at - b), b, reg[i][1], at + strlen(reg[i][0]));
        snprintf(out[n++], 160, "%s", r);
        break;
    }
    return n;
}

/* ------------------------------------------ console: dati libretro */
/* Il database libretro (CC BY-SA 4.0) ha per ogni console un file per campo, con il CRC della
   ROM: anno, editore, sviluppatore, genere, giocatori. Si scaricano una volta (Dats/libretro,
   rinnovati dopo 30 giorni) e si cerca il CRC di ogni gioco: il nome No-Intro ("comment")
   diventa il titolo e serve anche a trovare le miniature dei file con nomi GoodTools. */
static const char *LR_FIELDS[] = { "releaseyear", "publisher", "developer", "genre", "maxusers" };
#define NLR 5
enum { LR_YEAR, LR_PUBLISHER, LR_DEVELOPER, LR_GENRE, LR_USERS };
typedef struct { uint32_t crc; char *name, *val; } lr_t;
typedef struct { lr_t *e; int n; char *buf; } lr_list_t;

static int lr_cmp(const void *a, const void *b) {
    uint32_t x = ((const lr_t *)a)->crc, y = ((const lr_t *)b)->crc;
    return x < y ? -1 : x > y;
}

/* valore dopo la chiave: "tra virgolette" o una parola */
static char *lr_value(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { char *e = strchr(++p, '"'); if (e) *e = 0; return p; }
    char *e = p + strcspn(p, " \t\r\n");
    *e = 0;
    return p;
}

static void lr_parse(lr_list_t *l, char *buf) {
    int cap = 0;
    l->buf = buf;
    char *name = NULL, *val = NULL;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "game (", 6)) name = val = NULL;
        else if (!strncmp(p, "comment ", 8)) name = lr_value(p + 8);
        else if (!strncmp(p, "rom (", 5)) {
            char *c = strstr(p, "crc ");
            if (c && name && val) {
                if (l->n == cap) l->e = realloc(l->e, sizeof(lr_t) * (size_t)(cap = cap ? cap * 2 : 1024));
                l->e[l->n++] = (lr_t){ (uint32_t)strtoul(c + 4, NULL, 16), name, val };
            }
        } else if (*p && *p != ')' && strncmp(p, "clrmamepro", 10) && strncmp(p, "name ", 5) && strncmp(p, "description ", 12)) {
            char *sp = p + strcspn(p, " \t");
            if (*sp) val = lr_value(sp);
        }
        line = nl ? nl + 1 : NULL;
    }
    if (l->n) qsort(l->e, (size_t)l->n, sizeof(lr_t), lr_cmp);
}

static const lr_t *lr_find(const lr_list_t *l, uint32_t crc) {
    lr_t key = { crc, NULL, NULL };
    return l->n ? bsearch(&key, l->e, (size_t)l->n, sizeof(lr_t), lr_cmp) : NULL;
}

/* il file del campo per la console: dalla scheda se recente, altrimenti dalla rete */
static char *lr_fetch(const char *field, const char *system) {
    char dir[1200], p[1400];
    snprintf(dir, sizeof dir, "%s/Dats/libretro", root_dir);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Dats/libretro/%s", root_dir, field);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/%s.dat", dir, system);
    struct stat sb;
    bool fresh = stat(p, &sb) == 0 && sb.st_size > 100 && time(NULL) - sb.st_mtime < 30 * 86400;
    if (!fresh) {
        CURL *c = curl_easy_init();
        char *es = curl_easy_escape(c, system, 0), url[512];
        snprintf(url, sizeof url, "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/%s/%s.dat", field, es ? es : "");
        curl_free(es);
        curl_easy_cleanup(c);
        buf_t b;
        if (http_get(url, &b, NULL, 0) && b.len > 100) {
            char tmp[1500];
            snprintf(tmp, sizeof tmp, "%s.tmp", p);
            FILE *f = fopen(tmp, "wb");
            bool ok = f && fwrite(b.data, 1, b.len, f) == b.len;
            if (f && fclose(f) != 0) ok = false;
            if (ok) rename(tmp, p); else remove(tmp);
        }
        free(b.data);
    }
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (buf) { n = (long)fread(buf, 1, (size_t)n, f); buf[n] = 0; }
    fclose(f);
    return buf;
}

static bool has_ext_ci(const char *name, const char *ext) {
    const char *d = strrchr(name, '.');
    return d && !strcasecmp(d + 1, ext);
}

/* CRC della ROM come nel database: senza l'intestazione iNES (16 byte) e senza quella da 512
   byte dei copiatori SNES e delle HuCard; per gli zip, la prima voce con un'estensione giusta */
static bool rom_crc(const item_t *it, uint32_t *out) {
    void *data = NULL;
    size_t n = 0;
    char inner[256] = "";
    if (romzip_is_zip(it->path)) {
        if (!it->exts[0] || romzip_extract(it->path, it->exts, &data, &n, inner, sizeof inner) != ROMZIP_OK) return false;
    } else {
        FILE *f = fopen(it->path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > (64L << 20) || !(data = malloc((size_t)len))) { fclose(f); return false; }
        n = fread(data, 1, (size_t)len, f);
        fclose(f);
        snprintf(inner, sizeof inner, "%s", it->path);
    }
    const unsigned char *d = data;
    size_t skip = 0;
    if (has_ext_ci(inner, "nes") && n > 16 && !memcmp(d, "NES\x1a", 4)) skip = 16;
    else if ((has_ext_ci(inner, "sfc") || has_ext_ci(inner, "smc") || has_ext_ci(inner, "fig") || has_ext_ci(inner, "swc")) && n % 1024 == 512) skip = 512;
    else if (has_ext_ci(inner, "pce") && n % 8192 == 512) skip = 512;
    *out = (uint32_t)crc32(0L, d + skip, (uInt)(n - skip));
    free(data);
    return true;
}

typedef struct { char name[ROM_NAME], line[512]; } ixrow_t;
static int ixrow_cmp(const void *a, const void *b) { return strcmp(((const ixrow_t *)a)->name, ((const ixrow_t *)b)->name); }

/* aggiunge o sostituisce le righe dei giochi trovati nell'indice della console (stesso formato
   degli arcade: nome, titolo, anno, produttore, sistema, verticale, bios, parent) */
static void index_update(const char *rel, ixrow_t *add, int nadd) {
    char p[1400], tmp[1410], line[600];
    snprintf(p, sizeof p, "%s/%s", root_dir, rel);
    ixrow_t *rows = NULL;
    int n = 0, cap = 0;
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        if (n == cap) rows = realloc(rows, sizeof(ixrow_t) * (size_t)(cap = cap ? cap * 2 : 256));
        snprintf(rows[n].name, sizeof rows[n].name, "%.*s", (int)(tab - line), line);
        snprintf(rows[n].line, sizeof rows[n].line, "%s", line);
        n++;
    }
    if (f) fclose(f);
    for (int a = 0; a < nadd; a++) {
        int k = 0;
        while (k < n && strcmp(rows[k].name, add[a].name)) k++;
        if (k == n) {
            if (n == cap) rows = realloc(rows, sizeof(ixrow_t) * (size_t)(cap = cap ? cap * 2 : 256));
            n++;
        }
        rows[k] = add[a];
    }
    if (n) qsort(rows, (size_t)n, sizeof(ixrow_t), ixrow_cmp);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    f = fopen(tmp, "w");
    bool ok = f != NULL;
    for (int k = 0; ok && k < n; k++) ok = fprintf(f, "%s\n", rows[k].line) > 0;
    if (f && fclose(f) != 0) ok = false;
    if (ok) durable_replace(tmp, p); else remove(tmp);
    free(rows);
}

static void clean_field(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

/* titolo, anno, editore, genere e giocatori dei giochi per console, console per console */
static void console_info(const int *todo, int ntodo) {
    for (int k = 0; k < ntodo && !st.cancel; k++) {
        item_t *first = &items[todo[k]];
        if (!first->console || !first->thumbs[0]) continue;
        /* già fatto per questa console in un giro precedente del ciclo */
        bool seen = false;
        for (int q = 0; q < k && !seen; q++) seen = items[todo[q]].console && !strcmp(items[todo[q]].thumbs, first->thumbs) && !strcmp(items[todo[q]].core, first->core);
        if (seen) continue;
        lr_list_t lists[NLR];
        memset(lists, 0, sizeof lists);
        for (int fi = 0; fi < NLR && !st.cancel; fi++) {
            char *buf = lr_fetch(LR_FIELDS[fi], first->thumbs);
            if (buf) lr_parse(&lists[fi], buf);
        }
        ixrow_t *rows = malloc(sizeof(ixrow_t) * (size_t)(ntodo + 1));
        int nrows = 0, found = 0, total = 0;
        for (int q = k; q < ntodo && !st.cancel; q++) {
            item_t *it = &items[todo[q]];
            if (!it->console || strcmp(it->thumbs, first->thumbs) || strcmp(it->core, first->core)) continue;
            total++;
            uint32_t crc;
            if (!rom_crc(it, &crc)) continue;
            const lr_t *v[NLR];
            for (int fi = 0; fi < NLR; fi++) v[fi] = lr_find(&lists[fi], crc);
            /* il nome come nei file No-Intro: anni, generi e giocatori lo scrivono così ("A - B"),
               editori e sviluppatori a volte con i due punti */
            static const int order[NLR] = { LR_YEAR, LR_GENRE, LR_USERS, LR_DEVELOPER, LR_PUBLISHER };
            const char *canon = NULL;
            for (int o = 0; o < NLR && !canon; o++) if (v[order[o]]) canon = v[order[o]]->name;
            if (!canon) continue;
            found++;
            snprintf(it->canon, sizeof it->canon, "%s", canon);
            char title[160], year[16], maker[96], sys[48];
            snprintf(title, sizeof title, "%s", canon);
            snprintf(year, sizeof year, "%s", v[LR_YEAR] ? v[LR_YEAR]->val : "");
            snprintf(maker, sizeof maker, "%s", v[LR_PUBLISHER] ? v[LR_PUBLISHER]->val : v[LR_DEVELOPER] ? v[LR_DEVELOPER]->val : "");
            snprintf(sys, sizeof sys, "%s", it->sysname);
            clean_field(title); clean_field(year); clean_field(maker); clean_field(sys);
            snprintf(rows[nrows].name, sizeof rows[nrows].name, "%s", it->name);
            snprintf(rows[nrows].line, sizeof rows[nrows].line, "%s\t%s\t%s\t%s\t%s\t0\t0\t", it->name, title, year, maker, sys);
            nrows++;
            char genre[64], players[8];
            snprintf(genre, sizeof genre, "%s", v[LR_GENRE] ? v[LR_GENRE]->val : "");
            snprintf(players, sizeof players, "%s", v[LR_USERS] ? v[LR_USERS]->val : "");
            clean_field(genre);
            meta_set(it->name, genre, players);
        }
        if (nrows) index_update(first->index, rows, nrows);
        ui_log("scraper: %s, %d giochi su %d trovati nel database libretro", first->thumbs, found, total);
        free(rows);
        for (int fi = 0; fi < NLR; fi++) { free(lists[fi].e); free(lists[fi].buf); }
    }
}

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
        /* arcade: informazioni ArcadeDB; console: dati libretro, trovati se c'è il genere */
        bool need = !meta_find(items[i].name);
        for (int t = 0; t < NTYPES && !need; t++) need = missing(&items[i], t);
        if (need) todo[ntodo++] = i;
    }
    st.total = ntodo;
    adb_t *adb = calloc((size_t)ntodo + 1, sizeof(adb_t));

    /* 1. informazioni ArcadeDB (solo arcade), 20 giochi per richiesta */
    st.phase = SCRAPE_INFO;
    int *arc = malloc(sizeof(int) * (size_t)(ntodo + 1)), narc = 0;
    for (int k = 0; k < ntodo; k++)
        if (!items[todo[k]].console) arc[narc++] = k;
    for (int i = 0; i < narc && !st.cancel; i += 20) {
        char url[4096] = ADB;
        for (int q = i; q < narc && q < i + 20; q++) {
            size_t len = strlen(url);
            char *esc = curl_easy_escape(NULL, items[todo[arc[q]]].name, 0);
            snprintf(url + len, sizeof url - len, "%s%s", q > i ? "%3B" : "", esc ? esc : "");
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
            for (int q = i; q < narc && q < i + 20 && !asked; q++) asked = !strcmp(items[todo[arc[q]]].name, name);
            if (!asked) continue;
            char players[8];
            snprintf(players, sizeof players, "%d", json_int(r, "players", 0));
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
            for (int q = i; q < narc && q < i + 20; q++) {
                int k = arc[q];
                if (!strcmp(items[todo[k]].name, name)) {
                    snprintf(adb[k].name, sizeof adb[k].name, "%s", name);
                    for (int t = 0; t < NTYPES; t++) snprintf(adb[k].urls[t], sizeof adb[k].urls[t], "%s", json_str(r, ADB_FIELDS[t], ""));
                }
            }
        }
        json_free(j);
        free(b.data);
    }
    free(arc);
    /* console: dati dal database libretro, cercati col CRC delle ROM */
    console_info(todo, ntodo);
    meta_save();

    /* 2. immagini: ArcadeDB, poi miniature libretro per titolo */
    st.phase = SCRAPE_IMAGES;
    for (int k = 0; k < ntodo && !st.cancel; k++) {
        item_t *it = &items[todo[k]];
        char names[4][160];
        int nnames = it->console ? console_names(it->title, names) : 1, first = 0;
        if (!it->console) snprintf(names[0], sizeof names[0], "%s", it->title);
        /* col nome No-Intro trovato col CRC le miniature si trovano anche per i file GoodTools */
        if (it->console && it->canon[0]) {
            snprintf(names[0], sizeof names[0], "%s", it->canon);
            nnames = 1;   /* è il nome esatto delle miniature: gli altri non servono */
        }
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
            /* arcade: due raccolte con il titolo; console: la sua raccolta con più nomi possibili,
               cominciando da quello che ha già funzionato per un altro tipo d'immagine */
            int ntries = it->console ? (it->thumbs[0] ? nnames : 0) : 2;
            for (int try = 0; !ok && LIBRETRO_KINDS[t] && try < ntries; try++) {
                int ni = it->console ? (first + try) % nnames : 0;
                /* convenzione delle miniature libretro: questi caratteri diventano "_" */
                char safe[160];
                snprintf(safe, sizeof safe, "%s", names[ni]);
                for (char *c = safe; *c; c++)
                    if (strchr("&*/:`<>?\\|\"", *c)) *c = '_';
                const char *repos[2] = { "FBNeo - Arcade Games", "MAME" };
                if (!strcmp(it->core, "mame2003p")) { repos[0] = "MAME"; repos[1] = "FBNeo - Arcade Games"; }
                const char *repo_name = it->console ? it->thumbs : repos[try];
                CURL *c = curl_easy_init();
                char *er = curl_easy_escape(c, repo_name, 0), *en = curl_easy_escape(c, safe, 0);
                char url[1024];
                snprintf(url, sizeof url, "https://thumbnails.libretro.com/%s/%s/%s.png", er, LIBRETRO_KINDS[t], en);
                curl_free(er);
                curl_free(en);
                curl_easy_cleanup(c);
                if (http_get(url, &b, NULL, 0)) {
                    ok = b.len > 200 && save_image(&b, p);
                    free(b.data);
                    if (ok && it->console) first = ni;
                }
            }
            if (ok) st.saved++;
            else {
                char key[LIST_KEY];
                snprintf(key, sizeof key, "%s/%s", TYPES[t], it->name);
                if (last_code == 404 || last_code == 200) nf_add(key);   /* non un errore di rete: si riproverà */
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
    for (int i = 0; i < nitems; i++) free(items[i].path);
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
        const emulator_t *e = emulator_find(games[i].core);
        it->console = e && e->console;
        snprintf(it->thumbs, sizeof it->thumbs, "%s", e ? e->thumbs : "");
        snprintf(it->sysname, sizeof it->sysname, "%s", e ? e->name : "");
        snprintf(it->index, sizeof it->index, "%s", e ? e->index : "");
        it->canon[0] = 0;
        it->path = strdup(games[i].path ? games[i].path : "");
        it->exts[0] = 0;
        for (int x = 0; e && x < e->nexts; x++) {
            const char *ext = e->exts[x] + 1;   /* senza il punto */
            if (!strcasecmp(ext, "zip") || !strcasecmp(ext, "7z")) continue;
            size_t l = strlen(it->exts);
            snprintf(it->exts + l, sizeof it->exts - l, "%s%s", l ? "|" : "", ext);
        }
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
