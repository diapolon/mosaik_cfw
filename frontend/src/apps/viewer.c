/* Immagini e fumetti: app di Mosaik, pacchetto dello Store (Arcade/Apps/viewer).

   Sfoglia la cartella Arcade (solo lettura) e mostra le immagini (PNG, JPG, BMP, GIF) e i fumetti
   .cbz (zip di immagini), anche i manuali dei giochi salvati come immagini. Aprendo un'immagine si
   scorrono tutte quelle della sua cartella; aprendo un .cbz le sue pagine, in ordine naturale
   (pagina 2 prima di pagina 10). Zoom con A: pagina intera, larghezza dello schermo, doppio;
   croce o levetta spostano l'immagine ingrandita, X ruota di 90°, L/R (e sinistra/destra a
   pagina intera) cambiano pagina, B torna all'elenco. L'ultima cartella aperta si ricorda in
   last.txt accanto al programma.

   Sul Mac: build/viewer --root sdcard --app package/apps/viewer [--auto "..."] */
#include "appkit.h"
#include "../launcher/zipread.h"
#include <SDL_image.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ------------------------------------------------------------ nomi */

static bool ends(const char *s, const char *ext) {
    size_t a = strlen(s), b = strlen(ext);
    return a > b && !strcasecmp(s + a - b, ext);
}
static bool is_image(const char *n) {
    return ends(n, ".png") || ends(n, ".jpg") || ends(n, ".jpeg") || ends(n, ".bmp") || ends(n, ".gif");
}
static bool is_comic(const char *n) { return ends(n, ".cbz"); }

/* ordine naturale: i numeri si confrontano per valore ("2" prima di "10") */
static int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)(ea - a) - (int)(eb - b);
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c) return c;
            a = ea;
            b = eb;
            continue;
        }
        int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (c) return c;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------ elenco */

typedef struct { char name[256]; bool dir; long long size; } ent_t;
static ent_t *ents;
static int nents, cap_ents, sel;
static float scroll;
static char cwd[1024];

static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return natcmp(x->name, y->name);
}

static void full(char *out, size_t n, const char *rel) {
    if (rel[0]) snprintf(out, n, "%s/%s", app_root, rel);
    else snprintf(out, n, "%s", app_root);
}

static void reload(const char *keep) {
    char p[1300];
    full(p, sizeof p, cwd);
    nents = 0;
    bool cut = false;   /* nome troppo lungo o memoria finita: l'elenco non è intero e si dice */
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!strcasecmp(cwd, "System")) continue;   /* niente da vedere, e lì c'è wifi.conf */
        char q[1600];
        struct stat sb;
        if (snprintf(q, sizeof q, "%s/%s", p, e->d_name) >= (int)sizeof q || strlen(e->d_name) >= sizeof ents[0].name) { cut = true; continue; }
        if (stat(q, &sb) != 0) continue;
        bool dir = S_ISDIR(sb.st_mode);
        if (!dir && !is_image(e->d_name) && !is_comic(e->d_name)) continue;
        if (dir && !cwd[0] && !strcasecmp(e->d_name, "System")) continue;
        if (nents == cap_ents) {
            int nc = cap_ents ? cap_ents * 2 : 256;
            ent_t *ne = realloc(ents, (size_t)nc * sizeof *ne);
            if (!ne) { cut = true; break; }
            ents = ne;
            cap_ents = nc;
        }
        ent_t *x = &ents[nents++];
        snprintf(x->name, sizeof x->name, "%s", e->d_name);
        x->dir = dir;
        x->size = dir ? 0 : (long long)sb.st_size;
    }
    if (d) closedir(d);
    if (cut) { ui_log("immagini: elenco di %s incompleto", cwd); ui_toast(FT("viewer.list_incomplete")); }
    if (nents > 1) qsort(ents, (size_t)nents, sizeof *ents, ent_cmp);
    if (sel >= nents) sel = nents ? nents - 1 : 0;
    for (int i = 0; keep && i < nents; i++)
        if (!strcmp(ents[i].name, keep)) { sel = i; break; }
}

/* l'ultima cartella aperta, per ripartire da lì */
static void last_save(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/last.txt", app_dir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", cwd); fclose(f); }
}

