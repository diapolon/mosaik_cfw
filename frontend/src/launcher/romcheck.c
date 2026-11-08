#include "romcheck.h"
#include <SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* --------------------------------------------------------- lettura zip */
/* Solo la "central directory": per ogni file nome, dimensione e CRC32. */

typedef struct { char name[96]; unsigned crc, size; } zentry_t;
typedef struct { zentry_t *e; int n; } zlist_t;

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }

static bool zip_list(const char *path, zlist_t *out) {
    out->e = NULL;
    out->n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), tail = size < 65557 ? size : 65557;
    unsigned char *buf = malloc((size_t)tail);
    fseek(f, size - tail, SEEK_SET);
    bool ok = fread(buf, 1, (size_t)tail, f) == (size_t)tail;
    long eocd = -1;
    for (long i = tail - 22; ok && i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); fclose(f); return false; }
    unsigned entries = rd16(buf + eocd + 10), cd_size = rd32(buf + eocd + 12), cd_off = rd32(buf + eocd + 16);
    free(buf);
    if ((long long)cd_off + cd_size > size) { fclose(f); return false; }   /* zip rovinato */
    unsigned char *cd = malloc(cd_size);
    if (!cd) { fclose(f); return false; }
    fseek(f, (long)cd_off, SEEK_SET);
    ok = fread(cd, 1, cd_size, f) == cd_size;
    fclose(f);
    if (!ok) { free(cd); return false; }
    out->e = calloc(entries + 1, sizeof(zentry_t));
    unsigned pos = 0;
    for (unsigned i = 0; i < entries && pos + 46 <= cd_size; i++) {
        if (rd32(cd + pos) != 0x02014b50) break;
        unsigned nlen = rd16(cd + pos + 28), xlen = rd16(cd + pos + 30), clen = rd16(cd + pos + 32);
        if (pos + 46 + nlen > cd_size) break;   /* zip rovinato: il nome uscirebbe dalla tabella */
        zentry_t *z = &out->e[out->n++];
        z->crc = rd32(cd + pos + 16);
        z->size = rd32(cd + pos + 24);
        /* i file nelle sottocartelle dello zip contano con il solo nome */
        const char *nm = (const char *)cd + pos + 46;
        unsigned start = 0;
        for (unsigned k = 0; k < nlen; k++)
            if (nm[k] == '/') start = k + 1;
        snprintf(z->name, sizeof z->name, "%.*s", (int)(nlen - start), nm + start);
        pos += 46 + nlen + xlen + clen;
    }
    free(cd);
    return true;
}

/* -------------------------------------------- elenchi attesi dal DAT */

typedef struct { char *name, *romof, *files; } want_t;
typedef struct {
    char id[16], romdir[1100];
    char *buf;
    want_t *w;
    int nw;
    char **bios;   /* nomi dei BIOS (colonna "bios" dell'indice) */
    int nbios;
    /* tutti gli zip della cartella ROM: nome -> percorso, per trovare parent e BIOS */
    char (*zname)[64];
    char **zpath;
    int nz, capz;
} emu_data_t;

static int want_cmp(const void *a, const void *b) { return strcmp(((const want_t *)a)->name, ((const want_t *)b)->name); }

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    n = (long)fread(s, 1, (size_t)n, f);
    s[n] = 0;
    fclose(f);
    return s;
}

static void index_zips(emu_data_t *d, const char *dir, int depth) {
    DIR *dd = opendir(dir);
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) index_zips(d, p, depth + 1);
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len < 5 || strcasecmp(e->d_name + len - 4, ".zip")) continue;
        if (d->nz == d->capz) {
            d->capz = d->capz ? d->capz * 2 : 256;
            d->zname = realloc(d->zname, sizeof *d->zname * (size_t)d->capz);
            d->zpath = realloc(d->zpath, sizeof *d->zpath * (size_t)d->capz);
        }
        snprintf(d->zname[d->nz], 64, "%.*s", (int)(len - 4), e->d_name);
        d->zpath[d->nz++] = strdup(p);
    }
    closedir(dd);
}

static const char *zip_of(const emu_data_t *d, const char *name) {
    for (int i = 0; i < d->nz; i++)
        if (!strcmp(d->zname[i], name)) return d->zpath[i];
    return NULL;
}

