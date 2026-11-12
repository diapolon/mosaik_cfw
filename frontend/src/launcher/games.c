#include "games.h"
#include "../common/json.h"
#include "../common/ui.h"
#include "../common/durable.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>

/* ----------------------------------------------------------- emulatori */

emulator_t EMUS[MAX_EMUS];
int NEMUS;
emulator_t PLANNED[16];
int NPLANNED;

static int emu_cmp(const void *a, const void *b) {
    const emulator_t *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

int emulators_load(const char *root) {
    char dirp[1100];
    snprintf(dirp, sizeof dirp, "%s/Emulators", root);
    NEMUS = 0;
    NPLANNED = 0;
    DIR *d = opendir(dirp);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        /* i file che iniziano con un punto sono nascosti (anche i ._ di macOS) */
        if (e->d_name[0] == '.' || len < 6 || len > 20 || strcmp(e->d_name + len - 5, ".json")) continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dirp, e->d_name);
        jval *j = json_load(p);
        if (!j) continue;
        bool planned = json_bool(j, "planned", false);
        if (planned ? NPLANNED >= 16 : NEMUS >= MAX_EMUS) { json_free(j); continue; }
        emulator_t *m = planned ? &PLANNED[NPLANNED++] : &EMUS[NEMUS++];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%.*s", (int)(len - 5), e->d_name);
        char upper[16];
        for (size_t i = 0; i <= strlen(m->id); i++) upper[i] = (char)(m->id[i] >= 'a' && m->id[i] <= 'z' ? m->id[i] - 32 : m->id[i]);
        char def[300];
        snprintf(m->name, sizeof m->name, "%s", json_str(j, "name", m->id));
        snprintf(def, sizeof def, "%s_libretro", m->id);
        snprintf(m->core, sizeof m->core, "%s", json_str(j, "core", def));
        snprintf(def, sizeof def, "Roms/%s", upper);
        snprintf(m->roms, sizeof m->roms, "%s", json_str(j, "roms", def));
        snprintf(def, sizeof def, "Dats/%s.tsv", m->id);
        snprintf(m->index, sizeof m->index, "%s", json_str(j, "index", def));
        m->order = json_int(j, "order", 100);
        m->console = !strcmp(json_str(j, "kind", ""), "console");
        snprintf(m->thumbs, sizeof m->thumbs, "%s", json_str(j, "thumbnails", ""));
        snprintf(m->shortname, sizeof m->shortname, "%s", json_str(j, "short", upper));
        snprintf(m->art, sizeof m->art, "%s", json_str(j, "art", ""));
        snprintf(m->launch, sizeof m->launch, "%s", json_str(j, "launch", ""));
        m->nomenu = !json_bool(j, "menu", true);
        m->planned = planned;
        jval *bi = json_get(j, "bios");
        for (jval *v = bi && bi->t == J_ARR ? bi->child : NULL; v && m->nbios < 4; v = v->next)
            if (v->t == J_STR && !strchr(v->s, '/')) snprintf(m->bios[m->nbios++], sizeof m->bios[0], "%s", v->s);
        jval *ex = json_get(j, "extensions");
        for (jval *v = ex && ex->t == J_ARR ? ex->child : NULL; v && m->nexts < 8; v = v->next)
            if (v->t == J_STR) snprintf(m->exts[m->nexts++], sizeof m->exts[0], ".%s", v->s);
        if (!m->nexts) {
            strcpy(m->exts[m->nexts++], ".zip");
            strcpy(m->exts[m->nexts++], ".7z");
        }
        json_free(j);
        /* cartella delle ROM: chi aggiorna via Wi-Fi riceve l'emulatore nuovo ma non la
           cartella. Si crea solo dentro la SD e solo se manca (mkdir di una cartella che c'è
           non scrive nulla); un percorso assoluto è dell'utente e non si tocca. */
        if (!planned && m->roms[0] && m->roms[0] != '/' && !strstr(m->roms, "..")) {
            char rp[600];
            snprintf(rp, sizeof rp, "%s/%s", root, m->roms);
            for (char *q = rp + strlen(root) + 1; *q; q++)
                if (*q == '/') { *q = 0; mkdir(rp, 0755); *q = '/'; }
            mkdir(rp, 0755);
        }
    }
    closedir(d);
    qsort(EMUS, (size_t)NEMUS, sizeof(emulator_t), emu_cmp);
    qsort(PLANNED, (size_t)NPLANNED, sizeof(emulator_t), emu_cmp);
    return NEMUS;
}

const emulator_t *emulator_find(const char *id) {
    for (int i = 0; i < NEMUS; i++)
        if (!strcmp(EMUS[i].id, id)) return &EMUS[i];
    return NULL;
}

const char *games_core_label(const char *core) {
    const emulator_t *m = emulator_find(core);
    if (m) return m->name;
    return !strcmp(core, "linux") ? "Linux" : core;
}