static void last_load(void) {
    char p[1100], line[1024] = "";
    snprintf(p, sizeof p, "%s/last.txt", app_dir);
    FILE *f = fopen(p, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\r\n")] = 0;
    char q[1300];
    struct stat sb;
    full(q, sizeof q, line);
    if (line[0] && !strstr(line, "..") && stat(q, &sb) == 0 && S_ISDIR(sb.st_mode)) snprintf(cwd, sizeof cwd, "%s", line);
}

/* ------------------------------------------------------------ pagine */

static struct {
    bool open, comic;
    char dir[1300];           /* cartella delle immagini (percorso intero) */
    char (*names)[256];       /* immagini della cartella */
    FILE *zf;                 /* fumetto: lo zip aperto e le sue voci di immagine */
    zent_t *zents;
    int *zpages, nz;
    int n, cur;
} pg;
static SDL_Texture *tex;
static int tw, th;                  /* misure dell'immagine */
static int zoom, rot;               /* 0 pagina intera, 1 larghezza, 2 doppio; rotazione in quarti */
static float panx, pany;            /* centro della vista, in frazioni dell'immagine */
static double info_until;
static bool load_failed, load_big;

static void page_free(void) {
    if (tex) SDL_DestroyTexture(tex);
    tex = NULL;
}

static void pages_close(void) {
    page_free();
    tw = th = 0;
    free(pg.names);
    pg.names = NULL;
    if (pg.zf) fclose(pg.zf);
    pg.zf = NULL;
    free(pg.zents);
    pg.zents = NULL;
    free(pg.zpages);
    pg.zpages = NULL;
    pg.open = false;
}

/* immagine troppo grande per la GPU: si rimpicciolisce prima di farne una texture */
static SDL_Texture *make_texture(SDL_Surface *s) {
    SDL_RendererInfo ri;
    int maxw = 4096, maxh = 4096;
    if (SDL_GetRendererInfo(UI.ren, &ri) == 0 && ri.max_texture_width > 0) { maxw = ri.max_texture_width; maxh = ri.max_texture_height; }
    if (s->w > maxw || s->h > maxh) {
        double k = s->w * 1.0 / maxw > s->h * 1.0 / maxh ? maxw * 1.0 / s->w : maxh * 1.0 / s->h;
        SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_Surface *d = c ? SDL_CreateRGBSurfaceWithFormat(0, (int)(s->w * k), (int)(s->h * k), 32, SDL_PIXELFORMAT_ARGB8888) : NULL;
        if (d && SDL_BlitScaled(c, NULL, d, NULL) == 0) { SDL_FreeSurface(c); c = d; d = NULL; }
        if (d) SDL_FreeSurface(d);
        if (c) { SDL_FreeSurface(s); s = c; }
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(UI.ren, s);
    if (t) { tw = s->w; th = s->h; SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear); }
    SDL_FreeSurface(s);
    return t;
}

/* misure dall'intestazione (PNG, GIF, BMP, JPEG), prima di decodificare: un'immagine enorme in
   pixel, anche se piccola su disco, finirebbe la memoria (audit APP-003). Falso se ignote. */
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static bool img_dims(const unsigned char *b, size_t n, long *w, long *h) {
    if (n >= 24 && !memcmp(b, "\x89PNG", 4)) { *w = be32(b + 16); *h = be32(b + 20); return true; }
    if (n >= 10 && !memcmp(b, "GIF8", 4)) { *w = b[6] | b[7] << 8; *h = b[8] | b[9] << 8; return true; }
    if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
        uint32_t hs = (uint32_t)(b[14] | b[15] << 8 | b[16] << 16 | (uint32_t)b[17] << 24);
        if (hs == 12) { *w = b[18] | b[19] << 8; *h = b[20] | b[21] << 8; return true; }   /* intestazione vecchia (OS/2) */
        if (hs < 40) return false;
        int32_t x = (int32_t)(b[18] | b[19] << 8 | b[20] << 16 | (uint32_t)b[21] << 24), y = (int32_t)(b[22] | b[23] << 8 | b[24] << 16 | (uint32_t)b[25] << 24);
        *w = labs((long)x); *h = labs((long)y); return true;
    }
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {   /* JPEG: si cerca il segmento SOF */
        size_t i = 2;
        while (i + 9 < n) {
            if (b[i] != 0xFF) { i++; continue; }
            unsigned m = b[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
            size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
            if (len < 2) return false;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { *h = b[i + 5] << 8 | b[i + 6]; *w = b[i + 7] << 8 | b[i + 8]; return true; }
            i += 2 + len;
        }
    }
    return false;
}
/* JPEG su file: i segmenti uno dopo l'altro fino a quello con le misure, anche oltre i primi 64 KB
   (metadati grandi: verifica audit V26-02) */
static bool jpeg_dims_file(FILE *f, long *w, long *h) {
    unsigned char m[9];
    if (fseek(f, 2, SEEK_SET) != 0) return false;
    for (int guard = 0; guard < 4096; guard++) {
        int c = fgetc(f);
        if (c == EOF) return false;
        if (c != 0xFF) continue;
        int mk;
        do mk = fgetc(f); while (mk == 0xFF);
        if (mk == EOF) return false;
        if (mk == 0xD8 || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) continue;
        if (fread(m, 1, 2, f) != 2) return false;
        long len = m[0] << 8 | m[1];
        if (len < 2) return false;
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            if (fread(m, 1, 5, f) != 5) return false;
            *h = m[1] << 8 | m[2];
            *w = m[3] << 8 | m[4];
            return true;
        }
        if (fseek(f, len - 2, SEEK_CUR) != 0) return false;
    }
    return false;
}