static bool load_emu(emu_data_t *d, const char *root, const emulator_t *emu) {
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "%s", emu->id);
    char p[1400];
    /* l'elenco dei file sta accanto all'indice: Dats/<id>.roms */
    snprintf(p, sizeof p, "%s/%.*s.roms", root, (int)(strlen(emu->index) > 4 ? strlen(emu->index) - 4 : strlen(emu->index)), emu->index);
    d->buf = read_all(p);
    if (!d->buf) return false;
    int cap = 1;
    for (char *c = d->buf; *c; c++) cap += *c == '\n';
    d->w = calloc((size_t)cap, sizeof(want_t));
    for (char *line = d->buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = *t2 = 0;
            d->w[d->nw++] = (want_t){ line, t1 + 1, t2 + 1 };
        }
        line = nl ? nl + 1 : NULL;
    }
    qsort(d->w, (size_t)d->nw, sizeof(want_t), want_cmp);
    /* BIOS dall'indice dei titoli (settima colonna) */
    snprintf(p, sizeof p, "%s/%s", root, emu->index);
    FILE *f = fopen(p, "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        char *col[8] = { 0 };
        int c = 0;
        for (char *q = line; q && c < 8; c++) {
            col[c] = q;
            q = strchr(q, '\t');
            if (q) *q++ = 0;
        }
        if (c >= 7 && col[6][0] == '1') {
            d->bios = realloc(d->bios, sizeof(char *) * (size_t)(d->nbios + 1));
            d->bios[d->nbios++] = strdup(col[0]);
        }
    }
    if (f) fclose(f);
    snprintf(d->romdir, sizeof d->romdir, "%s/%s", root, emu->roms);
    index_zips(d, d->romdir, 0);
    snprintf(p, sizeof p, "%s/Bios", root);
    index_zips(d, p, 0);
    return true;
}

static void free_emu(emu_data_t *d) {
    free(d->buf);
    free(d->w);
    for (int i = 0; i < d->nbios; i++) free(d->bios[i]);
    free(d->bios);
    for (int i = 0; i < d->nz; i++) free(d->zpath[i]);
    free(d->zpath);
    free(d->zname);
}

static want_t *want_find(emu_data_t *d, const char *name) {
    want_t key = { (char *)name, NULL, NULL };
    return d->nw ? bsearch(&key, d->w, (size_t)d->nw, sizeof(want_t), want_cmp) : NULL;
}

static bool is_bios(const emu_data_t *d, const char *name) {
    for (int i = 0; i < d->nbios; i++)
        if (!strcmp(d->bios[i], name)) return true;
    return false;
}

static bool has_crc(const zlist_t *z, unsigned crc, unsigned size) {
    for (int i = 0; i < z->n; i++)
        if (z->e[i].crc == crc && (!size || z->e[i].size == size)) return true;
    return false;
}

static bool has_name(const zlist_t *z, const char *name) {
    for (int i = 0; i < z->n; i++)
        if (!strcasecmp(z->e[i].name, name)) return true;
    return false;
}

/* ----------------------------------------------------- verifica gioco */