/* ------------------------------------------------------------- indice */
/* Dats/<id>.tsv: nome, titolo, anno, produttore, sistema, verticale, bios[, parent] */

typedef struct { char *name, *title, *year, *maker, *system, *parent; bool vertical, bios, present; } entry_t;
typedef struct { entry_t *e; int n; char *buf; } index_t;

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static void index_load(index_t *ix, const char *path) {
    memset(ix, 0, sizeof *ix);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    ix->buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (!ix->buf) { fclose(f); ui_log("indice %s: memoria esaurita", path); return; }
    len = (long)fread(ix->buf, 1, (size_t)len, f);
    ix->buf[len] = 0;
    fclose(f);
    int cap = 0;
    for (long i = 0; i < len; i++) cap += ix->buf[i] == '\n';
    ix->e = calloc((size_t)cap + 1, sizeof(entry_t));
    if (!ix->e) { free(ix->buf); ix->buf = NULL; ui_log("indice %s: memoria esaurita", path); return; }
    char *line = ix->buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *col[8] = { 0 };
        int c = 0;
        for (char *p = line; c < 8; c++) {
            col[c] = p;
            char *tab = strchr(p, '\t');
            if (!tab) { c++; break; }
            *tab = 0;
            p = tab + 1;
        }
        if (c >= 7) {
            entry_t *e = &ix->e[ix->n++];
            e->name = col[0]; e->title = col[1]; e->year = col[2]; e->maker = col[3]; e->system = col[4];
            e->vertical = col[5][0] == '1';
            e->bios = col[6][0] == '1';
            e->parent = c >= 8 ? col[7] : "";
        }
        line = nl ? nl + 1 : NULL;
    }
    /* l'indice è già ordinato dallo script, ma un ordinamento qui non costa nulla */
    qsort(ix->e, (size_t)ix->n, sizeof(entry_t), entry_cmp);
}

static entry_t *index_find(index_t *ix, const char *name) {
    entry_t key = { .name = (char *)name };
    return ix->n ? bsearch(&key, ix->e, (size_t)ix->n, sizeof(entry_t), entry_cmp) : NULL;
}

/* -------------------------------------------------------- scansione */

typedef struct { game_t *g; int n, cap; } list_t;

/* NULL se la memoria non basta: chi chiama smette di aggiungere (audit BUG-008) */
static game_t *push(list_t *l) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 256;
        game_t *ng = realloc(l->g, sizeof(game_t) * (size_t)nc);
        if (!ng) { ui_log("libreria: memoria esaurita a %d giochi", l->n); return NULL; }
        l->g = ng;
        l->cap = nc;
    }
    game_t *g = &l->g[l->n++];
    memset(g, 0, sizeof *g);
    return g;
}

/* Identità dei giochi di un emulatore (audit prelancio R04): salvataggi, stati e statistiche
   usano il nome della ROM senza estensione, quindi due ROM diverse con lo stesso nome (in due
   sottocartelle, o .zip e ROM nella stessa) li condividevano. La prima (meno annidata, poi in
   ordine di percorso) tiene il nome e i file che ha già; le altre ricevono "nome [sottocartella]"
   o "nome [estensione]", anche nel titolo, così in libreria si distinguono.
   Verifica audit BUG-003: quel nome poteva ancora coincidere (Game.nes, Region/Game.nes e
   Region/Game.zip davano due "Game [Region]"; A/B e A-B entrambi "A-B"; un file chiamato
   letteralmente "Game [Region]"). Le chiavi uguali ora si separano: tiene la chiave chi l'ha
   già (prima chi non è stato rinominato, poi il meno annidato e il percorso minore, quindi i
   suoi file restano suoi), gli altri aggiungono l'estensione e poi un numero. Le chiavi senza
   coincidenze non cambiano, così nessun salvataggio esistente va spostato.
   BUG-009: gruppi trovati ordinando (n log n), non confrontando ogni coppia. */
static int path_depth(const char *p) { int d = 0; for (; *p; p++) d += *p == '/'; return d; }

typedef struct { game_t *g; const game_t *keep; int depth, level; size_t tl; } keyref_t;

static int ref_name_cmp(const void *a, const void *b) {
    const keyref_t *x = a, *y = b;
    int c = strcmp(x->g->name, y->g->name);
    if (c) return c;
    if (x->depth != y->depth) return x->depth - y->depth;
    return strcmp(x->g->path, y->g->path);
}

static int ref_key_cmp(const void *a, const void *b) {
    const keyref_t *x = a, *y = b;
    int c = strcmp(x->g->key, y->g->key);
    if (c) return c;
    if ((x->level >= 0) != (y->level >= 0)) return x->level >= 0 ? 1 : -1;   /* chi tiene il nome vince */
    if (x->level != y->level) return x->level - y->level;                      /* poi chi ha la chiave da prima */
    if (x->depth != y->depth) return x->depth - y->depth;
    return strcmp(x->g->path, y->g->path);
}