#define MAX_PIXELS 40000000L   /* 40 milioni di pixel: circa 160 MB decodificati */
/* vero se l'immagine si può decodificare: misure note ed entro i limiti; con misure ignote no */
static bool size_ok(bool known, long w, long h) {
    load_big = known && (w <= 0 || h <= 0 || w > 30000 || h > 30000 || w * h > MAX_PIXELS);
    return known && !load_big;
}

static void page_load(void) {
    page_free();
    load_failed = load_big = false;
    tw = th = 0;
    SDL_Surface *s = NULL;
    if (pg.comic) {
        zent_t *e = &pg.zents[pg.zpages[pg.cur]];
        size_t n = (size_t)e->usize + 1;
        char *buf = e->usize < (64u << 20) ? malloc(n) : NULL;   /* una pagina oltre 64 MB non è un fumetto */
        long w = 0, h = 0;
        /* misure prima, controllo dopo: in una sola chiamata l'ordine degli argomenti non è
           garantito dal C e w, h potrebbero arrivare ancora a zero (verifica audit 0.9.27) */
        bool got = buf && zip_entry(pg.zf, e, NULL, buf, n);
        bool known = got && img_dims((unsigned char *)buf, e->usize, &w, &h);
        if (got && size_ok(known, w, h)) s = IMG_Load_RW(SDL_RWFromConstMem(buf, (int)e->usize), 1);
        free(buf);
    } else {
        char p[1600];
        unsigned char head[65536];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", pg.dir, pg.names[pg.cur]);
        FILE *f = fopen(p, "rb");
        size_t hn = f ? fread(head, 1, sizeof head, f) : 0;
        long w = 0, h = 0;
        bool known = hn >= 4 && head[0] == 0xFF && head[1] == 0xD8 ? jpeg_dims_file(f, &w, &h) : img_dims(head, hn, &w, &h);
        if (f) fclose(f);
        if (stat(p, &sb) == 0 && sb.st_size < (64 << 20) && size_ok(known, w, h)) s = IMG_Load(p);
    }
    if (s) tex = make_texture(s);
    if (!tex || tw <= 0 || th <= 0) { page_free(); tw = th = 0; load_failed = true; ui_log("immagini: pagina %d non leggibile o troppo grande (%s)", pg.cur + 1, IMG_GetError()); }
    panx = 0.5f;
    pany = 0.0f;   /* dall'alto: le pagine si leggono così */
    info_until = UI.t + 2.0;
}

static const zent_t *sort_z;
static int page_cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b, c = natcmp(sort_z[x].name, sort_z[y].name);
    return c ? c : x - y;
}

static bool comic_open(const char *path) {
    pg.zf = fopen(path, "rb");
    if (!pg.zf) return false;
    int n = zip_dir(pg.zf, &pg.zents);
    if (n <= 0) return false;
    pg.zpages = malloc(sizeof(int) * (size_t)n);
    if (!pg.zpages) return false;
    pg.nz = 0;
    for (int i = 0; i < n; i++) {
        const char *b = strrchr(pg.zents[i].name, '/');
        b = b ? b + 1 : pg.zents[i].name;
        if (b[0] != '.' && is_image(pg.zents[i].name)) pg.zpages[pg.nz++] = i;
    }
    /* ordine naturale del percorso nello zip, a parità l'ordine nello zip (audit APP-004: prima
       era quadratico) */
    sort_z = pg.zents;
    qsort(pg.zpages, (size_t)pg.nz, sizeof *pg.zpages, page_cmp);
    pg.n = pg.nz;
    return pg.n > 0;
}