/* *needs_bios: il file da recuperare (RC_NOPARENT) è un BIOS e non un gioco principale */
static int check_game(emu_data_t *d, const game_t *g, char *detail, size_t dn, bool *needs_bios) {
    detail[0] = 0;
    *needs_bios = false;
    size_t plen = strlen(g->path);
    if (plen > 3 && !strcasecmp(g->path + plen - 3, ".7z")) return RC_UNCHECKED;
    want_t *w = want_find(d, g->name);
    if (!w) return RC_NOTINDAT;
    zlist_t own, chain[3] = { { 0 } };
    if (!zip_list(g->path, &own)) { snprintf(detail, dn, "zip"); return RC_MISSING; }
    /* catena romof: parent del clone e poi BIOS */
    char missing_parent[64] = "";
    bool bios_missing = false;
    int nchain = 0;
    const char *r = w->romof;
    for (int depth = 0; r && *r && depth < 3; depth++) {
        const char *zp = zip_of(d, r);
        if (zp && zip_list(zp, &chain[nchain])) nchain++;
        else {
            if (!missing_parent[0]) snprintf(missing_parent, sizeof missing_parent, "%s", r);
            if (is_bios(d, r)) bios_missing = true;
        }
        want_t *pw = want_find(d, r);
        r = pw ? pw->romof : NULL;
    }
    int miss = 0, miss_merged = 0, bad = 0, merged_listed = 0;
    char list[256] = "";
    char *files = strdup(w->files), *save = NULL;
    for (char *item = strtok_r(files, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        char *f1 = strchr(item, '|'), *f2 = f1 ? strchr(f1 + 1, '|') : NULL, *f3 = f2 ? strchr(f2 + 1, '|') : NULL;
        if (!f1 || !f2 || !f3) continue;
        *f1 = *f2 = *f3 = 0;
        unsigned crc = (unsigned)strtoul(f2 + 1, NULL, 16);
        bool merged = f3[1] == 'm';
        if (merged) merged_listed++;
        unsigned fsize = (unsigned)strtoul(f1 + 1, NULL, 10);
        if (has_crc(&own, crc, fsize)) continue;
        bool in_chain = false;
        for (int c = 0; c < nchain && !in_chain; c++) in_chain = has_crc(&chain[c], crc, fsize);
        if (in_chain) continue;
        if (has_name(&own, item)) bad++;
        else {
            miss++;
            if (merged) miss_merged++;
        }
        if (strlen(list) + strlen(item) + 3 < sizeof list) {
            if (list[0]) strcat(list, ", ");
            strcat(list, item);
        }
    }
    free(files);
    free(own.e);
    for (int c = 0; c < nchain; c++) free(chain[c].e);
    if (miss && miss == miss_merged && missing_parent[0]) {
        snprintf(detail, dn, "%s.zip", missing_parent);
        *needs_bios = is_bios(d, missing_parent);
        return RC_NOPARENT;
    }
    if (miss) { snprintf(detail, dn, "%s", list); return RC_MISSING; }
    if (bad) { snprintf(detail, dn, "%s", list); return RC_BADCRC; }
    /* Se l'elenco comprende i file del BIOS, averli trovati basta anche senza lo zip del BIOS
       (rbisland.zip di solito li contiene). Se non li comprende, l'assenza dello zip conta. */
    if (bios_missing && !merged_listed) { snprintf(detail, dn, "%s.zip", missing_parent); *needs_bios = true; return RC_NOPARENT; }
    return RC_OK;
}

/* -------------------------------------------------------------- thread */

typedef struct { char name[64], core[16], *path; } job_t;

static romcheck_status_t st;
static SDL_Thread *thread;
static char root_dir[1024];
static job_t *jobs;
static int njobs;

static int worker(void *arg) {
    (void)arg;
    char out[1100], tmp[1110];
    snprintf(out, sizeof out, "%s/System/romcheck.tsv", root_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "w");
    for (int e = 0; e < NEMUS && !st.cancel; e++) {
        emu_data_t d;
        bool have = load_emu(&d, root_dir, &EMUS[e]);
        for (int i = 0; i < njobs && !st.cancel; i++) {
            if (strcmp(jobs[i].core, EMUS[e].id)) continue;
            game_t g = { 0 };
            snprintf(g.name, sizeof g.name, "%s", jobs[i].name);
            g.path = jobs[i].path;
            char detail[300];
            bool needs_bios;
            int rc = have ? check_game(&d, &g, detail, sizeof detail, &needs_bios) : RC_UNCHECKED;
            if (!have) detail[0] = 0;
            st.count[rc]++;
            st.done++;
            if (f) fprintf(f, "%s\t%s\t%d\t%s\n", jobs[i].core, jobs[i].name, rc, detail);
        }
        if (have) free_emu(&d);
    }
    if (f) {
        fclose(f);
        if (!st.cancel) rename(tmp, out);
        else remove(tmp);
    }
    st.finished = true;
    return 0;
}

bool romcheck_start(const char *root, const game_t *games, int n) {
    if (thread) return false;
    memset((void *)&st, 0, sizeof st);
    snprintf(root_dir, sizeof root_dir, "%s", root);
    for (int i = 0; i < njobs; i++) free(jobs[i].path);
    free(jobs);
    jobs = calloc((size_t)n + 1, sizeof(job_t));
    njobs = 0;
    for (int i = 0; i < n; i++) {
        if (games[i].missing || !strcmp(games[i].core, "linux")) continue;
        job_t *j = &jobs[njobs++];
        snprintf(j->name, sizeof j->name, "%s", games[i].name);
        snprintf(j->core, sizeof j->core, "%s", games[i].core);
        j->path = strdup(games[i].path);
    }
    st.total = njobs;
    st.running = true;
    thread = SDL_CreateThread(worker, "romcheck", NULL);
    return thread != NULL;
}

void romcheck_cancel(void) { st.cancel = true; }
const romcheck_status_t *romcheck_status(void) { return &st; }

void romcheck_join(void) {
    if (!thread) return;
    SDL_WaitThread(thread, NULL);
    thread = NULL;
    st.running = false;
}

/* ------------------------------------------------- diagnosi di un gioco */

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void romcheck_diagnose(const char *root, const game_t *g, const char *core_path, bool core_failed, rdiag_t *out) {
    memset(out, 0, sizeof *out);
    const emulator_t *emu = emulator_find(g->core);
    struct stat st;
    snprintf(out->where, sizeof out->where, "Arcade/%s", emu ? emu->roms : "Roms");
    if (stat(core_path, &st) != 0) {
        out->kind = DG_NO_CORE;
        snprintf(out->file, sizeof out->file, "%s", base_name(core_path));
        return;
    }
    if (core_failed) {
        out->kind = DG_CORE_BAD;
        snprintf(out->file, sizeof out->file, "%s", base_name(core_path));
        return;
    }
    if (!g->path || !*g->path || stat(g->path, &st) != 0) {
        out->kind = DG_NO_ROM;
        snprintf(out->file, sizeof out->file, "%s", g->path && *g->path ? base_name(g->path) : g->name);
        return;
    }
    emu_data_t d;
    if (!emu || !load_emu(&d, root, emu)) { out->kind = DG_UNKNOWN; return; }
    char detail[300];
    bool needs_bios;
    int rc = check_game(&d, g, detail, sizeof detail, &needs_bios);
    free_emu(&d);
    snprintf(out->file, sizeof out->file, "%s", detail);
    switch (rc) {
    case RC_OK: out->kind = DG_OK; break;
    case RC_NOPARENT: out->kind = needs_bios ? DG_NO_BIOS : DG_NO_PARENT; break;
    case RC_BADCRC: out->kind = DG_BAD_VERSION; snprintf(out->where, sizeof out->where, "%s", emu->name); break;
    case RC_NOTINDAT:
        out->kind = DG_NOT_IN_DAT;
        snprintf(out->file, sizeof out->file, "%s", base_name(g->path));
        snprintf(out->where, sizeof out->where, "%s", emu->name);
        break;
    case RC_MISSING:
        if (!strcmp(detail, "zip")) {   /* la tabella dello zip non si legge */
            out->kind = DG_BAD_ZIP;
            snprintf(out->file, sizeof out->file, "%s", base_name(g->path));
        } else {
            out->kind = DG_MISSING;
            snprintf(out->where, sizeof out->where, "%s", emu->name);
        }
        break;
    default: out->kind = DG_UNKNOWN; break;
    }
}

/* ------------------------------------------------- risultati salvati */

int romcheck_load(const char *root, game_t *g, int n) {
    char p[1100], line[512];
    snprintf(p, sizeof p, "%s/System/romcheck.tsv", root);
    for (int i = 0; i < n; i++) g[i].romstatus = RC_NONE;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int loaded = 0;
    while (fgets(line, sizeof line, f)) {
        char core[16], name[64];
        int rc;
        if (sscanf(line, "%15[^\t]\t%63[^\t]\t%d", core, name, &rc) != 3) continue;
        for (int i = 0; i < n; i++)
            if (!strcmp(g[i].core, core) && !strcmp(g[i].name, name)) { g[i].romstatus = rc; loaded++; break; }
    }
    fclose(f);
    return loaded;
}

bool romcheck_detail(const char *root, const game_t *g, char *out, int outn) {
    char p[1100], line[512];
    snprintf(p, sizeof p, "%s/System/romcheck.tsv", root);
    FILE *f = fopen(p, "r");
    out[0] = 0;
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char core[16], name[64], detail[300] = "";
        int rc;
        if (sscanf(line, "%15[^\t]\t%63[^\t]\t%d\t%299[^\n]", core, name, &rc, detail) < 3) continue;
        if (!strcmp(core, g->core) && !strcmp(name, g->name)) {
            snprintf(out, (size_t)outn, "%s", detail);
            found = true;
        }
    }
    fclose(f);
    return found;
}