/* livello 0: la regola storica; 1: con l'estensione; da 2: anche un numero */
static void key_make(keyref_t *r, const char *dir, size_t dl) {
    game_t *g = r->g;
    char sub[160] = "", ext[16];
    const char *rel = !strncmp(g->path, dir, dl) && g->path[dl] == '/' ? g->path + dl + 1 : g->path;
    const char *slash = strrchr(rel, '/');
    if (slash) {   /* sottocartella rispetto alla cartella dell'emulatore, "/" -> "-" */
        snprintf(sub, sizeof sub, "%.*s", (int)(slash - rel), rel);
        for (char *c = sub; *c; c++) if (*c == '/') *c = '-';
    }
    const char *dot = strrchr(g->path, '.'), *last = strrchr(g->path, '/');
    snprintf(ext, sizeof ext, "%s", dot && (!last || dot > last) ? dot + 1 : "2");
    /* stessa cartella del primo: si distingue con l'estensione */
    const game_t *k = r->keep ? r->keep : g;
    const char *keep_slash = strrchr(k->path, '/'), *my_slash = last;
    size_t kd = keep_slash ? (size_t)(keep_slash - k->path) : 0, md = my_slash ? (size_t)(my_slash - g->path) : 0;
    if (kd == md && !strncmp(k->path, g->path, kd)) snprintf(sub, sizeof sub, "%s", ext);
    if (!sub[0]) snprintf(sub, sizeof sub, "2");
    if (r->level == 0) {
        snprintf(g->key, sizeof g->key, "%s [%s]", g->name, sub);
    } else {
        /* la parte che distingue sta in fondo e non va tagliata: si accorcia il resto */
        char tail[200];
        const char *s = strlen(sub) > 60 ? sub + strlen(sub) - 60 : sub;
        bool same = !strcmp(s, ext);
        if (r->level == 1) snprintf(tail, sizeof tail, same ? "%s 2" : "%s %s", s, ext);
        else if (same) snprintf(tail, sizeof tail, "%s %d", s, r->level + 1);
        else snprintf(tail, sizeof tail, "%s %s %d", s, ext, r->level + 1);
        int room = (int)sizeof g->key - (int)strlen(tail) - 4;
        snprintf(g->key, sizeof g->key, "%.*s [%s]", room > 0 ? room : 0, g->name, tail);
        snprintf(sub, sizeof sub, "%s", tail);
    }
    g->title[r->tl] = 0;
    snprintf(g->title + r->tl, sizeof g->title - r->tl, " [%s]", sub);
}

static void assign_keys(game_t *g, int n, const char *dir) {
    if (n <= 0) return;
    keyref_t *r = malloc(sizeof *r * (size_t)n);
    for (int i = 0; i < n; i++) snprintf(g[i].key, sizeof g[i].key, "%s", g[i].name);
    if (!r) return;   /* senza memoria: le chiavi di sempre */
    size_t dl = strlen(dir);
    for (int i = 0; i < n; i++)
        r[i] = (keyref_t){ .g = &g[i], .depth = path_depth(g[i].path), .level = -1, .tl = strlen(g[i].title) };
    qsort(r, (size_t)n, sizeof *r, ref_name_cmp);
    for (int i = 0, j; i < n; i = j) {
        for (j = i + 1; j < n && !strcmp(r[j].g->name, r[i].g->name); j++) {
            r[j].keep = r[i].g;
            r[j].level = 0;
            key_make(&r[j], dir, dl);
        }
    }
    for (int round = 0; round < 16; round++) {
        qsort(r, (size_t)n, sizeof *r, ref_key_cmp);
        bool again = false;
        for (int i = 0, j; i < n; i = j) {
            for (j = i + 1; j < n && !strcmp(r[j].g->key, r[i].g->key); j++) {
                if (!r[j].keep) r[j].keep = r[i].g;
                r[j].level = r[j].level < 0 ? 1 : r[j].level + 1;
                key_make(&r[j], dir, dl);
                again = true;
            }
        }
        if (!again) break;
    }
    free(r);
}

static bool has_ext(const char *f, const char *ext) {
    size_t a = strlen(f), b = strlen(ext);
    return a > b && !strcasecmp(f + a - b, ext);
}

static void fill_from_entry(game_t *g, const entry_t *en) {
    snprintf(g->title, sizeof g->title, "%s", en->title);
    snprintf(g->year, sizeof g->year, "%s", en->year);
    snprintf(g->maker, sizeof g->maker, "%s", en->maker);
    snprintf(g->system, sizeof g->system, "%s", en->system);
    g->vertical = en->vertical;
}