static void open_entry(int i) {
    char base[1300], p[1600];
    full(base, sizeof base, cwd);
    snprintf(p, sizeof p, "%s/%s", base, ents[i].name);
    memset(&pg, 0, sizeof pg);
    zoom = rot = 0;
    if (is_comic(ents[i].name)) {
        pg.comic = true;
        if (!comic_open(p)) { pages_close(); ui_toast(FT("viewer.bad_comic")); ui_sound(SND_ERROR); return; }
    } else {
        snprintf(pg.dir, sizeof pg.dir, "%s", base);
        int n = 0;
        for (int k = 0; k < nents; k++) n += !ents[k].dir && is_image(ents[k].name);
        pg.names = malloc((size_t)(n ? n : 1) * sizeof *pg.names);
        if (!pg.names) return;
        for (int k = 0; k < nents; k++) {
            if (ents[k].dir || !is_image(ents[k].name)) continue;
            if (k == i) pg.cur = pg.n;
            snprintf(pg.names[pg.n++], 256, "%s", ents[k].name);
        }
    }
    pg.open = true;
    ui_log("immagini: apro %s (%d pagine)", ents[i].name, pg.n);
    page_load();
}

/* rettangolo dell'immagine sullo schermo con zoom, rotazione e spostamento */
static void view_rect(SDL_Rect *r, int *vw, int *vh) {
    bool side = rot % 2;
    double iw = side ? th : tw, ih = side ? tw : th;   /* misure dopo la rotazione */
    double k;
    if (zoom == 0) k = UI.w / iw < UI.h / ih ? UI.w / iw : UI.h / ih;
    else if (zoom == 1) k = UI.w / iw;
    else k = 2.0 * (UI.w / iw < UI.h / ih ? UI.w / iw : UI.h / ih);
    int w = (int)(iw * k), h = (int)(ih * k);
    *vw = w;
    *vh = h;
    int x = w <= UI.w ? (UI.w - w) / 2 : (int)(-(w - UI.w) * panx);
    int y = h <= UI.h ? (UI.h - h) / 2 : (int)(-(h - UI.h) * pany);
    /* SDL ruota attorno al centro del rettangolo con le misure non ruotate */
    int rw = side ? h : w, rh = side ? w : h;
    r->x = x + (w - rw) / 2;
    r->y = y + (h - rh) / 2;
    r->w = rw;
    r->h = rh;
}

static void page_go(int d) {
    int n = pg.cur + d;
    if (n < 0 || n >= pg.n) { ui_sound(SND_ERROR); info_until = UI.t + 1.5; return; }
    pg.cur = n;
    ui_sound(SND_MOVE);
    page_load();
}

static void view_input(void) {
    input_t *in = &UI.in;
    SDL_Rect r;
    int vw = 0, vh = 0;
    if (tex) view_rect(&r, &vw, &vh);   /* senza immagine valida niente misure (audit APP-002) */
    bool pan_x = vw > UI.w, pan_y = vh > UI.h;
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) { pages_close(); ui_sound(SND_BACK); return; }
    if (in->pressed[BTN_L]) page_go(-1);
    if (in->pressed[BTN_R]) page_go(+1);
    if (!pan_x && in->pressed[BTN_LEFT]) page_go(-1);
    if (!pan_x && in->pressed[BTN_RIGHT]) page_go(+1);
    if (!tex) {   /* pagina illeggibile: solo cambio pagina (sinistra/destra già sopra) e uscita */
        if (in->pressed[BTN_UP]) page_go(-1);
        if (in->pressed[BTN_DOWN]) page_go(+1);
        return;
    }
    if (in->pressed[BTN_A]) { zoom = (zoom + 1) % 3; panx = 0.5f; pany = 0; ui_sound(SND_OK); info_until = UI.t + 1.5; }
    if (in->pressed[BTN_X]) { rot = (rot + 1) % 4; ui_sound(SND_OK); }
    /* spostamento: croce tenuta o levetta sinistra, più veloce sulle immagini piccole */
    float sx = 0, sy = 0;
    if (in->down[BTN_LEFT]) sx -= 1;
    if (in->down[BTN_RIGHT]) sx += 1;
    if (in->down[BTN_UP]) sy -= 1;
    if (in->down[BTN_DOWN]) sy += 1;
    if (abs(in->axis[0]) > 8000) sx += in->axis[0] / 32768.0f;
    if (abs(in->axis[1]) > 8000) sy += in->axis[1] / 32768.0f;
    float speed = (float)UI.dt * 900.0f;
    if (pan_x && sx) panx += sx * speed / (float)(vw - UI.w);
    if (pan_y && sy) pany += sy * speed / (float)(vh - UI.h);
    /* in fondo alla pagina, giù porta alla seguente (in alto, su alla precedente) */
    if (!pan_y && in->pressed[BTN_DOWN] && zoom == 0) page_go(+1);
    if (!pan_y && in->pressed[BTN_UP] && zoom == 0) page_go(-1);
    panx = panx < 0 ? 0 : panx > 1 ? 1 : panx;
    pany = pany < 0 ? 0 : pany > 1 ? 1 : pany;
}