/* Giochi su più dischi: un .m3u elenca i dischi (uno per riga, accanto al file). In elenco va
   solo il .m3u; i dischi che elenca non si mostrano da soli (sarebbero doppioni). */
#define M3U_MAX 32
static int m3u_listed(const char *dir, const emulator_t *emu, char out[][256]) {
    bool m3u = false;
    for (int i = 0; i < emu->nexts && !m3u; i++) m3u = !strcasecmp(emu->exts[i], ".m3u");
    if (!m3u) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < M3U_MAX) {
        if (e->d_name[0] == '.' || !has_ext(e->d_name, ".m3u")) continue;
        char p[2048], line[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "r");
        while (f && n < M3U_MAX && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || line[0] == '#' || strchr(line, '/')) continue;   /* solo file accanto al .m3u */
            snprintf(out[n++], 256, "%s", line);
        }
        if (f) fclose(f);
    }
    closedir(d);
    return n;
}

static void scan_dir(list_t *l, index_t *ix, const emulator_t *emu, const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    char listed[M3U_MAX][256];   /* per cartella: la ricorsione non deve sovrascriverlo */
    int nlisted = m3u_listed(dir, emu, listed);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) scan_dir(l, ix, emu, p, depth + 1);
            continue;
        }
        bool ok = false;
        for (int i = 0; i < emu->nexts && !ok; i++) ok = has_ext(e->d_name, emu->exts[i]);
        if (!ok) continue;
        bool in_m3u = false;
        for (int i = 0; i < nlisted && !in_m3u; i++) in_m3u = !strcmp(listed[i], e->d_name);
        if (in_m3u) continue;
        char name[ROM_NAME];
        snprintf(name, sizeof name, "%.*s", (int)(strrchr(e->d_name, '.') - e->d_name), e->d_name);
        entry_t *en = index_find(ix, name);
        if (en && en->bios) continue;   /* i BIOS servono ai giochi, non si avviano */
        if (en) en->present = true;
        game_t *g = push(l);
        if (!g) break;
        snprintf(g->name, sizeof g->name, "%s", name);
        snprintf(g->core, sizeof g->core, "%s", emu->id);
        g->path = strdup(p);
        if (!g->path) { l->n--; break; }
        if (en) fill_from_entry(g, en);
        else {
            snprintf(g->title, sizeof g->title, "%s", name);
            snprintf(g->system, sizeof g->system, "%s", emu->name);
            /* console senza indice: il titolo è il nome del file, senza le note dei set
               GoodTools fra parentesi quadre ("[!]", "[T+Ita]"); la regione resta */
            if (emu->console) {
                char *w = g->title;
                int depth = 0;
                for (const char *c = g->title; *c; c++) {
                    if (*c == '[') depth++;
                    else if (*c == ']') { if (depth) depth--; }
                    else if (!depth) *w++ = *c;
                }
                *w = 0;
                while (w > g->title && w[-1] == ' ') *--w = 0;
                if (!g->title[0]) snprintf(g->title, sizeof g->title, "%s", name);
            }
        }
    }
    closedir(d);
}

/* Giochi Linux: Linux/<cartella>/launch.sh, con game.json e cover.png facoltativi. */
static void scan_linux(list_t *l, const char *root) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Linux", root);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sh[2048], meta[2048];
        snprintf(sh, sizeof sh, "%s/%s/launch.sh", dir, e->d_name);
        struct stat st;
        if (stat(sh, &st) != 0) continue;
        game_t *g = push(l);
        if (!g) break;
        snprintf(g->name, sizeof g->name, "%s", e->d_name);
        snprintf(g->core, sizeof g->core, "linux");
        g->path = strdup(sh);
        if (!g->path) { l->n--; break; }
        snprintf(g->system, sizeof g->system, "Linux");
        snprintf(meta, sizeof meta, "%s/%s/game.json", dir, e->d_name);
        jval *j = json_load(meta);
        snprintf(g->title, sizeof g->title, "%s", json_str(j, "title", e->d_name));
        snprintf(g->year, sizeof g->year, "%s", json_str(j, "year", ""));
        snprintf(g->maker, sizeof g->maker, "%s", json_str(j, "author", ""));
        json_free(j);
    }
    closedir(d);
}

/* Genere e giocatori scritti dallo scraper: "nome<TAB>genere<TAB>giocatori", ordinato per nome. */
typedef struct { char name[ROM_NAME + 20], genre[32]; int players; } meta_t;

static int meta_cmp(const void *a, const void *b) {
    return strcmp(((const meta_t *)a)->name, ((const meta_t *)b)->name);
}

static void load_genres(const char *root, game_t *g, int n) {
    char p[1100], line[256];
    snprintf(p, sizeof p, "%s/Dats/arcadedb.tsv", root);
    FILE *f = fopen(p, "r");
    if (!f) return;
    int cap = 1024, m = 0;
    meta_t *meta = malloc(sizeof(meta_t) * (size_t)cap);
    if (!meta) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *g1 = strchr(line, '\t');
        if (!g1) continue;
        *g1++ = 0;
        char *g2 = strchr(g1, '\t');
        if (g2) *g2++ = 0;
        if (m == cap) {
            meta_t *nm = realloc(meta, sizeof(meta_t) * (size_t)(cap * 2));
            if (!nm) break;
            meta = nm;
            cap *= 2;
        }
        snprintf(meta[m].name, sizeof meta[m].name, "%s", line);
        snprintf(meta[m].genre, sizeof meta[m].genre, "%s", g1);
        meta[m].players = g2 ? atoi(g2) : 0;
        m++;
    }
    fclose(f);
    qsort(meta, (size_t)m, sizeof(meta_t), meta_cmp);
    for (int i = 0; i < n; i++) {
        meta_t key, *hit = NULL;
        /* console: "<console>/<nome>" (issue #5), poi il solo nome scritto dalle versioni prima */
        const emulator_t *e = emulator_find(g[i].core);
        if (e && e->console) {
            snprintf(key.name, sizeof key.name, "%s/%s", g[i].core, g[i].name);
            hit = bsearch(&key, meta, (size_t)m, sizeof(meta_t), meta_cmp);
        }
        snprintf(key.name, sizeof key.name, "%s", g[i].name);
        if (hit || (hit = bsearch(&key, meta, (size_t)m, sizeof(meta_t), meta_cmp))) {
            snprintf(g[i].genre, sizeof g[i].genre, "%s", hit->genre);
            g[i].players = hit->players;
        }
    }
    free(meta);
}

/* ------------------------------------------------- generi modificati dall'utente */
typedef struct { char key[64], val[32]; } kv_t;

/* -1 con la memoria esaurita: un file letto a metà non va riscritto (perderebbe le altre righe) */
static int kv_load(const char *path, kv_t **out) {
    int n = 0, cap = 64;
    kv_t *a = malloc(sizeof(kv_t) * (size_t)cap);
    *out = NULL;
    if (!a) return -1;
    FILE *f = fopen(path, "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '\t');
        if (!v || !line[0] || !v[1]) continue;
        *v++ = 0;
        if (n == cap) {
            kv_t *na = realloc(a, sizeof(kv_t) * (size_t)(cap * 2));
            if (!na) { fclose(f); free(a); return -1; }
            a = na;
            cap *= 2;
        }
        snprintf(a[n].key, sizeof a[n].key, "%s", line);
        snprintf(a[n].val, sizeof a[n].val, "%s", v);
        n++;
    }
    if (f) fclose(f);
    *out = a;
    return n;
}

/* Il file provvisorio prende il posto di quello vero solo se è arrivato tutto sulla scheda:
   se la scrittura si interrompe, resta l'elenco di prima. */
static void commit_file(FILE *f, const char *tmp, const char *path) {
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok && durable_replace(tmp, path) == 0) return;
    ui_log("scrittura di %s non riuscita: resta il file di prima", path);
    remove(tmp);
}

static void kv_save(const char *path, const kv_t *a, int n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) if (a[i].key[0]) fprintf(f, "%s\t%s\n", a[i].key, a[i].val);
    commit_file(f, tmp, path);
}

static void genre_file(const char *root, const char *name, char *out, size_t n) { snprintf(out, n, "%s/System/%s", root, name); }

void games_set_genre(const char *root, const char *rom, const char *genre) {
    char p[1100];
    genre_file(root, "genre-custom.tsv", p, sizeof p);
    kv_t *a;
    int n = kv_load(p, &a), i;
    if (n < 0) return;
    for (i = 0; i < n; i++) if (!strcmp(a[i].key, rom)) break;
    if (i == n && genre) {
        kv_t *na = realloc(a, sizeof(kv_t) * (size_t)(n + 1));
        if (!na) { free(a); return; }
        a = na;
        snprintf(a[n].key, sizeof a[n].key, "%s", rom);
        n++;
    }
    if (i < n) {
        if (genre) snprintf(a[i].val, sizeof a[i].val, "%s", genre);
        else a[i].key[0] = 0;
    }
    kv_save(p, a, n);
    free(a);
}