static void view_draw(void) {
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 255));
    TTF_Font *fs = UI.f_small;
    if (tex) {
        SDL_Rect r;
        int vw, vh;
        view_rect(&r, &vw, &vh);
        SDL_RenderCopyEx(UI.ren, tex, NULL, &r, rot * 90.0, NULL, SDL_FLIP_NONE);
    } else if (load_failed) ui_text(UI.f_normal, FT(load_big ? "viewer.too_big" : "viewer.load_failed"), UI.w / 2, UI.h / 2, rgba(220, 220, 220, 255), ALIGN_CENTER);
    if (UI.t < info_until) {   /* pagina e nome, per qualche istante dopo ogni cambio */
        char line[400], name[300];
        const char *nm = pg.comic ? pg.zents[pg.zpages[pg.cur]].name : pg.names[pg.cur];
        const char *b = strrchr(nm, '/');
        ui_text_fit(fs, b ? b + 1 : nm, UI.w / 2, name, sizeof name);
        snprintf(line, sizeof line, "%d / %d  ·  %s", pg.cur + 1, pg.n, name);
        int w = ui_text_w(fs, line) + M(0.04f), h = TTF_FontHeight(fs) + M(0.02f);
        ui_rrect((UI.w - w) / 2, UI.h - h - M(0.03f), w, h, h / 2, rgba(0, 0, 0, 170));
        ui_text(fs, line, UI.w / 2, UI.h - h - M(0.03f) + M(0.01f), rgba(240, 240, 240, 255), ALIGN_CENTER);
    }
}

/* ------------------------------------------------------------ elenco */

static int rows_visible(int *top_y, int *row_h) {
    theme_t *t = &UI.th;
    int y = M(t->topbar_h) + M(0.02f) + TTF_FontHeight(UI.f_normal) + M(0.025f), bottom = UI.h - ui_keybar_h() - M(0.01f);
    int rh = TTF_FontHeight(UI.f_normal) + M(0.028f);
    *top_y = y;
    *row_h = rh;
    return (bottom - y) / rh;
}

/* cartella, immagine (cornice con un monte) o fumetto (libretto) */
static void icon(int x, int y, int s, const ent_t *e, color_t c, color_t bg) {
    if (e->dir) {
        ui_rrect(x, y + s / 6, s * 5 / 12, s / 4, s / 10, c);
        ui_rrect(x, y + s / 4, s, s * 2 / 3, s / 8, c);
    } else if (is_comic(e->name)) {
        ui_rrect(x + s / 8, y + s / 12, s * 3 / 4, s * 5 / 6, s / 10, c);
        ui_rect(x + s / 4, y + s / 12, s / 14 + 1, s * 5 / 6, bg);
    } else {
        ui_rrect(x, y + s / 6, s, s * 2 / 3, s / 10, c);
        ui_triangle(x + s / 8, y + s * 3 / 4, x + s * 3 / 8, y + s * 7 / 16, x + s * 5 / 8, y + s * 3 / 4, bg);
        ui_circle(x + s * 3 / 4, y + s * 3 / 8, s / 10, bg);
    }
}