void games_group_genre(const char *root, const char *from, const char *to) {
    if (!strcmp(from, to)) return;
    char p[1100];
    genre_file(root, "genre-groups.tsv", p, sizeof p);
    kv_t *a;
    int n = kv_load(p, &a), found = 0;
    if (n < 0) return;
    /* chi puntava a "from" ora punta a "to"; un genere non punta mai a sé stesso */
    for (int i = 0; i < n; i++) {
        if (!strcmp(a[i].val, from)) snprintf(a[i].val, sizeof a[i].val, "%s", to);
        if (!strcmp(a[i].key, from)) { snprintf(a[i].val, sizeof a[i].val, "%s", to); found = 1; }
        if (!strcmp(a[i].key, a[i].val)) a[i].key[0] = 0;
    }
    if (!found) {
        kv_t *na = realloc(a, sizeof(kv_t) * (size_t)(n + 1));
        if (!na) { free(a); return; }
        a = na;
        snprintf(a[n].key, sizeof a[n].key, "%s", from);
        snprintf(a[n].val, sizeof a[n].val, "%s", to);
        n++;
    }
    kv_save(p, a, n);
    free(a);
}

void games_clear_genre_groups(const char *root) {
    char p[1100];
    genre_file(root, "genre-groups.tsv", p, sizeof p);
    unlink(p);
}

/* dopo i generi dello scraper: prima quelli scelti per il singolo gioco, poi accorpamenti e nomi */
static void apply_genre_edits(const char *root, game_t *g, int n) {
    char p[1100];
    kv_t *a;
    genre_file(root, "genre-custom.tsv", p, sizeof p);
    int na = kv_load(p, &a);
    for (int i = 0; i < n && na; i++)
        for (int k = 0; k < na; k++)
            if (!strcmp(a[k].key, g[i].name)) { snprintf(g[i].genre, sizeof g[i].genre, "%s", a[k].val); break; }
    free(a);
    genre_file(root, "genre-groups.tsv", p, sizeof p);
    na = kv_load(p, &a);
    for (int i = 0; i < n && na; i++) {
        char m[32];
        snprintf(m, sizeof m, "%s", g[i].genre);
        char *slash = strchr(m, '/');
        if (slash) *slash = 0;
        for (size_t e = strlen(m); e > 0 && m[e - 1] == ' '; e--) m[e - 1] = 0;
        for (int k = 0; k < na; k++)
            if (!strcmp(a[k].key, m)) { snprintf(g[i].genre, sizeof g[i].genre, "%s", a[k].val); break; }
    }
    free(a);
}

static int title_cmp(const void *a, const void *b) {
    const game_t *x = a, *y = b;
    int c = strcasecmp(x->title, y->title);
    return c ? c : strcmp(x->core, y->core);
}

int games_scan(const char *root, game_t **out, bool include_missing) {
    list_t l = { 0 };
    for (int i = 0; i < NEMUS; i++) {
        char ixp[1400], dir[1400];
        index_t ix;
        snprintf(ixp, sizeof ixp, "%s/%s", root, EMUS[i].index);
        /* percorso relativo alla cartella del sistema, oppure assoluto (es. /mnt/SDCARD/Roms/FBNEO) */
        if (EMUS[i].roms[0] == '/') snprintf(dir, sizeof dir, "%s", EMUS[i].roms);
        else snprintf(dir, sizeof dir, "%s/%s", root, EMUS[i].roms);
        index_load(&ix, ixp);
        int first = l.n;
        scan_dir(&l, &ix, &EMUS[i], dir, 0);
        assign_keys(&l.g[first], l.n - first, dir);
        /* catalogo: giochi dell'indice senza ROM, solo "parent" (i cloni affollerebbero l'elenco) */
        /* le console hanno un indice solo dei giochi che ci sono (lo scrive lo scraper) */
        for (int k = 0; include_missing && !EMUS[i].console && k < ix.n; k++) {
            entry_t *en = &ix.e[k];
            if (en->present || en->bios || en->parent[0]) continue;
            game_t *g = push(&l);
            if (!g) break;
            snprintf(g->name, sizeof g->name, "%s", en->name);
            snprintf(g->core, sizeof g->core, "%s", EMUS[i].id);
            g->path = strdup("");
            if (!g->path) { l.n--; break; }
            g->missing = true;
            fill_from_entry(g, en);
        }
        /* i titoli sono copiati nei game_t: l'indice si può liberare */
        free(ix.e);
        free(ix.buf);
    }
    scan_linux(&l, root);
    load_genres(root, l.g, l.n);
    apply_genre_edits(root, l.g, l.n);
    qsort(l.g, (size_t)l.n, sizeof(game_t), title_cmp);
    for (int i = 0; i < l.n; i++) if (!l.g[i].key[0]) snprintf(l.g[i].key, sizeof l.g[i].key, "%s", l.g[i].name);
    *out = l.g;
    return l.n;
}

void games_free(game_t *g, int n) {
    for (int i = 0; g && i < n; i++) free(g[i].path);
    free(g);
}

/* ------------------------------------------------ preferiti e recenti */

static int read_list(const char *path, char keys[][LIST_KEY], int max) {
    FILE *f = fopen(path, "r");
    int n = 0;
    if (!f) return 0;
    char line[256];
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (*line) snprintf(keys[n++], LIST_KEY, "%s", line);
    }
    fclose(f);
    return n;
}

static void write_list(const char *path, char keys[][LIST_KEY], int n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { ui_log("scrittura di %s non riuscita", path); return; }
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", keys[i]);
    commit_file(f, tmp, path);
}

static void key_of(const game_t *g, char *k) { snprintf(k, LIST_KEY, "%s\t%s", g->core, g->key[0] ? g->key : g->name); }

/* Il runner scrive le statistiche con il nome del core senza "_libretro" (lo stesso della
   cartella Saves/): "genesis_plus_gx" per l'emulatore "md". Per FBNeo e MAME coincide con l'id. */
static bool stats_core_is(const game_t *g, const char *c) {
    if (!strcmp(g->core, c)) return true;
    const emulator_t *e = emulator_find(g->core);
    if (!e) return false;
    size_t len = strlen(c);
    return !strncmp(e->core, c, len) && (!e->core[len] || !strcmp(e->core + len, "_libretro"));
}

static void load_stats(const char *root, game_t *g, int n) {
    char p[1100], line[512];
    snprintf(p, sizeof p, "%s/System/stats.tsv", root);
    for (int i = 0; i < n; i++) { g[i].plays = 0; g[i].seconds = 0; g[i].last_played = 0; }
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char c[64], name[200];
        long plays, secs, last;
        if (sscanf(line, "%63[^\t]\t%199[^\t]\t%ld\t%ld\t%ld", c, name, &plays, &secs, &last) != 5) continue;
        for (int i = 0; i < n; i++)
            if (!strcmp(g[i].key[0] ? g[i].key : g[i].name, name) && stats_core_is(&g[i], c)) {
                g[i].plays = (int)plays;
                g[i].seconds = secs;
                g[i].last_played = last;
            }
    }
    fclose(f);
}

/* --------------------------------------------- collezioni e nascosti */

char COLLS[MAX_COLLS][48];
int NCOLLS;

static int key_cmp(const void *a, const void *b) { return strcmp(a, b); }

static void load_collections(const char *root) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/System/collections", root);
    NCOLLS = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && NCOLLS < MAX_COLLS) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || l - 4 >= 48 || strcmp(e->d_name + l - 4, ".txt")) continue;
        snprintf(COLLS[NCOLLS++], 48, "%.*s", (int)(l - 4), e->d_name);
    }
    closedir(d);
    qsort(COLLS, (size_t)NCOLLS, 48, (int (*)(const void *, const void *))strcasecmp);
}

/* elenco su file ordinato, per cercare ogni gioco con una ricerca binaria */
enum { LIST_MAX = 8192 };
static char (*sorted_list(const char *path, int *n))[LIST_KEY] {
    char (*keys)[LIST_KEY] = malloc(sizeof *keys * LIST_MAX);
    *n = keys ? read_list(path, keys, LIST_MAX) : 0;
    if (*n) qsort(keys, (size_t)*n, LIST_KEY, key_cmp);
    return keys;
}

static void mark_from(const char *path, game_t *g, int n, void (*set)(game_t *, void *), void *arg) {
    int nk;
    char (*keys)[LIST_KEY] = sorted_list(path, &nk), k[LIST_KEY];
    for (int i = 0; i < n && nk; i++) {
        key_of(&g[i], k);
        if (bsearch(k, keys, (size_t)nk, LIST_KEY, key_cmp)) set(&g[i], arg);
    }
    free(keys);
}

static void set_hidden(game_t *g, void *arg) { (void)arg; g->user_hidden = true; }
static void set_coll(game_t *g, void *arg) { g->colls |= 1u << *(int *)arg; }

static void list_set(const char *path, const game_t *g, bool add) {
    char (*keys)[LIST_KEY] = malloc(sizeof *keys * LIST_MAX), k[LIST_KEY];
    if (!keys) return;
    int n = read_list(path, keys, LIST_MAX - 1), w = 0;
    key_of(g, k);
    for (int i = 0; i < n; i++)
        if (strcmp(keys[i], k)) memcpy(keys[w++], keys[i], LIST_KEY);
    if (add) memcpy(keys[w++], k, LIST_KEY);
    write_list(path, keys, w);
    free(keys);
}

void games_set_hidden(const char *root, game_t *g, bool hidden) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/hidden.txt", root);
    list_set(p, g, hidden);
    g->user_hidden = hidden;
}

void games_set_coll(const char *root, game_t *g, int ci, bool in) {
    if (ci < 0 || ci >= NCOLLS) return;
    char p[1100];
    snprintf(p, sizeof p, "%s/System/collections/%s.txt", root, COLLS[ci]);
    list_set(p, g, in);
    if (in) g->colls |= 1u << ci;
    else g->colls &= ~(1u << ci);
}