static void list_draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    ui_topbar(FT("viewer.title"));
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int py = M(t->topbar_h) + M(0.02f);
    char path[1600] = "Arcade", fit[1600];
    for (const char *c = cwd, *s; *c; c = *s ? s + 1 : s) {
        s = c + strcspn(c, "/");
        size_t l = strlen(path);
        snprintf(path + l, sizeof path - l, " / %.*s", (int)(s - c), c);
    }
    ui_text_fit(fn, path, W, fit, sizeof fit);
    ui_text(fn, fit, mx, py, t->accent, ALIGN_LEFT);
    int top_y, rh, vis = rows_visible(&top_y, &rh);
    if (!nents) {
        int ny = top_y + rh;
        char lines[3][160];
        int n = ui_wrap(fn, FT("viewer.empty"), W, lines, 3);
        for (int k = 0; k < n; k++, ny += fnh) ui_text(fn, lines[k], UI.w / 2, ny, t->text_dim, ALIGN_CENTER);
    } else {
        int first = (int)scroll;
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        scroll = (float)first;
        for (int i = first; i < nents && i < first + vis; i++) {
            ent_t *e = &ents[i];
            int ry = top_y + (i - first) * rh;
            bool on = i == sel;
            if (on) ui_rrect(mx - M(0.012f), ry, W + M(0.024f), rh, M(t->panel_radius), with_alpha(t->accent, 230));
            color_t tc = on ? t->accent_text : t->text, dc = on ? t->accent_text : t->text_dim;
            int is = fnh;
            icon(mx, ry + (rh - is) / 2, is, e, on ? t->accent_text : e->dir ? t->accent : t->text_dim, on ? t->accent : t->bg);
            char right[48] = "";
            if (!e->dir) app_size(e->size, right, sizeof right);
            int rw = ui_text_w(fs, right);
            ui_text(fs, right, mx + W, ry + (rh - fsh) / 2, dc, ALIGN_RIGHT);
            ui_text_fit(fn, e->name, W - is - M(0.03f) - rw - M(0.03f), fit, sizeof fit);
            ui_text(fn, fit, mx + is + M(0.02f), ry + (rh - fnh) / 2, tc, ALIGN_LEFT);
        }
        if (nents > vis) {
            int bh = vis * rh, sh = bh * vis / nents, sy = top_y + (bh - sh) * first / (nents - vis);
            ui_rrect(UI.w - mx / 2 - M(0.004f), sy, M(0.006f), sh, M(0.003f), with_alpha(t->text_dim, 120));
        }
    }
    const char *h[] = { "B", cwd[0] ? T("common.back") : FT("viewer.quit"), NULL, "A", FT("viewer.open"), NULL };
    ui_keybar(h, nents ? 6 : 3);
}

static bool quit;

static void list_input(void) {
    input_t *in = &UI.in;
    int top_y, rh, vis = rows_visible(&top_y, &rh), old = sel;
    if (in->pressed[BTN_UP] && nents) sel = (sel + nents - 1) % nents;
    if (in->pressed[BTN_DOWN] && nents) sel = (sel + 1) % nents;
    if (in->pressed[BTN_L] || in->pressed[BTN_LEFT]) sel = sel - vis < 0 ? 0 : sel - vis;
    if (in->pressed[BTN_R] || in->pressed[BTN_RIGHT]) sel = sel + vis >= nents ? (nents ? nents - 1 : 0) : sel + vis;
    if (sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_MENU]) { quit = true; return; }
    if (in->pressed[BTN_B]) {
        ui_sound(SND_BACK);
        if (!cwd[0]) { quit = true; return; }
        char last[256];
        char *s = strrchr(cwd, '/');
        if (s) { snprintf(last, sizeof last, "%s", s + 1); *s = 0; }
        else { snprintf(last, sizeof last, "%s", cwd); cwd[0] = 0; }
        scroll = 0;
        reload(last);
        last_save();
        return;
    }
    if (in->pressed[BTN_A] && nents) {
        ui_sound(SND_OK);
        if (ents[sel].dir) {
            char n[1024];
            if (cwd[0]) snprintf(n, sizeof n, "%s/%s", cwd, ents[sel].name);
            else snprintf(n, sizeof n, "%s", ents[sel].name);
            snprintf(cwd, sizeof cwd, "%s", n);
            sel = 0;
            scroll = 0;
            reload(NULL);
            last_save();
        } else open_entry(sel);
    }
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "viewer")) return 1;
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    last_load();
    reload(NULL);
    while (!quit) {
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        if (pg.open) view_input(); else list_input();
        ui_begin();
        if (pg.open) view_draw(); else list_draw();
        ui_end();
    }
    app_exit_begin();
    pages_close();
    app_quit();
    return 0;
}