/* nome utilizzabile come file: niente spazi doppi, barre o nomi nascosti */
static bool coll_clean(const char *name, char clean[48]) {
    int w = 0;
    for (const char *s = name; *s && w < 47; s++) {
        if (*s == ' ' && (!w || clean[w - 1] == ' ')) continue;
        clean[w++] = *s == '/' || *s == '\\' ? '-' : *s;
    }
    while (w && clean[w - 1] == ' ') w--;
    clean[w] = 0;
    return w && clean[0] != '.';
}

bool games_rename_coll(const char *root, int ci, const char *name, game_t *g, int n) {
    char clean[48], a[1100], b[1100];
    if (ci < 0 || ci >= NCOLLS || !coll_clean(name, clean)) return false;
    for (int i = 0; i < NCOLLS; i++)
        if (i != ci && !strcasecmp(COLLS[i], clean)) return false;
    snprintf(a, sizeof a, "%s/System/collections/%s.txt", root, COLLS[ci]);
    snprintf(b, sizeof b, "%s/System/collections/%s.txt", root, clean);
    if (rename(a, b) != 0) return false;
    games_mark_lists(root, g, n);
    return true;
}

bool games_delete_coll(const char *root, int ci, game_t *g, int n) {
    char p[1100];
    if (ci < 0 || ci >= NCOLLS) return false;
    snprintf(p, sizeof p, "%s/System/collections/%s.txt", root, COLLS[ci]);
    if (remove(p) != 0) return false;
    games_mark_lists(root, g, n);
    return true;
}

int games_new_coll(const char *root, const char *name, game_t *g, int n) {
    char clean[48];
    if (!coll_clean(name, clean)) return -1;
    for (int i = 0; i < NCOLLS; i++)
        if (!strcasecmp(COLLS[i], clean)) return i;
    if (NCOLLS >= MAX_COLLS) return -1;
    char p[1100];
    snprintf(p, sizeof p, "%s/System/collections", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/System/collections/%s.txt", root, clean);
    FILE *f = fopen(p, "a");
    if (!f) return -1;
    fclose(f);
    /* gli indici cambiano (ordine alfabetico): si ricaricano anche le appartenenze */
    games_mark_lists(root, g, n);
    for (int i = 0; i < NCOLLS; i++)
        if (!strcmp(COLLS[i], clean)) return i;
    return -1;
}

void games_mark_lists(const char *root, game_t *g, int n) {
    load_stats(root, g, n);
    load_collections(root);
    for (int i = 0; i < n; i++) { g[i].user_hidden = false; g[i].colls = 0; }
    char lp[1100];
    snprintf(lp, sizeof lp, "%s/System/hidden.txt", root);
    mark_from(lp, g, n, set_hidden, NULL);
    for (int c = 0; c < NCOLLS; c++) {
        snprintf(lp, sizeof lp, "%s/System/collections/%s.txt", root, COLLS[c]);
        mark_from(lp, g, n, set_coll, &c);
    }
    char favp[1100], recp[1100], favs[512][LIST_KEY], recs[32][LIST_KEY], k[LIST_KEY];
    snprintf(favp, sizeof favp, "%s/System/favorites.txt", root);
    snprintf(recp, sizeof recp, "%s/System/recent.txt", root);
    int nf = read_list(favp, favs, 512), nr = read_list(recp, recs, 32);
    for (int i = 0; i < n; i++) {
        key_of(&g[i], k);
        g[i].fav = false;
        g[i].recent = 0;
        for (int j = 0; j < nf; j++)
            if (!strcmp(favs[j], k)) g[i].fav = true;
        for (int j = 0; j < nr; j++)
            if (!strcmp(recs[j], k)) { g[i].recent = j + 1; break; }
    }
}

void games_toggle_fav(const char *root, game_t *g) {
    char path[1100], favs[512][LIST_KEY], k[LIST_KEY];
    snprintf(path, sizeof path, "%s/System/favorites.txt", root);
    int n = read_list(path, favs, 511), w = 0;
    key_of(g, k);
    for (int i = 0; i < n; i++)
        if (strcmp(favs[i], k)) memcpy(favs[w++], favs[i], LIST_KEY);
    g->fav = !g->fav;
    if (g->fav) memcpy(favs[w++], k, LIST_KEY);
    write_list(path, favs, w);
}

void games_add_recent(const char *root, game_t *g) {
    char path[1100], recs[33][LIST_KEY], k[LIST_KEY];
    snprintf(path, sizeof path, "%s/System/recent.txt", root);
    int n = read_list(path, recs + 1, 32), w = 1;
    key_of(g, k);
    memcpy(recs[0], k, LIST_KEY);
    for (int i = 1; i <= n && w < 20; i++)
        if (strcmp(recs[i], k)) memcpy(recs[w++], recs[i], LIST_KEY);
    write_list(path, recs, w);
}
