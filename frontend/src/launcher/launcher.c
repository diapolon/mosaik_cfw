/* Launcher: home, libreria, impostazioni. Esce con 10 per avviare un gioco
   (descritto in System/next.txt), 20 per spegnere, 0 per chiudere. */
#include "../common/ui.h"
#include "../common/json.h"
#include "../common/filters.h"
#include "games.h"
#include "scraper.h"
#include "romcheck.h"
#include "music.h"
#include "../common/platform.h"
#include "../common/wifi.h"
#include "update.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define CORE_EXT "dylib"
#else
#define CORE_EXT "so"
#endif

enum { EXIT_QUIT = 0, EXIT_LAUNCH = 10, EXIT_POWEROFF = 20, EXIT_REBOOT = 21 };
enum { SCR_HOME, SCR_LIBRARY, SCR_SETTINGS, SCR_DETAIL };
static int detail_idx = -1;   /* gioco mostrato nella scheda */
static void open_detail(int idx);
static const char *genre_hint(void);
static void panel_box(int w, int h, int *x, int *y);

static game_t *games;
static int ngames;

static int screen = SCR_HOME, back_screen = SCR_HOME;
static double screen_t0;
static double last_input;

#define CPU_ACTIVE_KHZ 1608000   /* come il profilo Bilanciato: stessa prontezza di prima */
#define CPU_IDLE_KHZ    600000
#define IDLE_AFTER_S       3.0

/* home */
static int carousel[12], ncarousel;
static int home_row, home_sel, icon_sel;
static float home_scroll, home_scroll_target, ring_x, ring_y;
static bool ring_init;

/* libreria */
/* schede: tutti, un emulatore ciascuno (TAB_EMU + indice), Linux, preferiti */
enum { TAB_ALL, TAB_LINUX, TAB_FAV, TAB_EMU = 100, TAB_COLL = 200 };
static int tabs[24], ntabs, tab_sel;
static int *view, nview, lib_sel;
static float lib_scroll, lib_scroll_target, lib_hl;

/* impostazioni */
static int set_sel;              /* voce scelta (S_*) */
static int set_page = -1;        /* -1: elenco delle categorie; altrimenti la categoria aperta */
static int set_row;              /* riga scelta nella pagina */
static float set_scroll, set_hl;
static void settings_open(int page, int row);
enum { SC_DISPLAY, SC_LOOK, SC_GAMES, SC_CONTROLS, SC_SYSTEM, SC_TOOLS, SC_UPDATES, SC_COUNT };
static char theme_ids[16][64], theme_names[16][64];
static int nthemes;

/* dialogo spegnimento */
static bool dlg_open, dlg_suspend;   /* dlg_suspend: la finestra chiede di sospendere, non di spegnere */

/* scaricamento immagini e verifica ROM in corso (finestre di avanzamento) */
static bool scrape_open, romcheck_open;
static bool only_problems;   /* filtro: solo giochi con ROM da sistemare */

/* pannello filtri e tastiera a schermo */
static bool fpanel_open, kb_open;
static int fpanel_sel, kb_row, kb_col;

/* collezioni: filtro attivo (-1 tutte, COLL_HIDDEN i giochi nascosti, altrimenti indice) */
enum { COLL_ALL = -1, COLL_HIDDEN = -2 };
static int coll_sel = COLL_ALL;
/* finestra "opzioni del gioco" nella scheda: collezioni e nascondi */
static bool gopt_open;
static int gopt_sel;
/* la tastiera serve per la ricerca, il nome di una nuova collezione e la password Wi-Fi */
enum { KBM_SEARCH, KBM_COLL, KBM_WIFI, KBM_GENRE_NEW, KBM_GENRE_RENAME };
static int kb_mode;
static int kb_layout;   /* 0 maiuscole, 1 minuscole, 2 simboli (si cambia con X) */
static char coll_name[48];
static char genre_text[30];   /* nome scritto con la tastiera per un genere */
/* finestra delle reti Wi-Fi */
static bool wpanel_open;
static wifi_net_t wnets[32];
static int nwnets, wsel;
static float wscroll;
static double wscan_t, wlist_t;
static char wifi_target[33], wifi_pass[64];
static int dlg_sel;

static bool is_list_layout(void) { return !strcmp(UI.th.layout, "list"); }

static void go(int scr) {
    if (scr == screen) return;
    back_screen = screen;
    screen = scr;
    screen_t0 = UI.t;
    ring_init = false;
}

/* ------------------------------------------------------------ dati */

/* Giochi doppi: lo stesso nome ROM nel set FBNeo e in quello MAME. Secondo
   l'impostazione si mostrano entrambi o solo quello del core preferito. */
static int name_cmp(const void *a, const void *b) {
    return strcmp(games[*(const int *)a].name, games[*(const int *)b].name);
}

static void mark_duplicates(void) {
    int *ix = malloc(sizeof(int) * (size_t)(ngames + 1));
    for (int i = 0; i < ngames; i++) {
        ix[i] = i;
        games[i].hidden = false;
    }
    qsort(ix, (size_t)ngames, sizeof(int), name_cmp);
    /* dup_mode: 0 mostra tutti, n = preferisci l'emulatore n (in ordine di configurazione) */
    const char *keep = UI.set.dup_mode > 0 && UI.set.dup_mode <= NEMUS ? EMUS[UI.set.dup_mode - 1].id : NULL;
    for (int a = 0; keep && a < ngames; ) {
        int b = a;
        bool has_keep = false;
        while (b < ngames && !strcmp(games[ix[b]].name, games[ix[a]].name)) has_keep |= !strcmp(games[ix[b++]].core, keep);
        if (b - a > 1 && has_keep)
            for (int k = a; k < b; k++)
                if (strcmp(games[ix[k]].core, keep) && strcmp(games[ix[k]].core, "linux")) games[ix[k]].hidden = true;
        a = b;
    }
    free(ix);
    for (int i = 0; i < ngames; i++) games[i].hidden |= games[i].user_hidden;
}

/* Filtro per genere: categorie principali presenti, ordinate per nome tradotto. */
static char genres[48][48];
static int ngenres, genre_sel = -1;

static void genre_main(const char *raw, char *out, size_t n) {
    snprintf(out, n, "%s", raw);
    char *slash = strchr(out, '/');
    if (slash) *slash = 0;
    for (size_t e = strlen(out); e > 0 && out[e - 1] == ' '; e--) out[e - 1] = 0;
}

static const char *genre_label(const char *raw, char *out, size_t n);

static void build_genres(void) {
    char cur[48] = "", m[48], la[64], lb[64];
    if (genre_sel >= 0) snprintf(cur, sizeof cur, "%s", genres[genre_sel]);
    ngenres = 0;
    for (int i = 0; i < ngames && ngenres < 48; i++) {
        if (games[i].hidden) continue;
        genre_main(games[i].genre, m, sizeof m);
        bool known = !m[0];
        for (int k = 0; k < ngenres && !known; k++) known = !strcmp(genres[k], m);
        if (!known) snprintf(genres[ngenres++], 48, "%s", m);
    }
    for (int a = 0; a < ngenres; a++)
        for (int b = a + 1; b < ngenres; b++)
            if (strcasecmp(genre_label(genres[a], la, sizeof la), genre_label(genres[b], lb, sizeof lb)) > 0) {
                char t[48];
                memcpy(t, genres[a], 48); memcpy(genres[a], genres[b], 48); memcpy(genres[b], t, 48);
            }
    genre_sel = -1;
    for (int k = 0; k < ngenres && cur[0]; k++)
        if (!strcmp(genres[k], cur)) genre_sel = k;
}

static void build_cats(void);

static void build_carousel(void) {
    ncarousel = 0;
    for (int r = 1; r <= 20 && ncarousel < 10; r++)
        for (int i = 0; i < ngames; i++)
            if (games[i].recent == r && !games[i].hidden && !games[i].missing) { carousel[ncarousel++] = i; break; }
    for (int i = 0; i < ngames && ncarousel < 10; i++)
        if (games[i].fav && !games[i].recent && !games[i].hidden && !games[i].missing) carousel[ncarousel++] = i;
    for (int i = 0; i < ngames && ncarousel < 6; i++) {
        if (games[i].hidden || games[i].missing) continue;
        bool dup = false;
        for (int k = 0; k < ncarousel; k++) dup |= carousel[k] == i;
        if (!dup) carousel[ncarousel++] = i;
    }
}

static const char *tab_label(int t) {
    if (t >= TAB_COLL) return COLLS[t - TAB_COLL];
    if (t >= TAB_EMU) return EMUS[t - TAB_EMU].name;
    switch (t) {
    case TAB_ALL: return T("library.all");
    case TAB_LINUX: return "Linux";
    default: return T("library.favorites");
    }
}

/* nella vista "Nascosti" si vedono solo i giochi nascosti dall'utente */
static bool visible(const game_t *g) { return coll_sel == COLL_HIDDEN ? g->user_hidden : !g->hidden; }
static bool in_coll(const game_t *g) { return coll_sel < 0 || (g->colls >> coll_sel & 1u); }

static bool in_tab(const game_t *g, int t) {
    if (!visible(g)) return false;
    if (t >= TAB_COLL) return g->colls >> (t - TAB_COLL) & 1u;
    if (t >= TAB_EMU) return !strcmp(g->core, EMUS[t - TAB_EMU].id);
    switch (t) {
    case TAB_LINUX: return !strcmp(g->core, "linux");
    case TAB_FAV: return g->fav;
    default: return true;
    }
}

static void build_tabs(void) {
    ntabs = 0;
    tabs[ntabs++] = TAB_ALL;
    int cand[18], nc = 0;
    for (int e = 0; e < NEMUS && nc < 16; e++) cand[nc++] = TAB_EMU + e;
    cand[nc++] = TAB_LINUX;
    for (int c = 0; c < nc; c++) {
        bool has = false;
        for (int i = 0; i < ngames && !has; i++) has = in_tab(&games[i], cand[c]);
        if (has) tabs[ntabs++] = cand[c];
    }
    tabs[ntabs++] = TAB_FAV;
    for (int c = 0; c < NCOLLS && ntabs < 24; c++) {
        bool has = false;
        for (int i = 0; i < ngames && !has; i++) has = in_tab(&games[i], TAB_COLL + c);
        if (has) tabs[ntabs++] = TAB_COLL + c;
    }
    if (tab_sel >= ntabs) tab_sel = 0;
}

static bool rom_problem(const game_t *g) {
    return g->romstatus == RC_MISSING || g->romstatus == RC_BADCRC || g->romstatus == RC_NOPARENT;
}

/* ricerca: parole cercate nel titolo o nel nome ROM, senza distinguere maiuscole */
static char search[48];

static bool ci_contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return !n;
}

static bool matches_search(const game_t *g) {
    if (!search[0]) return true;
    char words[48], *save = NULL;
    snprintf(words, sizeof words, "%s", search);
    for (char *w = strtok_r(words, " ", &save); w; w = strtok_r(NULL, " ", &save))
        if (!ci_contains(g->title, w) && !ci_contains(g->name, w)) return false;
    return true;
}

/* ordinamento: a parità resta l'ordine per titolo (indice nell'elenco) */
static int view_cmp(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    const game_t *x = &games[ia], *y = &games[ib];
    long d = 0;
    switch (UI.set.sort_mode) {
    case 1: {
        int ya = atoi(x->year), yb = atoi(y->year);
        d = (ya ? ya : 9999) - (yb ? yb : 9999);
        break;
    }
    case 2: d = y->seconds != x->seconds ? (y->seconds > x->seconds ? 1 : -1) : y->plays - x->plays; break;
    case 3: d = y->last_played != x->last_played ? (y->last_played > x->last_played ? 1 : -1) : 0; break;
    }
    return d ? (d > 0 ? 1 : -1) : ia - ib;
}

static void build_view(void) {
    free(view);
    view = malloc(sizeof(int) * (size_t)(ngames + 1));
    nview = 0;
    char m[48];
    for (int i = 0; i < ngames; i++) {
        if (!in_tab(&games[i], tabs[tab_sel])) continue;
        if (genre_sel >= 0) {
            genre_main(games[i].genre, m, sizeof m);
            if (strcmp(m, genres[genre_sel])) continue;
        }
        if (!matches_search(&games[i]) || !in_coll(&games[i])) continue;
        if (only_problems && !rom_problem(&games[i])) continue;
        view[nview++] = i;
    }
    if (UI.set.sort_mode) qsort(view, (size_t)nview, sizeof(int), view_cmp);
    if (lib_sel >= nview) lib_sel = nview ? nview - 1 : 0;
}

static void reload_games(void) {
    games_free(games, ngames);
    emulators_load(UI.root);
    ngames = games_scan(UI.root, &games, UI.set.show_missing);
    ui_mark("lettura dei giochi");
    games_mark_lists(UI.root, games, ngames);
    romcheck_load(UI.root, games, ngames);
    mark_duplicates();
    build_genres();
    build_cats();
    build_carousel();
    build_tabs();
    build_view();
    ui_log("giochi trovati: %d", ngames);
}

/* Giochi Linux: le variabili "env" di game.json vanno in System/next.env, che lo
   script di avvio legge prima di lanciare launch.sh (es. SDL_GAMECONTROLLERCONFIG).
   La voce "keys" ({"A": "SPACE", "START": "ENTER"}) diventa GAMEWATCH_KEYS: gamewatch
   preme quei tasti su una tastiera virtuale per i giochi che non leggono i controller. */
static void write_linux_env(const game_t *g) {
    char meta[1200], envp[1100];
    snprintf(envp, sizeof envp, "%s/System/next.env", UI.root);
    snprintf(meta, sizeof meta, "%.*s/game.json", (int)(strrchr(g->path, '/') - g->path), g->path);
    jval *j = json_load(meta), *env = json_get(j, "env"), *keys = json_get(j, "keys");
    bool has_env = env && env->t == J_OBJ, has_keys = keys && keys->t == J_OBJ;
    FILE *f = has_env || has_keys ? fopen(envp, "w") : NULL;
    if (f && has_keys) {
        /* solo lettere, cifre e _: il valore va nello script senza bisogno di apici */
        fputs("export GAMEWATCH_KEYS='", f);
        bool first = true;
        for (jval *v = keys->child; v; v = v->next) {
            if (v->t != J_STR) continue;
            bool ok = v->key[0] && v->s[0];
            for (const char *c = v->key; *c; c++) ok &= isalnum((unsigned char)*c) || *c == '_';
            for (const char *c = v->s; *c; c++) ok &= isalnum((unsigned char)*c) || *c == '_';
            if (!ok) continue;
            fprintf(f, "%s%s=%s", first ? "" : ",", v->key, v->s);
            first = false;
        }
        fputs("'\n", f);
    }
    if (!has_env) env = NULL;
    for (jval *v = f && env ? env->child : NULL; v; v = v->next) {
        if (v->t != J_STR) continue;
        bool valid = v->key[0] != 0;
        for (const char *c = v->key; *c; c++)
            valid &= (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!valid) continue;
        /* valore tra apici singoli: un apice diventa '\'' */
        fprintf(f, "export %s='", v->key);
        for (const char *c = v->s; *c; c++) {
            if (*c == '\'') fputs("'\\''", f);
            else fputc(*c, f);
        }
        fputs("'\n", f);
    }
    if (f) fclose(f);
    else unlink(envp);
    json_free(j);
}

/* altoparlante spento durante i lavori su SD e Wi-Fi: si spegne PRIMA di iniziare, perché il
   rumore arriva già con le prime letture; lo riaccende upd_audio quando il lavoro finisce */
static bool audio_quiet;

static void quiet_begin(void) {
    if (audio_quiet) return;
    audio_quiet = true;
    ui_log("altoparlante spento (lavoro su SD o Wi-Fi)");
    plat_audio_mute();
}

/* conferme delle azioni che fanno perdere qualcosa (Annulla già selezionato) */
enum { CFM_NONE, CFM_FORGET, CFM_GENRES };
static int cfm_kind, cfm_sel;
static char cfm_ssid[33];

static bool have_games(void) {
    for (int i = 0; i < ngames; i++)
        if (!games[i].missing) return true;
    return false;
}

/* scheda senza giochi: dove copiarli, con le cartelle vere degli emulatori presenti */
static void empty_guide(int x, int y, int w) {
    theme_t *t = &UI.th;
    char dirs[400] = "", step2[600], lines[3][160];
    for (int e = 0; e < NEMUS && e < 3; e++) {
        size_t len = strlen(dirs);
        snprintf(dirs + len, sizeof dirs - len, "%sArcade/%s", e ? ", " : "", EMUS[e].roms);
    }
    snprintf(step2, sizeof step2, T("empty.step2"), dirs);
    const char *steps[3] = { T("empty.step1"), step2, T("empty.step3") };
    ui_text(UI.f_normal, T("empty.title"), x, y, t->accent, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_normal) + M(0.012f);
    for (int s = 0; s < 3; s++) {
        int n = ui_wrap(UI.f_small, steps[s], w, lines, 3);
        for (int i = 0; i < n; i++, y += TTF_FontHeight(UI.f_small)) ui_text(UI.f_small, lines[i], x, y, t->text, ALIGN_LEFT);
        y += M(0.01f);
    }
}

static void launch(game_t *g) {
    char path[1100], core[1100];
    if (g->missing) {
        const emulator_t *emu = emulator_find(g->core);
        char msg[300];
        snprintf(msg, sizeof msg, T("missing.toast"), g->name, emu ? emu->roms : "Roms");
        ui_toast(msg);
        ui_sound(SND_ERROR);
        return;
    }
    games_add_recent(UI.root, g);
    if (!strcmp(g->core, "linux")) write_linux_env(g);
    if (!strcmp(g->core, "linux")) snprintf(core, sizeof core, "-");
    else {
        const emulator_t *emu = emulator_find(g->core);
        snprintf(core, sizeof core, "%s/Cores/%s.%s", UI.root, emu ? emu->core : g->core, CORE_EXT);
    }
    snprintf(path, sizeof path, "%s/System/next.txt", UI.root);
    FILE *f = fopen(path, "w");
    if (!f) {
        ui_log("impossibile scrivere %s: %s", path, strerror(errno));
        ui_toast(T("common.launch_error"));
        ui_sound(SND_ERROR);
        return;
    }
    fprintf(f, "%s\t%s\t%s\t%s\n", !strcmp(g->core, "linux") ? "linux" : "libretro", core, g->path, g->title);
    fclose(f);
    ui_log("avvio %s (%s)", g->title, g->core);
    ui_sound(SND_OK);
    char msg[200];
    snprintf(msg, sizeof msg, T("common.starting"), g->title);
    ui_busy(msg, 0.35);
    music_quit();
    ui_shutdown_code(EXIT_LAUNCH);
    exit(EXIT_LAUNCH);
}

static void power_off(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/resume.txt", UI.root);
    unlink(p);   /* spento dal menu: al riavvio non c'è una partita da riprendere */
    settings_save(&UI.set, UI.root);
    ui_busy(T("common.shutting_down"), 0.5);
    music_quit();
    ui_shutdown_code(EXIT_POWEROFF);
    exit(EXIT_POWEROFF);
}

/* --------------------------------------------------------- copertine */

static color_t hsv(float h, float s, float v) {
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return rgba((Uint8)((r + m) * 255), (Uint8)((g + m) * 255), (Uint8)((b + m) * 255), 255);
}

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* Immagine del gioco: Media/<tipo>/<rom>.png nei tipi scelti dal tema (snap, title,
   marquee, flyer), poi Media/<core>/<rom>.png messa a mano; per Linux cover.png. */
static SDL_Texture *media_image(const game_t *g, char types[][16], int n) {
    char p[1400];
    if (!strcmp(g->core, "linux")) {
        snprintf(p, sizeof p, "%.*s/cover.png", (int)(strrchr(g->path, '/') - g->path), g->path);
        return ui_image(p);
    }
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/Media/%s/%s.png", UI.root, types[i], g->name);
        SDL_Texture *img = ui_image(p);
        if (img) return img;
    }
    snprintf(p, sizeof p, "%s/Media/%s/%s.png", UI.root, g->core, g->name);
    return ui_image(p);
}

/* Copertina: l'immagine scaricata se c'è, altrimenti una generata con colore per sistema e titolo. */
static void draw_cover(const game_t *g, int x, int y, int w, int s, int r) {
    SDL_Texture *img = media_image(g, UI.th.cover_types, UI.th.ncover);
    if (img && UI.th.cover_fit) {
        ui_rrect(x, y, w, s, r, rgba(14, 14, 18, 255));
        ui_image_fit(img, x, y, w, s);
    } else if (img) {
        ui_image_fill(img, x, y, w, s, r);
    } else {
        /* le misure seguono l'altezza s; la larghezza w può essere maggiore (riquadri 4:3) */
        float hue = (float)(hash(g->system) % 360), var = (float)(hash(g->name) % 40) - 20;
        color_t base = hsv(fmodf(hue + var + 360, 360), 0.55f, 0.62f);
        ui_rrect(x, y, w, s, r, base);
        ui_clip_push(x, y, w, s);
        ui_circle(x + w - s / 5, y + s / 4, s / 2, rgba(255, 255, 255, 22));
        ui_circle(x + s / 6, y + s, s / 3, rgba(0, 0, 0, 30));
        ui_clip_pop();
        TTF_Font *ft = s > M(0.3f) ? UI.f_title : UI.f_normal, *fs = UI.f_small;
        int pad = s / 12;
        /* etichetta del sistema */
        int bw = ui_text_w(fs, g->system) + pad, bh = TTF_FontHeight(fs) + pad / 3;
        ui_rrect(x + pad, y + pad, bw, bh, bh / 2, rgba(0, 0, 0, 70));
        ui_text(fs, g->system, x + pad + pad / 2, y + pad + pad / 6, rgba(255, 255, 255, 230), ALIGN_LEFT);
        char lines[4][160];
        char title[160];
        snprintf(title, sizeof title, "%s", g->title);
        char *paren = strstr(title, " (");
        if (paren) *paren = 0;   /* "(set 1)" e simili tolgono spazio al titolo */
        /* righe del titolo: quante ne stanno sotto l'etichetta del sistema */
        int room = s - pad * 3 - bh - (g->year[0] ? TTF_FontHeight(fs) : 0);
        int maxl = room / TTF_FontHeight(ft);
        maxl = maxl < 1 ? 1 : maxl > 3 ? 3 : maxl;
        int nl = ui_wrap(ft, title, w - pad * 2, lines, maxl);
        int lh = TTF_FontHeight(ft), ty = y + s - pad - nl * lh - (g->year[0] ? TTF_FontHeight(fs) : 0);
        for (int i = 0; i < nl; i++) ui_text(ft, lines[i], x + pad, ty + i * lh, rgba(255, 255, 255, 255), ALIGN_LEFT);
        if (g->year[0]) ui_text(fs, g->year, x + pad, ty + nl * lh, rgba(255, 255, 255, 190), ALIGN_LEFT);
    }
    if (rom_problem(g)) {
        TTF_Font *fs = UI.f_small;
        char key[32];
        snprintf(key, sizeof key, "rom.badge%d", g->romstatus);
        const char *lbl = T(key);
        int pad = s / 12, bw = ui_text_w(fs, lbl) + pad, bh = TTF_FontHeight(fs) + pad / 3;
        ui_rrect(x + w - pad - bw, y + s - pad - bh, bw, bh, bh / 2, rgba(230, 130, 20, 235));
        ui_text(fs, lbl, x + w - pad - bw / 2, y + s - pad - bh + pad / 6, rgba(255, 255, 255, 255), ALIGN_CENTER);
    }
    if (g->missing) {
        ui_rrect(x, y, w, s, r, rgba(0, 0, 0, 150));
        TTF_Font *fs = UI.f_small;
        const char *lbl = T("missing.badge");
        /* etichetta in alto a destra, dove non copre il titolo */
        int pad = s / 12, bw = ui_text_w(fs, lbl) + pad, bh = TTF_FontHeight(fs) + pad / 3;
        ui_rrect(x + w - pad - bw, y + pad, bw, bh, bh / 2, rgba(0, 0, 0, 170));
        ui_text(fs, lbl, x + w - pad - bw / 2, y + pad + pad / 6, rgba(255, 255, 255, 220), ALIGN_CENTER);
    }
    if (g->fav) {
        int sr = s / 14;
        ui_circle(x + w - sr * 2, y + sr * 2, sr + sr / 2, rgba(0, 0, 0, 90));
        ui_star(x + w - sr * 2, y + sr * 2, sr, rgba(255, 210, 60, 255));
    }
}

static void draw_all_tile(int x, int y, int w, int s, int r) {
    theme_t *t = &UI.th;
    ui_rrect(x, y, w, s, r, t->tile);
    int q = s / 7, g = q / 3, cx = x + w / 2, cy = y + s / 2 - s / 10;
    for (int i = 0; i < 4; i++)
        ui_rrect(cx - q - g / 2 + (i % 2) * (q + g), cy - q - g / 2 + (i / 2) * (q + g), q, q, q / 5, t->accent);
    ui_text(UI.f_normal, T("common.all_games"), cx, y + s - s / 4, t->text, ALIGN_CENTER);
}

static void selection_ring(int x, int y, int w, int h, int r) {
    theme_t *t = &UI.th;
    int b = M(t->select_border) < 2 ? 2 : M(t->select_border), o = b + M(0.006f);
    float a = t->pulse ? 0.75f + 0.25f * sinf((float)UI.t * 5) : 1;
    ui_rborder(x - o, y - o, w + 2 * o, h + 2 * o, r + o, b, with_alpha(t->accent, (int)(255 * a)));
}

/* ---------------------------------------------------------- icone */

enum { IC_ALL, IC_FAV, IC_THEMES, IC_SETTINGS, IC_POWER, IC_COUNT };
static const char *icon_label[IC_COUNT] = { "common.all_games", "home.favorites", "home.themes", "common.settings", "common.power_off" };

static void draw_icon_glyph(int kind, int cx, int cy, int s, color_t c, color_t bg) {
    int q = s / 6;
    switch (kind) {
    case IC_ALL:
        for (int i = 0; i < 4; i++) ui_rrect(cx - q - q / 4 + (i % 2) * (q + q / 2), cy - q - q / 4 + (i / 2) * (q + q / 2), q, q, q / 4, c);
        break;
    case IC_FAV: ui_star(cx, cy, s / 4, c); break;
    case IC_THEMES:
        ui_circle(cx - q, cy + q / 2, q * 2 / 3, c);
        ui_circle(cx + q, cy + q / 2, q * 2 / 3, with_alpha(c, 170));
        ui_circle(cx, cy - q, q * 2 / 3, with_alpha(c, 110));
        break;
    case IC_SETTINGS: ui_gear(cx, cy, s * 27 / 100, c); break;
    case IC_POWER:
        ui_ring(cx, cy + q / 6, s / 4, q / 3 + 1, c);
        ui_rect(cx - q / 2, cy - s / 4 - q / 3, q, q, bg);
        ui_rrect(cx - q / 6, cy - s / 4 - q / 4, q / 3 + 1, s / 4 + q / 4, q / 6, c);
        break;
    }
}

/* ----------------------------------------------------------- home */

static void home_activate_icon(void) {
    switch (icon_sel) {
    case IC_ALL: tab_sel = 0; build_view(); go(SCR_LIBRARY); break;
    case IC_FAV:
        for (int i = 0; i < ntabs; i++)
            if (tabs[i] == TAB_FAV) tab_sel = i;
        build_view();
        go(SCR_LIBRARY);
        break;
    case IC_THEMES: settings_open(SC_LOOK, 0); go(SCR_SETTINGS); break;
    case IC_SETTINGS: settings_open(-1, 0); go(SCR_SETTINGS); break;
    case IC_POWER: dlg_open = true; dlg_suspend = false; dlg_sel = 0; break;
    }
    ui_sound(SND_OK);
}

static void home_input(void) {
    input_t *in = &UI.in;
    int items = ncarousel + 1;
    if (in->pressed[BTN_UP] && home_row == 1) { home_row = 0; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN] && home_row == 0) { home_row = 1; ui_sound(SND_MOVE); }
    if (home_row == 0) {
        if (in->pressed[BTN_LEFT] && home_sel > 0) { home_sel--; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_RIGHT] && home_sel < items - 1) { home_sel++; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_A]) {
            if (home_sel < ncarousel) launch(&games[carousel[home_sel]]);
            else { tab_sel = 0; build_view(); go(SCR_LIBRARY); ui_sound(SND_OK); }
        }
        if (in->pressed[BTN_X] && home_sel < ncarousel) { open_detail(carousel[home_sel]); return; }
        if (in->pressed[BTN_Y] && home_sel < ncarousel) {
            games_toggle_fav(UI.root, &games[carousel[home_sel]]);
            ui_toast(games[carousel[home_sel]].fav ? T("home.fav_added") : T("home.fav_removed"));
            build_tabs();
            ui_sound(SND_OK);
        }
    } else {
        if (in->pressed[BTN_LEFT] && icon_sel > 0) { icon_sel--; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_RIGHT] && icon_sel < IC_COUNT - 1) { icon_sel++; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_A]) home_activate_icon();
    }
    if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); }
}

static void home_draw(void) {
    theme_t *t = &UI.th;
    ui_topbar(t->title);
    static const float home_scale[3] = { 0.8f, 1.0f, 1.2f };
    int S = (int)(M(t->tile_size) * home_scale[UI.set.home_size % 3]), SW = (int)(S * t->tile_aspect), gap = M(t->tile_gap), mx = M(t->margin), r = M(t->tile_radius);
    int top = M(t->topbar_h), label_h = TTF_FontHeight(UI.f_normal);
    int y = top + label_h + M(0.05f);
    int items = ncarousel + 1;
    float sx = (float)home_sel * (SW + gap), view_w = (float)(UI.w - 2 * mx);
    if (sx - home_scroll_target > view_w - SW) home_scroll_target = sx - (view_w - SW);
    if (sx < home_scroll_target) home_scroll_target = sx;
    home_scroll = ui_approach(home_scroll, home_scroll_target);

    for (int i = 0; i < items; i++) {
        int x = mx + (int)(i * (SW + gap) - home_scroll);
        if (x > UI.w || x + SW < 0) continue;
        ui_shadow(x, y + M(0.006f), SW, S, r, M(0.018f), t->shadow);
        if (i < ncarousel) draw_cover(&games[carousel[i]], x, y, SW, S, r);
        else draw_all_tile(x, y, SW, S, r);
    }
    if (ncarousel == 0) {
        if (!have_games()) empty_guide(mx + SW + gap * 2, y, UI.w - (mx + SW + gap * 2) - mx);
        else ui_text(UI.f_normal, T("home.no_games"), mx + SW + gap * 2, y + S / 2 - label_h / 2, t->text_dim, ALIGN_LEFT);
    }

    /* icone */
    int is = M(t->icon_size), step = is + M(0.045f);
    int iy = y + S + (UI.h - M(t->hintbar_h) - (y + S)) / 2 - M(0.01f);
    int ix0 = UI.w / 2 - (IC_COUNT - 1) * step / 2;
    for (int i = 0; i < IC_COUNT; i++) {
        int cx = ix0 + i * step;
        ui_shadow(cx - is / 2, iy - is / 2 + M(0.004f), is, is, is / 2, M(0.012f), t->shadow);
        ui_circle(cx, iy, is / 2, t->surface);
        color_t ic = i == IC_POWER ? t->text : t->accent;
        draw_icon_glyph(i, cx, iy, is, ic, t->surface);
    }

    /* selezione animata */
    float tx, ty;
    int tw, th, tr;
    if (home_row == 0) {
        tx = (float)(mx + home_sel * (SW + gap) - home_scroll_target);
        ty = (float)y;
        tw = SW;
        th = S;
        tr = r;
    } else {
        tx = (float)(ix0 + icon_sel * step - is / 2);
        ty = (float)(iy - is / 2);
        tw = th = is;
        tr = is / 2;
    }
    if (!ring_init) { ring_x = tx; ring_y = ty; ring_init = true; }
    ring_x = ui_approach(ring_x, tx);
    ring_y = ui_approach(ring_y, ty);
    selection_ring((int)ring_x, (int)ring_y, tw, th, tr);

    if (home_row == 0) {
        char buf[200];
        const char *label = home_sel < ncarousel ? games[carousel[home_sel]].title : T("common.all_games");
        int lx = (int)ring_x;
        ui_text_fit(UI.f_normal, label, UI.w - mx - lx, buf, sizeof buf);
        ui_text(UI.f_normal, buf, lx, y - label_h - M(0.018f), t->accent, ALIGN_LEFT);
    } else {
        ui_text(UI.f_small, T(icon_label[icon_sel]), ix0 + icon_sel * step, iy + is / 2 + M(0.022f), t->accent, ALIGN_CENTER);
    }

    if (home_row == 0 && home_sel < ncarousel) {
        const char *h[] = { "X", T("detail.hint"), "Y", T(games[carousel[home_sel]].fav ? "common.unfavorite" : "common.favorite"), "START", T("common.settings"), "A", T("common.play") };
        ui_hints(h, 8);
    } else {
        const char *h[] = { "START", T("common.settings"), "A", T("common.ok") };
        ui_hints(h, 4);
    }
}

/* ------------------------------------------------------ layout a barre */
/* Categorie in orizzontale (impostazioni, recenti, preferiti, tutti, generi) e
   giochi della categoria in verticale, con anteprima a destra. */

enum { CAT_SETTINGS, CAT_RECENT, CAT_FAV, CAT_ALL, CAT_GENRE, CAT_COLL };
typedef struct { int kind, genre; } cat_t;
static cat_t cats[64];
static int ncats, cat_sel = -1, cat_item[64];
static float cat_anim, item_anim;
static int bar_list[2048], nbar;

static bool is_bar_layout(void) { return !strcmp(UI.th.layout, "bar"); }
static void game_info(const game_t *g, char *out, size_t n);

static void build_cats(void) {
    int prev_kind = cat_sel >= 0 ? cats[cat_sel].kind : CAT_ALL, prev_genre = cat_sel >= 0 ? cats[cat_sel].genre : -1;
    bool any_recent = false;
    for (int i = 0; i < ngames; i++) any_recent |= games[i].recent && !games[i].hidden;
    ncats = 0;
    cats[ncats++] = (cat_t){ CAT_SETTINGS, -1 };
    if (any_recent) cats[ncats++] = (cat_t){ CAT_RECENT, -1 };
    cats[ncats++] = (cat_t){ CAT_FAV, -1 };
    for (int c = 0; c < NCOLLS && ncats < 32; c++) {
        bool has = false;
        for (int i = 0; i < ngames && !has; i++) has = !games[i].hidden && (games[i].colls >> c & 1u);
        if (has) cats[ncats++] = (cat_t){ CAT_COLL, c };
    }
    cats[ncats++] = (cat_t){ CAT_ALL, -1 };
    for (int g = 0; g < ngenres && ncats < 64; g++) cats[ncats++] = (cat_t){ CAT_GENRE, g };
    cat_sel = 0;
    for (int i = 0; i < ncats; i++)
        if (cats[i].kind == prev_kind && cats[i].genre == prev_genre) cat_sel = i;
    if (cats[cat_sel].kind == CAT_SETTINGS && prev_kind != CAT_SETTINGS) cat_sel = ncats > 3 ? 3 : 0;
    cat_anim = (float)cat_sel;
}

static bool matches_search(const game_t *g);

static void bar_fill(void) {
    cat_t c = cats[cat_sel];
    char m[48];
    nbar = 0;
    if (c.kind == CAT_RECENT) {
        for (int r = 1; r <= 20; r++)
            for (int i = 0; i < ngames; i++)
                if (games[i].recent == r && visible(&games[i]) && in_coll(&games[i]) && matches_search(&games[i])) bar_list[nbar++] = i;
        return;
    }
    for (int i = 0; i < ngames && nbar < 2048; i++) {
        if (!visible(&games[i]) || !in_coll(&games[i])) continue;
        if (c.kind == CAT_FAV && !games[i].fav) continue;
        if (c.kind == CAT_COLL && !(games[i].colls >> c.genre & 1u)) continue;
        if (c.kind == CAT_GENRE) {
            genre_main(games[i].genre, m, sizeof m);
            if (strcmp(m, genres[c.genre])) continue;
        }
        if (c.kind == CAT_SETTINGS || !matches_search(&games[i])) continue;
        bar_list[nbar++] = i;
    }
}

static const char *cat_label(cat_t c, char *buf, size_t n) {
    switch (c.kind) {
    case CAT_SETTINGS: return T("common.settings");
    case CAT_RECENT: return T("bar.recent");
    case CAT_FAV: return T("home.favorites");
    case CAT_ALL: return T("common.all_games");
    case CAT_COLL: return COLLS[c.genre];
    default: return genre_label(genres[c.genre], buf, n);
    }
}

/* Sigla del genere: iniziale più la prima lettera successiva che la rende diversa
   dalle sigle degli altri generi ("Pi" Picchiaduro, "Pa" Piattaforme). Solo ASCII:
   una lettera accentata come seconda lettera viene saltata. */
static void genre_abbr(int gi, char *out, size_t n) {
    char la[64], lb[64];
    const char *l = genre_label(genres[gi], la, sizeof la);
    char first = l[0] >= 'a' && l[0] <= 'z' ? (char)(l[0] - 32) : l[0];
    for (const char *p = l + 1; *p; p++) {
        char c = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        if (c < 'a' || c > 'z') continue;
        bool clash = false;
        for (int k = 0; k < gi && !clash; k++) {
            char other[16];
            genre_abbr(k, other, sizeof other);
            clash = other[0] == first && other[1] == c;
        }
        if (!clash) { snprintf(out, n, "%c%c", first, c); return; }
    }
    snprintf(out, n, "%c", first);
    (void)lb;
}

static void draw_cat_icon(cat_t c, int cx, int cy, int s, color_t col) {
    switch (c.kind) {
    case CAT_SETTINGS: ui_gear(cx, cy, s * 32 / 100, col); break;
    case CAT_FAV: draw_icon_glyph(IC_FAV, cx, cy, s * 13 / 10, col, col); break;
    case CAT_ALL: draw_icon_glyph(IC_ALL, cx, cy, s * 13 / 10, col, col); break;
    case CAT_RECENT:
        /* orologio */
        ui_ring(cx, cy, s * 30 / 100, s / 16 + 1, col);
        ui_rrect(cx - s / 32, cy - s / 5, s / 16 + 1, s / 5, s / 32, col);
        ui_rrect(cx - s / 32, cy - s / 32, s / 7, s / 16 + 1, s / 32, col);
        break;
    case CAT_COLL: {
        /* collezione: due schede sovrapposte con l'iniziale */
        int q = s * 52 / 100, th = s / 16 + 1;
        char ini[8];
        snprintf(ini, sizeof ini, "%.*s", COLLS[c.genre][0] & 0x80 ? 2 : 1, COLLS[c.genre]);
        ui_rborder(cx - q / 2 + s / 10, cy - q / 2 - s / 10, q, q, s / 10, th, with_alpha(col, 140));
        ui_rborder(cx - q / 2, cy - q / 2, q, q, s / 10, th, col);
        ui_text(UI.f_small, ini, cx, cy - TTF_FontHeight(UI.f_small) / 2, col, ALIGN_CENTER);
        break;
    }
    default: {
        /* genere: sigla di due lettere in un anello */
        char ab[16];
        genre_abbr(c.genre, ab, sizeof ab);
        ui_ring(cx, cy, s * 32 / 100, s / 16 + 1, col);
        ui_text(UI.f_normal, ab, cx, cy - TTF_FontHeight(UI.f_normal) / 2, col, ALIGN_CENTER);
    }
    }
}

static void bar_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_LEFT] && cat_sel > 0) { cat_sel--; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_RIGHT] && cat_sel < ncats - 1) { cat_sel++; ui_sound(SND_MOVE); }
    bar_fill();
    int *it = &cat_item[cat_sel];
    if (*it >= nbar) *it = nbar ? nbar - 1 : 0;
    if (in->pressed[BTN_UP] && *it > 0) { (*it)--; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN] && *it < nbar - 1) { (*it)++; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_L]) { *it = *it - 8 > 0 ? *it - 8 : 0; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_R] && nbar) { *it = *it + 8 < nbar ? *it + 8 : nbar - 1; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_SELECT]) { fpanel_open = true; fpanel_sel = 0; ui_sound(SND_OK); return; }
    if (in->pressed[BTN_START] || (in->pressed[BTN_A] && cats[cat_sel].kind == CAT_SETTINGS)) {
        settings_open(-1, 0);
        go(SCR_SETTINGS);
        ui_sound(SND_OK);
        return;
    }
    if (in->pressed[BTN_A] && nbar) launch(&games[bar_list[*it]]);
    if (in->pressed[BTN_X] && nbar) { open_detail(bar_list[cat_item[cat_sel]]); return; }
    if (in->pressed[BTN_Y] && nbar) {
        game_t *g = &games[bar_list[*it]];
        games_toggle_fav(UI.root, g);
        ui_toast(g->fav ? T("home.fav_added") : T("home.fav_removed"));
        build_carousel();
        ui_sound(SND_OK);
    }
}

static void bar_draw(void) {
    theme_t *t = &UI.th;
    ui_topbar(NULL);
    bar_fill();
    int is = M(t->icon_size), step = is * 17 / 10;
    int anchor = UI.w * 22 / 100, cy = M(0.27f);
    cat_anim = ui_approach(cat_anim, (float)cat_sel);
    char buf[200];

    /* categorie */
    for (int i = 0; i < ncats; i++) {
        float d = i - cat_anim;
        int cx = anchor + (int)(d * step);
        if (cx < -is || cx > UI.w + is) continue;
        bool sel = i == cat_sel;
        float k = fmaxf(0, 1 - fabsf(d));       /* 1 sulla categoria scelta, 0 sulle altre */
        int s = (int)(is * (1 + 0.3f * k));
        draw_cat_icon(cats[i], cx, cy, s, with_alpha(t->text, (int)(110 + 145 * k)));
        if (sel) ui_text(UI.f_small, cat_label(cats[i], buf, sizeof buf), cx, cy + s / 2 + M(0.01f), t->text, ALIGN_CENTER);
    }

    /* giochi della categoria, sotto l'icona scelta */
    int list_x = anchor - is / 2, list_y = cy + is + M(0.07f), rh = M(t->list_row_h) * 13 / 10;
    int list_w = UI.w * 52 / 100 - list_x;
    int *it = &cat_item[cat_sel];
    item_anim = ui_approach(item_anim, (float)*it);
    if (cats[cat_sel].kind == CAT_SETTINGS) {
        ui_text(UI.f_normal, T("bar.open_settings"), list_x, list_y, t->text, ALIGN_LEFT);
    } else if (!nbar) {
        if (!have_games()) empty_guide(list_x, list_y, UI.w - list_x - M(t->margin));
        else ui_text(UI.f_normal, T("library.empty"), list_x, list_y, t->text_dim, ALIGN_LEFT);
    }
    int bottom = UI.h - M(t->hintbar_h);
    ui_clip_push(0, list_y - M(0.01f), UI.w, bottom - list_y);
    for (int i = 0; i < nbar; i++) {
        float d = i - item_anim;
        int y = list_y + (int)(d * rh);
        if (y + rh < list_y || y > bottom) continue;
        if (d < -0.5f) continue;                 /* i giochi già passati spariscono sotto le categorie */
        game_t *g = &games[bar_list[i]];
        bool sel = i == *it;
        int th = rh * 8 / 10, tw = th * 4 / 3;
        SDL_Texture *img = media_image(g, t->cover_types, t->ncover);
        if (img) ui_image_fill(img, list_x, y + (rh - th) / 2, tw, th, M(0.006f));
        else ui_rrect(list_x, y + (rh - th) / 2, tw, th, M(0.006f), with_alpha(t->text, 40));
        TTF_Font *f = sel ? UI.f_title : UI.f_normal;
        ui_text_fit(f, g->title, list_w - tw - M(0.03f), buf, sizeof buf);
        int tx = list_x + tw + M(0.025f);
        if (sel) {
            ui_text(f, buf, tx, y + rh / 2 - TTF_FontHeight(f) + M(0.008f), t->text, ALIGN_LEFT);
            char info[300], fit[300];
            game_info(g, info, sizeof info);
            ui_text_fit(UI.f_small, info, list_w - tw - M(0.03f), fit, sizeof fit);
            ui_text(UI.f_small, fit, tx, y + rh / 2 + M(0.004f), t->accent, ALIGN_LEFT);
        } else {
            ui_text(f, buf, tx, y + (rh - TTF_FontHeight(f)) / 2, with_alpha(t->text, 170), ALIGN_LEFT);
        }
        if (g->fav) ui_star(list_x + tw - M(0.015f), y + (rh - th) / 2 + M(0.015f), M(0.011f), rgba(255, 210, 60, 255));
    }
    ui_clip_pop();

    /* anteprima del gioco scelto */
    if (nbar && cats[cat_sel].kind != CAT_SETTINGS) {
        game_t *g = &games[bar_list[*it]];
        int px = UI.w * 56 / 100, pw = UI.w - px - M(t->margin), py = list_y;
        int ph = pw * 3 / 4;
        if (py + ph > bottom - M(0.02f)) { ph = bottom - M(0.02f) - py; pw = ph * 4 / 3; }
        SDL_Texture *shot = media_image(g, t->preview_types, t->npreview);
        if (shot) {
            ui_shadow(px, py + M(0.01f), pw, ph, M(t->tile_radius), M(0.03f), t->shadow);
            ui_rrect(px, py, pw, ph, M(t->tile_radius), rgba(0, 0, 0, 255));
            ui_image_fit(shot, px, py, pw, ph);
        }
        static char marquee_type[1][16] = { "marquee" };
        SDL_Texture *mq = t->show_marquee ? media_image(g, marquee_type, 1) : NULL;
        if (mq) ui_image_fit(mq, px, cy - is / 2, pw, is + is / 2);
    }

    const char *h[] = { "SELECT", genre_hint(), "X", T("detail.hint"), "Y", T("common.favorite"), "A",
                        T(cats[cat_sel].kind == CAT_SETTINGS ? "common.ok" : "common.play") };
    ui_hints(h, 8);
}

/* -------------------------------------------------------- libreria */

static void lib_common_input(int cols) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_L] || in->pressed[BTN_R]) {
        tab_sel = (tab_sel + (in->pressed[BTN_R] ? 1 : ntabs - 1)) % ntabs;
        lib_sel = 0;
        lib_scroll_target = 0;
        build_view();
        ui_sound(SND_MOVE);
    }
    if (in->pressed[BTN_SELECT]) { fpanel_open = true; fpanel_sel = 0; ui_sound(SND_OK); return; }
    int old = lib_sel;
    if (in->pressed[BTN_RIGHT] && cols > 1 && lib_sel + 1 < nview) lib_sel++;
    if (in->pressed[BTN_LEFT] && cols > 1 && lib_sel > 0) lib_sel--;
    if (in->pressed[BTN_DOWN] && lib_sel + cols < nview) lib_sel += cols;
    else if (in->pressed[BTN_DOWN] && cols > 1 && lib_sel / cols < (nview - 1) / cols) lib_sel = nview - 1;
    if (in->pressed[BTN_UP] && lib_sel - cols >= 0) lib_sel -= cols;
    if (cols == 1) {
        /* nell'elenco, L2/R2 e sinistra/destra saltano di pagina */
        int page = 8;
        if ((in->pressed[BTN_RIGHT] || in->pressed[BTN_R2]) && nview) lib_sel = lib_sel + page < nview ? lib_sel + page : nview - 1;
        if ((in->pressed[BTN_LEFT] || in->pressed[BTN_L2])) lib_sel = lib_sel - page > 0 ? lib_sel - page : 0;
    }
    if (lib_sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_A] && nview) launch(&games[view[lib_sel]]);
    if (in->pressed[BTN_X] && nview) { open_detail(view[lib_sel]); return; }
    if (in->pressed[BTN_Y] && nview) {
        game_t *g = &games[view[lib_sel]];
        games_toggle_fav(UI.root, g);
        ui_toast(g->fav ? T("home.fav_added") : T("home.fav_removed"));
        if (tabs[tab_sel] == TAB_FAV) build_view();
        build_carousel();
        ui_sound(SND_OK);
    }
    if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); }
    if (in->pressed[BTN_B] && !is_list_layout()) { go(SCR_HOME); ui_sound(SND_BACK); }
}

static void draw_tabs(int y) {
    theme_t *t = &UI.th;
    TTF_Font *f = UI.f_normal;
    int x = M(t->margin), fh = TTF_FontHeight(f);
    ui_button_glyph("L", x + M(0.02f), y + fh / 2, M(0.04f));
    x += M(0.06f);
    for (int i = 0; i < ntabs; i++) {
        const char *l = tab_label(tabs[i]);
        int w = ui_text(f, l, x, y, i == tab_sel ? t->accent : t->text_dim, ALIGN_LEFT);
        if (i == tab_sel) ui_rrect(x, y + fh + M(0.008f), w, M(0.006f), M(0.003f), t->accent);
        x += w + M(0.045f);
    }
    ui_button_glyph("R", x + M(0.005f), y + fh / 2, M(0.04f));
}

/* Genere: ArcadeDB lo dà in inglese ("Platform / Shooter Scrolling"); la categoria
   principale si traduce con la chiave genre.<categoria> del file di lingua. */
static const char *genre_label(const char *raw, char *out, size_t n) {
    char main[48], key[80] = "genre.";
    genre_main(raw, main, sizeof main);
    size_t k = strlen(key);
    for (const char *c = main; *c && k < sizeof key - 4; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) key[k++] = *c;
        else if (*c >= 'A' && *c <= 'Z') key[k++] = (char)(*c + 32);
        else if (*c == '&') { memcpy(key + k, "and", 3); k += 3; }
        else if ((*c == ' ' || *c == '-') && key[k - 1] != '_') key[k++] = '_';
    }
    key[k] = 0;
    const char *tr = main[0] ? T_opt(key) : NULL;
    snprintf(out, n, "%s", tr ? tr : main);
    return out;
}

/* "1996 · Nazca · Neo Geo · Piattaforme · 2 giocatori": solo le parti note */
static void game_info(const game_t *g, char *out, size_t n) {
    const char *parts[5];
    char players[24] = "";
    int k = 0;
    char genre[48];
    if (g->players > 1) snprintf(players, sizeof players, T("info.players"), g->players);
    /* per MAME il "sistema" è il produttore: non ripeterlo */
    const char *sys = strncmp(g->maker, g->system, strlen(g->system)) ? g->system : "";
    const char *src[5] = { g->year, g->maker, sys, genre_label(g->genre, genre, sizeof genre), players };
    for (int i = 0; i < 5; i++)
        if (src[i][0]) parts[k++] = src[i];
    out[0] = 0;
    for (int i = 0; i < k; i++) {
        size_t len = strlen(out);
        snprintf(out + len, n - len, "%s%s", i ? " · " : "", parts[i]);
    }
}

/* "Genere: Tutti" / "Genere: Picchiaduro" per la barra dei tasti */
/* "Filtri" con il numero di filtri attivi, per la barra dei tasti */
static const char *genre_hint(void) {
    static char buf[96];
    int active = (genre_sel >= 0) + (search[0] != 0) + only_problems + (coll_sel != COLL_ALL);
    if (active) snprintf(buf, sizeof buf, "%s (%d)", T("filters.hint"), active);
    else snprintf(buf, sizeof buf, "%s", T("filters.hint"));
    return buf;
}

static void lib_footer(const game_t *g, int y) {
    theme_t *t = &UI.th;
    char info[300], fit[300];
    game_info(g, info, sizeof info);
    ui_text_fit(UI.f_normal, g->title, UI.w - 2 * M(t->margin), fit, sizeof fit);
    ui_text(UI.f_normal, fit, M(t->margin), y, t->text, ALIGN_LEFT);
    ui_text_fit(UI.f_small, info, UI.w - 2 * M(t->margin), fit, sizeof fit);
    ui_text(UI.f_small, fit, M(t->margin), y + TTF_FontHeight(UI.f_normal), t->text_dim, ALIGN_LEFT);
}

/* misura delle miniature: quella del tema, o ricavata dal numero scelto per riga */
static void grid_metrics(int *cols, int *tw, int *th) {
    theme_t *t = &UI.th;
    int gap = M(t->tile_gap), avail = UI.w - 2 * M(t->margin);
    if (UI.set.grid_columns) {
        *cols = UI.set.grid_columns;
        *tw = (avail - gap * (*cols - 1)) / *cols;
        *th = (int)(*tw / t->tile_aspect);
        return;
    }
    *th = M(t->grid_tile);
    *tw = (int)(*th * t->tile_aspect);
    *cols = (avail + gap) / (*tw + gap);
    if (*cols < 1) *cols = 1;
}

static int grid_cols(void) {
    int c, w, h;
    grid_metrics(&c, &w, &h);
    return c;
}

static void library_grid_draw(void) {
    theme_t *t = &UI.th;
    ui_topbar(T("library.title"));
    int top = M(t->topbar_h);
    draw_tabs(top);
    int cols, TW, TH, gap = M(t->tile_gap), r = M(t->tile_radius);
    grid_metrics(&cols, &TW, &TH);
    int gx = (UI.w - (cols * (TW + gap) - gap)) / 2;
    int area_y = top + TTF_FontHeight(UI.f_normal) + M(0.05f);
    int footer_h = TTF_FontHeight(UI.f_normal) + TTF_FontHeight(UI.f_small) + M(0.02f);
    int area_h = UI.h - M(t->hintbar_h) - footer_h - area_y;
    int rows = (area_h + gap) / (TH + gap);
    if (rows < 1) rows = 1;
    area_h = rows * (TH + gap) - gap;   /* solo righe intere: niente riquadri tagliati sopra il riepilogo */
    int row = lib_sel / cols;
    float ry = (float)row * (TH + gap);
    if (ry - lib_scroll_target > area_h - TH) lib_scroll_target = ry - (area_h - TH);
    if (ry < lib_scroll_target) lib_scroll_target = ry;
    lib_scroll = ui_approach(lib_scroll, lib_scroll_target);

    int ring = M(t->select_border) + M(0.012f);   /* spazio per la cornice di selezione */
    ui_clip_push(0, area_y - ring, UI.w, area_h + 2 * ring);
    for (int i = 0; i < nview; i++) {
        int x = gx + (i % cols) * (TW + gap), y = area_y + (int)((i / cols) * (TH + gap) - lib_scroll);
        if (y > area_y + area_h || y + TH < area_y - TH) continue;
        ui_shadow(x, y + M(0.005f), TW, TH, r, M(0.014f), t->shadow);
        draw_cover(&games[view[i]], x, y, TW, TH, r);
    }
    if (nview) {
        int x = gx + (lib_sel % cols) * (TW + gap), y = area_y + (int)(row * (TH + gap) - lib_scroll);
        selection_ring(x, y, TW, TH, r);
    }
    ui_clip_pop();
    if (!nview) {
        if (!have_games()) empty_guide(M(t->margin) * 2, area_y + area_h / 5, UI.w - M(t->margin) * 4);
        else ui_text(UI.f_normal, T(tabs[tab_sel] == TAB_FAV ? "library.empty_fav" : "library.empty"),
                     UI.w / 2, area_y + area_h / 2, t->text_dim, ALIGN_CENTER);
    } else {
        lib_footer(&games[view[lib_sel]], UI.h - M(t->hintbar_h) - footer_h);
    }
    const char *h[] = { "SELECT", genre_hint(), "X", T("detail.hint"), "Y", T("common.favorite"), "A", T("common.play") };
    ui_hints(h, 8);
}

static void library_list_draw(void) {
    theme_t *t = &UI.th;
    ui_topbar(t->title);
    int top = M(t->topbar_h), mx = M(t->margin);
    draw_tabs(top);
    int rh = M(t->list_row_h), list_x = mx, list_w = UI.w * 52 / 100 - mx;
    int area_y = top + TTF_FontHeight(UI.f_normal) + M(0.05f), area_h = UI.h - M(t->hintbar_h) - area_y - M(0.02f);
    float ry = (float)lib_sel * rh;
    if (ry - lib_scroll_target > area_h - rh * 2) lib_scroll_target = ry - (area_h - rh * 2);
    if (ry < lib_scroll_target + rh && lib_sel > 0) lib_scroll_target = ry - rh;
    if (lib_scroll_target < 0) lib_scroll_target = 0;
    lib_scroll = ui_approach(lib_scroll, lib_scroll_target);
    lib_hl = ui_approach(lib_hl, ry);

    ui_clip_push(0, area_y, UI.w, area_h);
    if (nview) ui_rrect(list_x, area_y + (int)(lib_hl - lib_scroll), list_w, rh, M(t->panel_radius), t->accent);
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    for (int i = 0; i < nview; i++) {
        int y = area_y + (int)(i * rh - lib_scroll);
        if (y + rh < area_y || y > area_y + area_h) continue;
        game_t *g = &games[view[i]];
        bool sel = i == lib_sel;
        char buf[200];
        int sysw = ui_text_w(UI.f_small, g->system);
        ui_text_fit(f, g->title, list_w - sysw - M(0.08f), buf, sizeof buf);
        color_t c = sel ? t->accent_text : g->missing ? with_alpha(t->text, 110) : t->text;
        ui_text(f, buf, list_x + M(0.02f), y + (rh - fh) / 2, c, ALIGN_LEFT);
        ui_text(UI.f_small, g->system, list_x + list_w - M(0.02f), y + (rh - TTF_FontHeight(UI.f_small)) / 2,
                sel ? with_alpha(t->accent_text, 200) : t->text_dim, ALIGN_RIGHT);
        if (g->fav) ui_star(list_x + list_w - sysw - M(0.045f), y + rh / 2, M(0.012f), sel ? t->accent_text : t->accent);
    }
    ui_clip_pop();

    /* anteprima a destra */
    int px = UI.w * 55 / 100, pw = UI.w - px - mx;
    if (nview) {
        game_t *g = &games[view[lib_sel]];
        int y = area_y;
        /* marquee in alto, come l'insegna luminosa del cabinato */
        static char marquee_type[1][16] = { "marquee" };
        SDL_Texture *mq = UI.th.show_marquee ? media_image(g, marquee_type, 1) : NULL;
        if (mq) {
            int mh = pw / 4;
            ui_image_fit(mq, px, y, pw, mh);
            y += mh + M(0.02f);
        }
        SDL_Texture *shot = media_image(g, UI.th.preview_types, UI.th.npreview);
        if (shot) {
            /* schermata di gioco in un "monitor" 4:3, intera anche per i giochi verticali */
            int bw = pw, bh = pw * 3 / 4, maxh = (area_y + area_h - y) * 62 / 100;
            if (bh > maxh) { bh = maxh; bw = bh * 4 / 3; }
            int bx = px + (pw - bw) / 2;
            ui_shadow(bx, y + M(0.008f), bw, bh, M(t->tile_radius), M(0.02f), t->shadow);
            ui_rrect(bx, y, bw, bh, M(t->tile_radius), rgba(0, 0, 0, 255));
            ui_image_fit(shot, bx, y, bw, bh);
            y += bh + M(0.03f);
        } else {
            int cs = pw < (area_y + area_h - y) * 6 / 10 ? pw : (area_y + area_h - y) * 6 / 10;
            int cx = px + (pw - cs) / 2;
            ui_shadow(cx, y + M(0.008f), cs, cs, M(t->tile_radius), M(0.02f), t->shadow);
            draw_cover(g, cx, y, cs, cs, M(t->tile_radius));
            y += cs + M(0.03f);
        }
        char lines[3][160];
        int n = ui_wrap(UI.f_title, g->title, pw, lines, 2);
        for (int i = 0; i < n; i++, y += TTF_FontHeight(UI.f_title)) ui_text(UI.f_title, lines[i], px, y, t->text, ALIGN_LEFT);
        char info[300], fit[300];
        game_info(g, info, sizeof info);
        ui_text_fit(UI.f_small, info, pw, fit, sizeof fit);
        ui_text(UI.f_small, fit, px, y + M(0.01f), t->text_dim, ALIGN_LEFT);
        snprintf(info, sizeof info, "%s%s%s", games_core_label(g->core), g->vertical ? " · " : "", g->vertical ? T("info.vertical") : "");
        ui_text(UI.f_small, info, px, y + M(0.01f) + TTF_FontHeight(UI.f_small), t->text_dim, ALIGN_LEFT);
    } else {
        if (!have_games()) empty_guide(px, area_y, UI.w - px - M(t->margin));
        else ui_text(UI.f_normal, T("library.empty"), px, area_y, t->text_dim, ALIGN_LEFT);
    }
    const char *h[] = { "SELECT", genre_hint(), "X", T("detail.hint"), "Y", T("common.favorite"), "START", T("common.settings"), "A", T("common.play") };
    ui_hints(h, 10);
}

/* ----------------------------------------------------- impostazioni */

enum { S_BRIGHT, S_VOLUME, S_MUSIC, S_LED, S_LEDCOLOR, S_LEDBRIGHT, S_THEME, S_VARIANT, S_LANG, S_GRIDCOLS, S_HOMESIZE, S_ASPECT, S_FILTER, S_BEZEL, S_DUP, S_MISSING, S_GENRES, S_CPU, S_PADS, S_PADTEST, S_KEYF1, S_KEYF2, S_KEYHOME, S_WIFI, S_WIFINETS, S_SAVER, S_SLEEP, S_SUSPEND, S_RESUME, S_CLOCK, S_TZ, S_DEVICE, S_ROMCHECK, S_SCRAPE, S_CLEANMEDIA, S_UPDATE, S_CHANNEL, S_UPDNOTIFY, S_LOGLEVEL, S_INFO, S_POWER, S_COUNT };

/* chiavi "settings.<voce>" e "settings_desc.<voce>" nei file di lingua */
static const char *set_key[S_COUNT] = {
    "brightness", "volume", "music", "led", "led_color", "led_brightness", "theme", "variant", "language", "grid_columns", "home_size", "aspect", "filter", "bezel", "duplicates", "missing", "genres",
    "cpu", "pads", "pad_test", "key_f1", "key_f2", "key_home", "wifi", "wifi_nets", "screensaver", "sleep", "suspend", "resume", "clock", "timezone", "device", "romcheck", "scrape", "clean_media", "update", "channel", "update_notify", "log_level", "info", "power",
};

/* Categorie delle impostazioni: nella prima pagina le categorie e "Spegni la console" */
static const char *cat_key[SC_COUNT] = { "display", "look", "games", "controls", "system", "tools", "updates" };
static const int cat_items[SC_COUNT][10] = {
    { S_BRIGHT, S_VOLUME, S_MUSIC, S_LED, S_LEDCOLOR, S_LEDBRIGHT, -1 },
    { S_THEME, S_VARIANT, S_LANG, S_GRIDCOLS, S_HOMESIZE, S_SAVER, -1 },
    { S_ASPECT, S_FILTER, S_BEZEL, S_DUP, S_MISSING, S_GENRES, S_CPU, S_RESUME, -1 },
    { S_PADS, S_PADTEST, S_KEYF1, S_KEYF2, S_KEYHOME, -1 },
    { S_WIFI, S_WIFINETS, S_CLOCK, S_TZ, S_SUSPEND, S_SLEEP, S_DEVICE, -1 },
    { S_ROMCHECK, S_SCRAPE, S_CLEANMEDIA, -1 },
    { S_UPDATE, S_CHANNEL, S_UPDNOTIFY, S_LOGLEVEL, S_INFO, -1 },
};
#define ROW_CAT 1000   /* righe della prima pagina: ROW_CAT + categoria */

/* HOME laterale, F1 e F2 ci sono solo sulla Brick Pro (sul Mac si mostrano per le prove) */
static bool has_extra_keys(void) {
    const char *m = plat_model();
    return !plat_is_device() || (m && !strcmp(m, "brickpro"));
}

/* righe della pagina corrente: voci S_* oppure ROW_CAT + categoria */
static int page_rows(int *rows) {
    int n = 0;
    if (set_page < 0) {
        for (int c = 0; c < SC_COUNT; c++) rows[n++] = ROW_CAT + c;
        rows[n++] = S_POWER;
        return n;
    }
    for (int k = 0; k < 10 && cat_items[set_page][k] >= 0; k++) {
        int it = cat_items[set_page][k];
        if (it == S_DEVICE && plat_is_device()) continue;   /* misura dello schermo: solo nel prototipo */
        if (it == S_WIFINETS && !wifi_supported()) continue;
        if ((it == S_KEYF1 || it == S_KEYF2 || it == S_KEYHOME) && !has_extra_keys()) continue;
        if ((it == S_UPDATE || it == S_CHANNEL || it == S_UPDNOTIFY) && !update_supported()) continue;   /* nell'app sul firmware TrimUI no */
        rows[n++] = it;
    }
    return n;
}

/* voci che in questo momento non possono fare nulla: in grigio, con la descrizione che resta leggibile */
static bool set_disabled(int i) {
    switch (i) {
    case S_SCRAPE: case S_UPDATE: case S_WIFINETS: return !UI.set.wifi;
    case S_MUSIC: return !music_count();
    case S_LEDCOLOR: case S_LEDBRIGHT: return !UI.set.led_mode;
    case S_VARIANT: return UI.th.nvariants < 2;
    case S_GRIDCOLS: case S_HOMESIZE: return is_list_layout() || is_bar_layout();
    default: return false;
    }
}

static void settings_open(int page, int row) {
    set_page = page;
    set_row = row;
    int rows[16], n = page_rows(rows);
    if (set_row >= n) set_row = n - 1;
    set_sel = rows[set_row];
    set_hl = set_scroll = (float)set_row * (M(UI.th.list_row_h) + M(0.01f));
    set_scroll = 0;
}

static const char *set_text(const char *group, int item) {
    char k[64];
    snprintf(k, sizeof k, "%s.%s", group, set_key[item]);
    return T(k);
}

static const char *aspect_names[] = { "aspect.original", "aspect.integer", "aspect.full" };
static filter_t filters[24];
static int nfilters;

static int filter_index(void) {
    for (int i = 0; i < nfilters; i++)
        if (!strcmp(filters[i].id, UI.set.filter)) return i;
    return 0;
}
static const char *cpu_names[] = { "cpu.save", "cpu.balanced", "cpu.perf" };
static const char *saver_keys[5] = { "saver.off", "saver.shots2", "saver.shots5", "saver.black2", "saver.black5" };

static char lang_codes[16][8], lang_names[16][32];
static int nlangs;

static int lang_index(void) {
    for (int i = 0; i < nlangs; i++)
        if (!strcmp(lang_codes[i], UI.set.lang)) return i;
    return 0;
}
static const int sleep_vals[] = { 0, 5, 10, 30 };
static const int suspend_vals[] = { 0, 2, 5, 10, 30 };

static int theme_index(void) {
    for (int i = 0; i < nthemes; i++)
        if (!strcmp(theme_ids[i], UI.set.theme)) return i;
    return 0;
}

static int variant_index(void) {
    for (int i = 0; i < UI.th.nvariants; i++)
        if (!strcmp(UI.th.variants[i], UI.set.variant)) return i;
    return 0;
}

static const char *variant_label(const char *v) {
    char k[48];
    snprintf(k, sizeof k, "variant.%s", v);
    const char *tr = T_opt(k);
    return tr ? tr : v;
}

static void set_value(int item, char *buf, size_t n) {
    settings_t *s = &UI.set;
    switch (item) {
    case S_BRIGHT: snprintf(buf, n, "%d", s->brightness); break;
    case S_VOLUME: snprintf(buf, n, "%d", s->volume); break;
    case S_MUSIC:
        if (!music_count()) snprintf(buf, n, "%s", T("music.none"));
        else if (!s->music_vol) snprintf(buf, n, "%s", T("common.off"));
        else if (music_title()[0]) snprintf(buf, n, "%d · %s", s->music_vol, music_title());
        else snprintf(buf, n, "%d", s->music_vol);
        break;
    case S_LED: {
        static const char *k[4] = { "led.off", "led.static", "led.breath", "led.pulse" };
        snprintf(buf, n, "%s", T(k[s->led_mode & 3]));
        break;
    }
    case S_LEDCOLOR: snprintf(buf, n, "%s", T(LED_COLORS[s->led_color % NLED_COLORS].key)); break;
    case S_LEDBRIGHT: snprintf(buf, n, "%d", s->led_bright); break;
    case S_GRIDCOLS:
        if (s->grid_columns) snprintf(buf, n, "%d", s->grid_columns);
        else snprintf(buf, n, "%s", T("look.auto"));
        break;
    case S_HOMESIZE: {
        static const char *k[3] = { "look.small", "look.normal", "look.large" };
        snprintf(buf, n, "%s", T(k[s->home_size % 3]));
        break;
    }
    case S_UPDATE: snprintf(buf, n, "%s", update_current()); break;
    case S_CHANNEL: snprintf(buf, n, "%s", T(s->update_channel ? "update.channel_dev" : "update.channel_stable")); break;
    case S_UPDNOTIFY: snprintf(buf, n, "%s", T(s->update_notify ? "common.on" : "common.off")); break;
    case S_GENRES: snprintf(buf, n, "%d", ngenres); break;
    case S_PADTEST: buf[0] = 0; break;
    case S_KEYF1: case S_KEYF2: {
        static const char *acts[4] = { "keyact.none", "keyact.save", "keyact.load", "keyact.shot" };
        snprintf(buf, n, "%s", T(acts[item == S_KEYF1 ? s->key_f1 : s->key_f2]));
        break;
    }
    case S_KEYHOME: {
        static const char *acts[3] = { "keyhome.launcher", "keyhome.menu", "keyhome.none" };
        snprintf(buf, n, "%s", T(acts[s->key_home]));
        break;
    }
    case S_LOGLEVEL: {
        static const char *k[4] = { "loglvl.off", "loglvl.essential", "loglvl.perf", "loglvl.full" };
        snprintf(buf, n, "%s", T(k[s->log_level % 4]));
        break;
    }
    case S_THEME: snprintf(buf, n, "%s", nthemes ? theme_names[theme_index()] : UI.th.name); break;
    case S_VARIANT: snprintf(buf, n, "%s", UI.th.nvariants ? variant_label(UI.th.variants[variant_index()]) : "—"); break;
    case S_ASPECT: snprintf(buf, n, "%s", T(aspect_names[s->aspect % 3])); break;
    case S_FILTER: snprintf(buf, n, "%s", nfilters ? filters[filter_index()].name : s->filter); break;
    case S_BEZEL: snprintf(buf, n, "%s", T(s->bezel ? "common.on" : "common.off")); break;
    case S_CPU: snprintf(buf, n, "%s", T(cpu_names[s->cpu_profile % 3])); break;
    case S_DUP:
        if (s->dup_mode > 0 && s->dup_mode <= NEMUS) snprintf(buf, n, T("dup.prefer"), EMUS[s->dup_mode - 1].name);
        else snprintf(buf, n, "%s", T("dup.all"));
        break;
    case S_MISSING: snprintf(buf, n, "%s", T(s->show_missing ? "missing.shown" : "missing.hidden")); break;
    case S_WIFI: snprintf(buf, n, "%s", T(s->wifi ? "common.on" : "common.off")); break;
    case S_WIFINETS: {
        const wifi_status_t *ws = wifi_status(false);
        if (!s->wifi) snprintf(buf, n, "%s", T("common.off"));
        else if (ws->state == WIFI_CONNECTED) snprintf(buf, n, "%s", ws->ssid);
        else if (ws->state == WIFI_CONNECTING) snprintf(buf, n, "%s…", ws->ssid);
        else snprintf(buf, n, "%s", T("wifi.disconnected"));
        break;
    }
    case S_SAVER: snprintf(buf, n, "%s", T(saver_keys[s->saver % 5])); break;
    case S_PADS: {
        int np = ui_pads_external();
        char count[48];
        if (np == 0) snprintf(count, sizeof count, "%s", T("pad.count_none"));
        else if (np == 1) snprintf(count, sizeof count, "%s", T("pad.count_one"));
        else snprintf(count, sizeof count, T("pad.count_many"), np);
        snprintf(buf, n, T(s->ext_p1 ? "pad.ext_p1" : "pad.ext_p2"), count);
        break;
    }
    case S_SLEEP:
        if (s->autosleep_min) snprintf(buf, n, T("common.minutes"), s->autosleep_min);
        else snprintf(buf, n, "%s", T("common.never"));
        break;
    case S_SUSPEND:
        if (s->autosuspend_min) snprintf(buf, n, T("common.minutes"), s->autosuspend_min);
        else snprintf(buf, n, "%s", T("common.never"));
        break;
    case S_RESUME: snprintf(buf, n, "%s", T(s->resume_last ? "common.yes" : "common.no")); break;
    case S_CLOCK: {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(buf, n, "%d/%m/%Y  %H:%M", &tmv);
        break;
    }
    case S_TZ: {
        snprintf(buf, n, "%s", TIMEZONES[settings_tz_index(s)].id);
        for (char *c = buf; *c; c++) if (*c == '_') *c = ' ';
        break;
    }
    case S_DEVICE: snprintf(buf, n, "%s", device_label(s->device)); break;
    case S_INFO: snprintf(buf, n, T("info.games"), ngames); break;
    case S_LANG: snprintf(buf, n, "%s", nlangs ? lang_names[lang_index()] : UI.set.lang); break;
    default: buf[0] = 0;
    }
}

static void set_change(int item, int dir) {
    settings_t *s = &UI.set;
    switch (item) {
    case S_BRIGHT: s->brightness = s->brightness + dir < 1 ? 1 : s->brightness + dir > 10 ? 10 : s->brightness + dir; break;
    case S_VOLUME: s->volume = s->volume + dir < 0 ? 0 : s->volume + dir > 20 ? 20 : s->volume + dir; break;
    case S_MUSIC: {
        int was = s->music_vol;
        s->music_vol = was + dir < 0 ? 0 : was + dir > 10 ? 10 : was + dir;
        if (!was && s->music_vol) music_next();
        if (was && !s->music_vol) music_stop();
        break;
    }
    case S_THEME:
        if (nthemes > 1) {
            int i = (theme_index() + dir + nthemes) % nthemes;
            snprintf(s->theme, sizeof s->theme, "%s", theme_ids[i]);
            ui_reload_theme();
            if (UI.th.nvariants && strcmp(UI.th.variants[variant_index()], s->variant)) {
                snprintf(s->variant, sizeof s->variant, "%s", UI.th.variants[0]);
                ui_reload_theme();
            }
            ring_init = false;
        }
        break;
    case S_VARIANT:
        if (UI.th.nvariants > 1) {
            int i = (variant_index() + dir + UI.th.nvariants) % UI.th.nvariants;
            snprintf(s->variant, sizeof s->variant, "%s", UI.th.variants[i]);
            ui_reload_theme();
        }
        break;
    case S_LANG:
        if (nlangs > 1) {
            snprintf(s->lang, sizeof s->lang, "%s", lang_codes[(lang_index() + dir + nlangs) % nlangs]);
            lang_load(UI.root, s->lang);
            nfilters = filters_list(UI.root, s->lang, filters, 24);   /* nomi dei filtri nella nuova lingua */
            build_tabs();
        }
        break;
    case S_ASPECT: s->aspect = (s->aspect + dir + 3) % 3; break;
    case S_FILTER:
        if (nfilters) snprintf(s->filter, sizeof s->filter, "%s", filters[(filter_index() + dir + nfilters) % nfilters].id);
        break;
    case S_BEZEL: s->bezel = !s->bezel; break;
    case S_CPU: s->cpu_profile = (s->cpu_profile + dir + 3) % 3; break;
    case S_KEYF1: s->key_f1 = (s->key_f1 + dir + 4) % 4; break;
    case S_KEYF2: s->key_f2 = (s->key_f2 + dir + 4) % 4; break;
    case S_KEYHOME: s->key_home = (s->key_home + dir + 3) % 3; break;
    case S_DUP:
        s->dup_mode = (s->dup_mode + dir + NEMUS + 1) % (NEMUS + 1);
        mark_duplicates();
        build_genres();
        build_cats();
        build_carousel();
        build_tabs();
        build_view();
        break;
    case S_MISSING:
        s->show_missing = !s->show_missing;
        settings_save(s, UI.root);
        reload_games();
        break;
    case S_WIFI:
        s->wifi = !s->wifi;
        if (wifi_supported()) wifi_enable(s->wifi);
        break;
    case S_SAVER: s->saver = (s->saver + dir + 5) % 5; break;
    case S_PADS: s->ext_p1 = !s->ext_p1; break;
    case S_SLEEP: {
        int i = 0;
        while (i < 3 && sleep_vals[i] != s->autosleep_min) i++;
        s->autosleep_min = sleep_vals[(i + dir + 4) % 4];
        break;
    }
    case S_SUSPEND: {
        int i = 0;
        while (i < 4 && suspend_vals[i] != s->autosuspend_min) i++;
        s->autosuspend_min = suspend_vals[(i + dir + 5) % 5];
        break;
    }
    case S_UPDNOTIFY: s->update_notify = !s->update_notify; break;
    case S_CHANNEL:
        s->update_channel = !s->update_channel;
        if (s->update_channel) ui_toast(T("update.channel_warn"));
        break;
    case S_RESUME: s->resume_last = !s->resume_last; break;
    case S_LED: s->led_mode = (s->led_mode + dir + 4) % 4; break;
    case S_LEDCOLOR: s->led_color = (s->led_color + dir + NLED_COLORS) % NLED_COLORS; break;
    case S_LOGLEVEL: s->log_level = (s->log_level + dir + 4) % 4; break;
    case S_GRIDCOLS: {   /* 0 (tema), 3, 4, 5, 6, 7 */
        int c = s->grid_columns ? s->grid_columns - 2 : 0;
        c = (c + dir + 6) % 6;
        s->grid_columns = c ? c + 2 : 0;
        lib_scroll = lib_scroll_target = 0;
        ring_init = false;
        break;
    }
    case S_HOMESIZE:
        s->home_size = (s->home_size + dir + 3) % 3;
        ring_init = false;
        break;
    case S_LEDBRIGHT: s->led_bright = s->led_bright + dir < 1 ? 1 : s->led_bright + dir > 10 ? 10 : s->led_bright + dir; break;
    case S_TZ:
        snprintf(s->tz, sizeof s->tz, "%s", TIMEZONES[(settings_tz_index(s) + dir + NTIMEZONES) % NTIMEZONES].id);
        settings_apply_tz(s);
        break;
    case S_DEVICE:
        snprintf(s->device, sizeof s->device, "%s", !strcmp(s->device, "smartpro") ? "brickpro" : "smartpro");
        ui_apply_device();
        ring_init = false;
        break;
    default: return;
    }
    settings_save(s, UI.root);
    ui_sound(SND_MOVE);
}

/* Reti Wi-Fi: elenco con segnale, lucchetto e stato; A collega (password con la tastiera
   se serve), X dimentica una rete salvata, Y cerca di nuovo. */
/* ricerca ed elenco chiamano wpa_cli più volte: in un thread, così il menu non si ferma */
static wifi_net_t wnets_new[32];
static int wnets_new_n = -1;
static bool wfetching;

static int wfetch_worker(void *scan) {
    if (scan) wifi_scan();
    int n = wifi_networks(wnets_new, 32);
    __atomic_store_n(&wnets_new_n, n, __ATOMIC_SEQ_CST);
    __atomic_store_n(&wfetching, false, __ATOMIC_SEQ_CST);
    return 0;
}

static void wpanel_refresh(bool scan) {
    if (__atomic_load_n(&wfetching, __ATOMIC_SEQ_CST)) return;
    if (scan) wscan_t = UI.t;
    wlist_t = UI.t;
    __atomic_store_n(&wfetching, true, __ATOMIC_SEQ_CST);
    SDL_Thread *th = SDL_CreateThread(wfetch_worker, "wifi", scan ? (void *)1 : NULL);
    if (th) SDL_DetachThread(th);
    else __atomic_store_n(&wfetching, false, __ATOMIC_SEQ_CST);
}

/* quando il thread ha finito: la selezione resta sulla stessa rete anche se l'ordine cambia */
static void wpanel_collect(void) {
    if (__atomic_load_n(&wfetching, __ATOMIC_SEQ_CST)) return;
    int n = __atomic_load_n(&wnets_new_n, __ATOMIC_SEQ_CST);
    if (n < 0) return;
    __atomic_store_n(&wnets_new_n, -1, __ATOMIC_SEQ_CST);
    char cur[33];
    snprintf(cur, sizeof cur, "%s", wsel < nwnets ? wnets[wsel].ssid : "");
    memcpy(wnets, wnets_new, sizeof(wifi_net_t) * (size_t)n);
    nwnets = n;
    /* la selezione resta sulla stessa rete anche se l'ordine cambia */
    for (int i = 0; i < nwnets; i++) if (!strcmp(wnets[i].ssid, cur)) wsel = i;
    if (wsel >= nwnets) wsel = nwnets ? nwnets - 1 : 0;
}

static void wpanel_start(void) {
    quiet_begin();
    wpanel_open = true;
    wsel = 0;
    wscroll = 0;
    wifi_status(false);
    wpanel_refresh(true);
}

static void wifi_join(const char *ssid, const char *psk) {
    char m[160];
    if (wifi_connect(ssid, psk)) { snprintf(m, sizeof m, T("wifi.joining"), ssid); ui_sound(SND_OK); }
    else { snprintf(m, sizeof m, "%s", T("wifi.error")); ui_sound(SND_ERROR); }
    ui_toast(m);
    wifi_status(true);
    wpanel_refresh(false);
}

static void wpanel_input(void) {
    input_t *in = &UI.in;
    wpanel_collect();
    /* ricerca ogni 10 s, elenco e stato ogni 2 s */
    if (UI.t - wscan_t > 10) wpanel_refresh(true);
    else if (UI.t - wlist_t > 2) wpanel_refresh(false);
    if (in->pressed[BTN_UP] && nwnets) { wsel = (wsel + nwnets - 1) % nwnets; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN] && nwnets) { wsel = (wsel + 1) % nwnets; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { wpanel_open = false; ui_sound(SND_BACK); return; }
    if (in->pressed[BTN_Y]) { wpanel_refresh(true); ui_toast(T("wifi.searching")); ui_sound(SND_OK); }
    if (!nwnets) return;
    wifi_net_t *n = &wnets[wsel];
    if (in->pressed[BTN_X] && n->saved) {
        snprintf(cfm_ssid, sizeof cfm_ssid, "%s", n->ssid);
        cfm_kind = CFM_FORGET;
        cfm_sel = 0;
        ui_sound(SND_OK);
        return;
    }
    if (in->pressed[BTN_A]) {
        if (n->current) { ui_toast(T("wifi.already")); ui_sound(SND_MOVE); return; }
        snprintf(wifi_target, sizeof wifi_target, "%s", n->ssid);
        if (n->secured && (!n->saved || n->failed)) {
            /* password: tastiera in minuscolo, la più comune */
            wifi_pass[0] = 0;
            kb_mode = KBM_WIFI;
            kb_layout = 1;
            kb_row = 1;
            kb_col = 0;
            kb_open = true;
            ui_sound(SND_OK);
        } else wifi_join(n->ssid, NULL);
    }
}

static void lock_icon(int x, int y, int h, color_t c) {
    int w = h * 3 / 4, bh = h * 6 / 10, t = h > 16 ? 2 : 1;
    ui_rborder(x + w / 5, y, w * 3 / 5, h - bh + w / 5, w / 4, t, c);   /* archetto */
    ui_rrect(x, y + h - bh, w, bh, h / 8, c);
}

static void wpanel_draw(void) {
    theme_t *t = &UI.th;
    TTF_Font *f = UI.f_normal, *fs = UI.f_small;
    int fh = TTF_FontHeight(f), sh = TTF_FontHeight(fs);
    int pad = M(0.04f), rh = fh + M(0.03f), rows = 6;
    int w = M(0.95f), h = pad + TTF_FontHeight(UI.f_title) + M(0.01f) + sh + M(0.03f) + rows * rh + pad, x, y;
    if (h > UI.h - M(t->hintbar_h) - M(0.02f)) { rows = (UI.h - M(t->hintbar_h) - M(0.02f) - (h - rows * rh)) / rh; h = pad + TTF_FontHeight(UI.f_title) + M(0.01f) + sh + M(0.03f) + rows * rh + pad; }
    panel_box(w, h, &x, &y);
    int lx = x + pad, rx = x + w - pad, cy = y + pad;
    ui_text(UI.f_title, T("wifi.title"), lx, cy, t->text, ALIGN_LEFT);
    cy += TTF_FontHeight(UI.f_title) + M(0.01f);
    const wifi_status_t *ws = wifi_status(false);
    char line[160];
    if (ws->state == WIFI_CONNECTED) snprintf(line, sizeof line, T("wifi.connected"), ws->ssid, ws->ip);
    else if (ws->state == WIFI_CONNECTING) snprintf(line, sizeof line, T("wifi.connecting"), ws->ssid);
    else if (ws->state == WIFI_DISCONNECTED) snprintf(line, sizeof line, "%s", T("wifi.disconnected"));
    else snprintf(line, sizeof line, "%s", T("wifi.starting"));
    ui_text(fs, line, lx, cy, ws->state == WIFI_CONNECTED ? t->accent : t->text_dim, ALIGN_LEFT);
    cy += sh + M(0.03f);
    int top = cy, area = rows * rh;
    if (!nwnets) {
        ui_text(f, T(ws->state == WIFI_OFF ? "wifi.starting" : wfetching ? "wifi.searching" : "wifi.none"), x + w / 2, top + area / 2 - fh / 2, t->text_dim, ALIGN_CENTER);
    } else {
        float target = wscroll;
        if (wsel * rh - target > area - rh) target = wsel * rh - (area - rh);
        if (wsel * rh < target) target = wsel * rh;
        wscroll = ui_approach(wscroll, target);
        ui_clip_push(x, top, w, area);
        for (int i = 0; i < nwnets; i++) {
            int ry = top + (int)(i * rh - wscroll);
            if (ry + rh < top || ry > top + area) continue;
            wifi_net_t *n = &wnets[i];
            bool sel = i == wsel;
            if (sel) {
                ui_rrect(lx - M(0.015f), ry, rx - lx + M(0.03f), rh - M(0.008f), M(t->panel_radius), t->accent);
                selection_ring(lx - M(0.015f), ry, rx - lx + M(0.03f), rh - M(0.008f), M(t->panel_radius));
            }
            color_t tc = sel ? t->accent_text : t->text, dc = sel ? with_alpha(t->accent_text, 200) : t->text_dim;
            int ty = ry + (rh - M(0.008f) - fh) / 2, ih = M(0.03f), iy = ry + (rh - M(0.008f) - ih) / 2;
            ui_wifi_icon(lx + M(0.01f), iy, ih, n->bars ? n->bars : 1, tc, with_alpha(tc, 60));
            int nx = lx + M(0.01f) + ih + M(0.03f);
            const char *tag = n->current ? T("wifi.current") : n->failed ? T("wifi.failed") : n->saved ? T("wifi.saved") : "";
            int tagw = tag[0] ? ui_text_w(fs, tag) + M(0.02f) : 0;
            int lockw = n->secured ? ih + M(0.02f) : 0;
            char fit[64];
            ui_text_fit(f, n->ssid, rx - nx - tagw - lockw - M(0.03f), fit, sizeof fit);
            int tw = ui_text(f, fit, nx, ty, tc, ALIGN_LEFT);
            if (n->secured) lock_icon(nx + tw + M(0.015f), iy + ih / 8, ih * 3 / 4, dc);
            if (tag[0]) ui_text(fs, tag, rx - M(0.01f), ry + (rh - M(0.008f) - sh) / 2, n->failed && !sel ? rgba(220, 70, 70, 255) : dc, ALIGN_RIGHT);
        }
        ui_clip_pop();
    }
    bool can_forget = nwnets && wnets[wsel].saved;
    const char *hint[] = { "B", T("common.back"), "Y", T("wifi.refresh"), "X", T("wifi.forget"), "A", T("wifi.connect") };
    if (can_forget) ui_hints(hint, 8);
    else { const char *h2[] = { "B", T("common.back"), "Y", T("wifi.refresh"), "A", T("wifi.connect") }; ui_hints(h2, 6); }
}

/* Data e ora: giorno, mese, anno, ore, minuti. Su/giù cambiano il campo scelto con
   sinistra/destra; A salva (anche nell'orologio della console), B annulla. */
static bool clock_open;
static int clock_field;
static struct tm clock_tm;

static int days_in_month(int year, int mon) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return mon == 1 && leap ? 29 : d[mon];
}

static void clock_start(void) {
    time_t now = time(NULL);
    localtime_r(&now, &clock_tm);
    /* senza batteria tampone l'orologio può ripartire dal 1970: si parte da un anno sensato */
    if (clock_tm.tm_year < 2024 - 1900) { clock_tm.tm_year = 2026 - 1900; clock_tm.tm_mon = 0; clock_tm.tm_mday = 1; }
    clock_field = 0;
    clock_open = true;
}

static int wrap(int v, int lo, int hi) { return v < lo ? hi : v > hi ? lo : v; }

static void clock_change(int dir) {
    struct tm *c = &clock_tm;
    switch (clock_field) {
    case 0: c->tm_mday = wrap(c->tm_mday + dir, 1, days_in_month(c->tm_year + 1900, c->tm_mon)); break;
    case 1: c->tm_mon = wrap(c->tm_mon + dir, 0, 11); break;
    case 2: c->tm_year = wrap(c->tm_year + dir, 2024 - 1900, 2099 - 1900); break;
    case 3: c->tm_hour = wrap(c->tm_hour + dir, 0, 23); break;
    case 4: c->tm_min = wrap(c->tm_min + dir, 0, 59); break;
    }
    int dim = days_in_month(c->tm_year + 1900, c->tm_mon);
    if (c->tm_mday > dim) c->tm_mday = dim;
}

static void clock_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_LEFT]) { clock_field = (clock_field + 4) % 5; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_RIGHT]) { clock_field = (clock_field + 1) % 5; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_UP]) { clock_change(+1); ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { clock_change(-1); ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { clock_open = false; ui_sound(SND_BACK); }
    if (in->pressed[BTN_A]) {
        struct tm c = clock_tm;
        c.tm_sec = 0;
        c.tm_isdst = -1;
        bool ok = plat_set_time(mktime(&c));
        ui_toast(T(ok ? "clock.saved" : "clock.error"));
        ui_sound(ok ? SND_OK : SND_ERROR);
        clock_open = false;
    }
}

static void clock_draw(void) {
    theme_t *t = &UI.th;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    int w = M(0.9f), h = M(0.36f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    ui_text(UI.f_title, T("clock.title"), x + w / 2, y + M(0.05f), t->text, ALIGN_CENTER);
    char v[5][8];
    snprintf(v[0], 8, "%02d", clock_tm.tm_mday);
    snprintf(v[1], 8, "%02d", clock_tm.tm_mon + 1);
    snprintf(v[2], 8, "%04d", clock_tm.tm_year + 1900);
    snprintf(v[3], 8, "%02d", clock_tm.tm_hour);
    snprintf(v[4], 8, "%02d", clock_tm.tm_min);
    /* campi in riga: gg / mm / aaaa   hh : mm, ognuno in una casella */
    TTF_Font *f = UI.f_title;
    int fh = TTF_FontHeight(f), pad = M(0.02f), cw2 = ui_text_w(f, "00") + 2 * pad, cw4 = ui_text_w(f, "0000") + 2 * pad;
    int sepw = ui_text_w(f, "/") + 2 * pad, gap = M(0.05f);
    int total = 4 * cw2 + cw4 + 3 * sepw + gap;
    int cx = x + (w - total) / 2, cy = y + h / 2 - fh / 2 + M(0.035f);
    const char *sep[5] = { "/", "/", "", ":", "" };
    for (int i = 0; i < 5; i++) {
        int bw = i == 2 ? cw4 : cw2;
        bool sel = i == clock_field;
        ui_rrect(cx, cy - pad / 2, bw, fh + pad, M(t->panel_radius), sel ? t->accent : with_alpha(t->text_dim, 40));
        ui_text(f, v[i], cx + bw / 2, cy, sel ? t->accent_text : t->text, ALIGN_CENTER);
        if (sel) {
            int a = M(0.015f), mx2 = cx + bw / 2, ty = cy - pad / 2 - M(0.015f), by = cy + fh + pad / 2 + M(0.015f);
            ui_triangle(mx2 - a, ty, mx2 + a, ty, mx2, ty - a, t->accent);
            ui_triangle(mx2 - a, by, mx2 + a, by, mx2, by + a, t->accent);
        }
        cx += bw;
        if (*sep[i]) { ui_text(f, sep[i], cx + sepw / 2, cy, t->text_dim, ALIGN_CENTER); cx += sepw; }
        else if (i == 2) cx += gap;
    }
    const char *h2[] = { "B", T("common.cancel"), "<>", T("clock.field"), "^v", T("common.change"), "A", T("common.ok") };
    ui_hints(h2, 8);
}

/* Informazioni: spazio della scheda SD e cosa lo occupa. Si calcola all'apertura. */
static bool sdinfo_open;
typedef struct { char label[64]; int count; long long bytes; } sdrow_t;
static sdrow_t sdrows[24];
static int nsdrows;
static long long sd_total, sd_free;

static long long dir_bytes(const char *path, int depth) {
    DIR *d = opendir(path);
    if (!d) return 0;
    long long sum = 0;
    struct dirent *e;
    char p[1200];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 8) sum += dir_bytes(p, depth + 1); }
        else if (S_ISREG(st.st_mode)) sum += st.st_size;
    }
    closedir(d);
    return sum;
}

static void fmt_bytes(long long b, char *out, size_t n) {
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    snprintf(out, n, i == 0 ? "%.0f %s" : v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
    /* virgola decimale in tutte le lingue tranne l'inglese */
    if (strcmp(UI.set.lang, "en")) for (char *c = out; *c; c++) if (*c == '.') *c = ',';
}

static void sdrow_add(const char *label, int count, long long bytes) {
    if (nsdrows >= (int)(sizeof sdrows / sizeof *sdrows)) return;
    sdrow_t *r = &sdrows[nsdrows++];
    snprintf(r->label, sizeof r->label, "%s", label);
    r->count = count;
    r->bytes = bytes;
}

static void sdinfo_start(void) {
    quiet_begin();   /* il conteggio dello spazio legge tutta la scheda */
    struct statvfs vs;
    sd_total = sd_free = -1;
    if (statvfs(UI.root, &vs) == 0) {
        sd_total = (long long)vs.f_blocks * vs.f_frsize;
        sd_free = (long long)vs.f_bavail * vs.f_frsize;
    }
    nsdrows = 0;
    /* giochi per emulatore: numero e misura dei file ROM (i giochi Linux: la loro cartella) */
    const char *cores[18];
    int ncores = 0;
    for (int i = 0; i < NEMUS && ncores < 17; i++) cores[ncores++] = EMUS[i].id;
    cores[ncores++] = "linux";
    for (int c = 0; c < ncores; c++) {
        int cnt = 0;
        long long b = 0;
        for (int i = 0; i < ngames; i++) {
            game_t *g = &games[i];
            if (g->missing || !g->path || !*g->path || strcmp(g->core, cores[c])) continue;
            cnt++;
            if (!strcmp(cores[c], "linux")) {
                char dir[1024];
                snprintf(dir, sizeof dir, "%s", g->path);
                char *sl = strrchr(dir, '/');
                if (sl) { *sl = 0; b += dir_bytes(dir, 0); }
            } else {
                struct stat st;
                if (stat(g->path, &st) == 0) b += st.st_size;
            }
        }
        if (cnt) sdrow_add(games_core_label(cores[c]), cnt, b);
    }
    static const char *dirs[][2] = { { "Bios", "sdinfo.bios" }, { "Media", "sdinfo.media" },
                                     { "Saves", "sdinfo.saves" }, { "Music", "sdinfo.music" },
                                     { "System", "sdinfo.system" } };
    for (int i = 0; i < 5; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", UI.root, dirs[i][0]);
        sdrow_add(T(dirs[i][1]), -1, dir_bytes(p, 0));
    }
    sdinfo_open = true;
}

static void sdinfo_input(void) {
    if (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B]) { sdinfo_open = false; ui_sound(SND_BACK); }
}

static void sdinfo_draw(void) {
    theme_t *t = &UI.th;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    TTF_Font *f = UI.f_normal, *fs = UI.f_small;
    int fh = TTF_FontHeight(f), sh = TTF_FontHeight(fs), rh = fh + M(0.008f);
    int w = M(0.9f), pad = M(0.04f);
    int h = pad + TTF_FontHeight(UI.f_title) + M(0.02f) + M(0.03f) + M(0.015f) + sh + M(0.03f) + nsdrows * rh + M(0.02f) + sh + pad;
    if (h > UI.h - M(t->hintbar_h) - M(0.02f)) h = UI.h - M(t->hintbar_h) - M(0.02f);
    int x = (UI.w - w) / 2, y = (UI.h - M(t->hintbar_h) - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    int cy = y + pad, lx = x + pad, rx = x + w - pad, bw = w - 2 * pad;
    ui_text(UI.f_title, T("sdinfo.title"), lx, cy, t->text, ALIGN_LEFT);
    cy += TTF_FontHeight(UI.f_title) + M(0.02f);
    char a[32], b[32], line[160];
    /* barra dello spazio usato */
    int bh = M(0.03f);
    ui_rrect(lx, cy, bw, bh, bh / 2, with_alpha(t->text_dim, 60));
    if (sd_total > 0) {
        long long used = sd_total - sd_free;
        int uw = (int)((double)bw * used / sd_total);
        if (uw < bh) uw = bh;
        ui_rrect(lx, cy, uw, bh, bh / 2, sd_free * 10 < sd_total ? rgba(220, 70, 70, 255) : t->accent);
        cy += bh + M(0.015f);
        fmt_bytes(sd_free, a, sizeof a);
        fmt_bytes(sd_total, b, sizeof b);
        snprintf(line, sizeof line, T("sdinfo.free"), a, b);
        ui_text(fs, line, lx, cy, t->text_dim, ALIGN_LEFT);
        fmt_bytes(used, a, sizeof a);
        snprintf(line, sizeof line, T("sdinfo.used"), a);
        ui_text(fs, line, rx, cy, t->text_dim, ALIGN_RIGHT);
    } else {
        cy += bh + M(0.015f);
        ui_text(fs, T("sdinfo.unknown"), lx, cy, t->text_dim, ALIGN_LEFT);
    }
    cy += sh + M(0.03f);
    int bottom = y + h - pad - sh - M(0.02f);
    for (int i = 0; i < nsdrows && cy + rh <= bottom; i++) {
        sdrow_t *rw = &sdrows[i];
        ui_text(f, rw->label, lx, cy, t->text, ALIGN_LEFT);
        fmt_bytes(rw->bytes, a, sizeof a);
        if (rw->count >= 0) {
            if (rw->count == 1) snprintf(b, sizeof b, "%s", T("sdinfo.one_game"));
            else snprintf(b, sizeof b, T("info.games"), rw->count);
            snprintf(line, sizeof line, "%s · %s", b, a);
        } else snprintf(line, sizeof line, "%s", a);
        ui_text(f, line, rx, cy, t->text_dim, ALIGN_RIGHT);
        cy += rh;
    }
    const char *model = plat_model();
    snprintf(line, sizeof line, T("sdinfo.footer"), update_current(),
             model ? (!strcmp(model, "brickpro") ? "TrimUI Brick Pro" : "TrimUI Smart Pro") : device_label(UI.set.device));
    ui_text(fs, line, lx, y + h - pad - sh, t->text_dim, ALIGN_LEFT);
    const char *hint[] = { "B", T("common.back") };
    ui_hints(hint, 2);
}

/* Pulisci immagini: le immagini in Media/<tipo>/ di giochi che non sono sulla scheda (ROM
   tolte) si contano, si mostrano con lo spazio occupato e, con conferma, si eliminano. */
static bool clean_open;
static int clean_n, clean_sel;
static long long clean_bytes;

static int cmp_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int media_orphans(bool del, long long *bytes) {
    const char **names = malloc(sizeof *names * (size_t)(ngames ? ngames : 1));
    int nn = 0, count = 0;
    for (int i = 0; i < ngames; i++)
        if (!games[i].missing) names[nn++] = games[i].name;
    qsort(names, (size_t)nn, sizeof *names, cmp_name);
    *bytes = 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Media", UI.root);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1400];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR *d2 = opendir(sub);
        struct dirent *f;
        while (d2 && (f = readdir(d2))) {
            if (f->d_name[0] == '.') continue;
            char stem[256];
            snprintf(stem, sizeof stem, "%s", f->d_name);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            const char *key = stem;
            if (bsearch(&key, names, (size_t)nn, sizeof *names, cmp_name)) continue;
            char path[1700];
            snprintf(path, sizeof path, "%s/%s", sub, f->d_name);
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            count++;
            *bytes += st.st_size;
            if (del) unlink(path);
        }
        if (d2) closedir(d2);
    }
    if (d) closedir(d);
    free(names);
    return count;
}

static void clean_start(void) {
    quiet_begin();
    clean_n = media_orphans(false, &clean_bytes);
    if (!clean_n) { ui_toast(T("clean.none")); ui_sound(SND_MOVE); return; }
    clean_sel = 0;
    clean_open = true;
    ui_sound(SND_OK);
}

static void clean_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_LEFT] || in->pressed[BTN_RIGHT]) { clean_sel = !clean_sel; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { clean_open = false; ui_sound(SND_BACK); return; }
    if (in->pressed[BTN_A]) {
        clean_open = false;
        if (clean_sel == 1) {
            long long b;
            int n = media_orphans(true, &b);
            char size[32], m[160];
            fmt_bytes(b, size, sizeof size);
            snprintf(m, sizeof m, T("clean.done"), n, size);
            ui_log("pulizia immagini: %d file, %lld byte", n, b);
            ui_image_forget_missing();
            ui_toast(m);
            ui_sound(SND_OK);
        } else ui_sound(SND_BACK);
    }
}

static void clean_draw(void) {
    theme_t *t = &UI.th;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    int w = M(0.85f), h = M(0.40f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    ui_text(UI.f_title, T("clean.title"), x + w / 2, y + M(0.05f), t->text, ALIGN_CENTER);
    char size[32], body[200], lines[2][160];
    fmt_bytes(clean_bytes, size, sizeof size);
    snprintf(body, sizeof body, T("clean.body"), clean_n, size);
    int n = ui_wrap(UI.f_small, body, w - M(0.08f), lines, 2);
    int ty = y + M(0.05f) + TTF_FontHeight(UI.f_title) + M(0.015f);
    for (int i = 0; i < n; i++) ui_text(UI.f_small, lines[i], x + w / 2, ty + i * TTF_FontHeight(UI.f_small), t->text_dim, ALIGN_CENTER);
    int bw = (w - M(0.09f)) / 2, bh = M(0.085f), by = y + h - bh - M(0.04f);
    const char *labels[2] = { T("common.cancel"), T("clean.delete") };
    for (int i = 0; i < 2; i++) {
        int bx = x + M(0.03f) + i * (bw + M(0.03f));
        bool sel = clean_sel == i;
        ui_rrect(bx, by, bw, bh, bh / 2, sel ? (i == 1 ? rgba(220, 70, 70, 255) : t->accent) : with_alpha(t->text_dim, 40));
        ui_text(UI.f_normal, labels[i], bx + bw / 2, by + (bh - TTF_FontHeight(UI.f_normal)) / 2, sel ? rgba(255, 255, 255, 255) : t->text, ALIGN_CENTER);
    }
    const char *hint[] = { "B", T("common.cancel"), "<>", T("common.change"), "A", T("common.ok") };
    ui_hints(hint, 6);
}

/* Aggiorna sistema: cerca, chiede conferma, scarica, verifica, installa e riavvia */
static bool upd_open;
static bool ptest_open;      /* prova dei comandi */
static double ptest_hold;

static bool upd_busy(void) {
    int p = update_status()->phase;
    return upd_open && (p == UPD_SEARCH || p == UPD_CHECK || p == UPD_DOWNLOAD || p == UPD_VERIFY || p == UPD_INSTALL);
}

/* Wi-Fi e SD al massimo fanno ronzare l'altoparlante: aggiornamento, verifica ROM, immagini, pulizia, reti: si spegne e si
   riaccende se l'operazione si interrompe. A installazione finita resta spento: segue il riavvio. */
static void upd_audio(void) {
    int p = update_status()->phase;
    bool want = (upd_open && (p == UPD_SEARCH || p == UPD_CHECK || p == UPD_DOWNLOAD || p == UPD_VERIFY || p == UPD_INSTALL || p == UPD_DONE))
                || romcheck_open || scrape_open || clean_open || sdinfo_open || wpanel_open;
    if (want == audio_quiet) return;
    if (want) { quiet_begin(); return; }
    audio_quiet = false;
    ui_log("altoparlante riacceso");
    plat_audio_unmute();
}

static void upd_start(void) {
    quiet_begin();
    if (wifi_status(true)->state != WIFI_CONNECTED) { ui_toast(T("update.no_wifi")); ui_sound(SND_ERROR); return; }
    update_set_channel(UI.set.update_channel);
    update_check(UI.root);
    upd_open = true;
    ui_sound(SND_OK);
}

static void upd_reboot(void) {
    settings_save(&UI.set, UI.root);
    ui_busy(T("update.restarting"), 0.5);
    music_quit();
    ui_shutdown_code(EXIT_REBOOT);
    exit(EXIT_REBOOT);
}

static void upd_input(void) {
    input_t *in = &UI.in;
    const update_status_t *s = update_status();
    last_input = UI.t;   /* niente salvaschermo o spegnimento automatico durante l'aggiornamento */
    if (upd_busy()) {
        if (in->pressed[BTN_B] && s->phase != UPD_INSTALL) { update_cancel(); ui_sound(SND_BACK); }
        return;
    }
    if (s->phase == UPD_AVAILABLE && in->pressed[BTN_A]) { update_install(); ui_sound(SND_OK); return; }
    if (s->phase == UPD_READY) {
        if (in->pressed[BTN_A]) { update_apply(); ui_sound(SND_OK); }
        else if (in->pressed[BTN_B]) { update_discard(); ui_sound(SND_BACK); }
        return;
    }
    if (s->phase == UPD_DONE) { if (in->pressed[BTN_A]) { update_join(); upd_reboot(); } return; }
    if (in->pressed[BTN_A] || in->pressed[BTN_B]) { update_join(); upd_open = false; ui_sound(SND_BACK); }
}

static void upd_draw(void) {
    theme_t *t = &UI.th;
    const update_status_t *s = update_status();
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    int w = M(0.95f), h = M(0.42f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    if (t->surface.a < 200) ui_rrect(x, y, w, h, r, with_alpha(t->bg2, 230));
    int pad = M(0.05f), ty = y + pad, bw = w - 2 * pad;
    char upd_title[120];
    snprintf(upd_title, sizeof upd_title, "%s%s%s", T("update.title"), UI.set.update_channel ? " · " : "", UI.set.update_channel ? T("update.channel_dev") : "");
    ui_text(UI.f_title, upd_title, x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.03f);
    char line[200], key[32], size[32], sub[200] = "";
    switch (s->phase) {
    case UPD_SEARCH: snprintf(line, sizeof line, "%s", T("update.searching")); break;
    case UPD_CHECK: snprintf(line, sizeof line, "%s", T("update.checking")); break;
    case UPD_NO_SERVER: snprintf(line, sizeof line, "%s", T("update.none")); snprintf(sub, sizeof sub, "%s", T("update.none_hint")); break;
    case UPD_CURRENT: snprintf(line, sizeof line, "%s", T("update.current")); snprintf(sub, sizeof sub, "%s", update_current()); break;
    case UPD_AVAILABLE:
        fmt_bytes(s->total, size, sizeof size);
        snprintf(line, sizeof line, T("update.available"), s->version);
        snprintf(sub, sizeof sub, T("update.details"), update_current(), s->files, size);
        break;
    case UPD_DOWNLOAD: case UPD_VERIFY: snprintf(line, sizeof line, "%s", T("update.downloading")); break;
    case UPD_READY:
        snprintf(line, sizeof line, "%s", T("update.ready"));
        snprintf(sub, sizeof sub, T("update.ready_hint"), s->version);
        break;
    case UPD_INSTALL: snprintf(line, sizeof line, "%s", T("update.installing")); break;
    case UPD_DONE: snprintf(line, sizeof line, "%s", T("update.done")); snprintf(sub, sizeof sub, "%s", s->version); break;
    default:
        snprintf(key, sizeof key, "update.err%d", s->error);
        snprintf(line, sizeof line, "%s", T(key));
        snprintf(sub, sizeof sub, "%s", T("update.err_safe"));
    }
    ui_text(UI.f_normal, line, x + pad, ty, s->phase == UPD_FAILED ? rgba(220, 70, 70, 255) : t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_normal) + M(0.03f);
    if (s->phase == UPD_CHECK || s->phase == UPD_DOWNLOAD || s->phase == UPD_VERIFY || s->phase == UPD_INSTALL) {
        int bh = M(0.02f);
        float frac = s->total ? (float)s->done / (float)s->total : 0.f;
        if (s->phase == UPD_INSTALL) frac = 1.f;
        if (frac > 1) frac = 1;
        ui_rrect(x + pad, ty, bw, bh, bh / 2, with_alpha(t->text_dim, 70));
        if (frac > 0) ui_rrect(x + pad, ty, (int)(bw * frac), bh, bh / 2, t->accent);
        ty += bh + M(0.03f);
        if (s->total) {
            char a[32], b[32];
            fmt_bytes(s->done, a, sizeof a);
            fmt_bytes(s->total, b, sizeof b);
            snprintf(sub, sizeof sub, "%s / %s", a, b);
        }
    }
    if (sub[0]) {
        char lines[2][160];
        int n = ui_wrap(UI.f_small, sub, bw, lines, 2);
        for (int i = 0; i < n; i++) ui_text(UI.f_small, lines[i], x + pad, ty + i * TTF_FontHeight(UI.f_small), t->text_dim, ALIGN_LEFT);
    }
    int hb = M(t->hintbar_h);
    ui_rect(0, UI.h - hb, UI.w, hb, t->bg);
    if (s->phase == UPD_AVAILABLE) { const char *h2[] = { "B", T("common.cancel"), "A", T("update.download") }; ui_hints(h2, 4); }
    else if (s->phase == UPD_READY) { const char *h2[] = { "B", T("common.cancel"), "A", T("update.install") }; ui_hints(h2, 4); }
    else if (s->phase == UPD_DONE) { const char *h2[] = { "A", T("update.restart") }; ui_hints(h2, 2); }
    else if (s->phase == UPD_INSTALL) {}
    else if (upd_busy()) { const char *h2[] = { "B", T("common.cancel") }; ui_hints(h2, 2); }
    else { const char *h2[] = { "A", T("common.ok") }; ui_hints(h2, 2); }
}

/* Generi: scelta per il singolo gioco (opzioni della scheda) e gestione (Impostazioni > Giochi):
   rinominare, accorpare, tornare agli originali. Un'unica finestra a elenco, con quattro usi. */
enum { GP_GAME, GP_MANAGE, GP_ACTION, GP_MERGE };
static bool gp_open;
static int gp_mode, gp_sel;
static char gp_genre[48];     /* genere su cui si sta lavorando */

static void gp_show(int mode) { gp_mode = mode; gp_sel = 0; gp_open = true; ui_sound(SND_OK); }

/* i generi tra cui scegliere nell'accorpamento: tutti tranne quello di partenza */
static int gp_merge_index(int row) {
    int r = 0;
    for (int k = 0; k < ngenres; k++) {
        if (!strcmp(genres[k], gp_genre)) continue;
        if (r++ == row) return k;
    }
    return -1;
}

static int gp_rows(void) {
    switch (gp_mode) {
    case GP_GAME: return ngenres + 2;       /* Automatico, generi, Nuovo */
    case GP_MANAGE: return ngenres + 1;     /* generi, Ripristina */
    case GP_ACTION: return 2;               /* Rinomina, Accorpa */
    default: return ngenres > 1 ? ngenres - 1 : 0;
    }
}

/* dopo una modifica: si rileggono i giochi e si resta sul gioco aperto */
static void genres_changed(void) {
    char core[16] = "", name[64] = "";
    if (detail_idx >= 0 && detail_idx < ngames) {
        snprintf(core, sizeof core, "%s", games[detail_idx].core);
        snprintf(name, sizeof name, "%s", games[detail_idx].name);
    }
    reload_games();
    if (name[0])
        for (int i = 0; i < ngames; i++)
            if (!strcmp(games[i].name, name) && !strcmp(games[i].core, core)) { detail_idx = i; break; }
}

static void gp_set_game(const char *genre) {
    char rom[64], l[48], m[120];
    snprintf(rom, sizeof rom, "%s", games[detail_idx].name);
    games_set_genre(UI.root, rom, genre);
    genres_changed();
    gp_open = false;
    genre_label(games[detail_idx].genre, l, sizeof l);
    snprintf(m, sizeof m, "%s: %s", T("genre_ui.row"), l[0] ? l : "—");
    ui_toast(m);
    ui_sound(SND_OK);
}

static void kb_for_genre(int mode, const char *start) {
    snprintf(genre_text, sizeof genre_text, "%s", start);
    kb_mode = mode;
    kb_layout = 0;
    kb_row = 1;
    kb_col = 0;
    kb_open = true;
    ui_sound(SND_OK);
}

static void gp_input(void) {
    input_t *in = &UI.in;
    int n = gp_rows();
    if (n && in->pressed[BTN_UP]) { gp_sel = (gp_sel + n - 1) % n; ui_sound(SND_MOVE); }
    if (n && in->pressed[BTN_DOWN]) { gp_sel = (gp_sel + 1) % n; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) {
        if (gp_mode == GP_ACTION) { gp_mode = GP_MANAGE; gp_sel = 0; }
        else if (gp_mode == GP_MERGE) { gp_mode = GP_ACTION; gp_sel = 1; }
        else gp_open = false;
        ui_sound(SND_BACK);
        return;
    }
    if (!in->pressed[BTN_A] || !n) return;
    char m[160], a[48], b[48];
    switch (gp_mode) {
    case GP_GAME:
        if (gp_sel == 0) gp_set_game(NULL);
        else if (gp_sel <= ngenres) { char g[48]; snprintf(g, sizeof g, "%s", genres[gp_sel - 1]); gp_set_game(g); }
        else kb_for_genre(KBM_GENRE_NEW, "");
        break;
    case GP_MANAGE:
        if (gp_sel < ngenres) { snprintf(gp_genre, sizeof gp_genre, "%s", genres[gp_sel]); gp_mode = GP_ACTION; gp_sel = 0; ui_sound(SND_OK); }
        else {
            cfm_kind = CFM_GENRES;
            cfm_sel = 0;
            ui_sound(SND_OK);
        }
        break;
    case GP_ACTION:
        if (gp_sel == 0) kb_for_genre(KBM_GENRE_RENAME, genre_label(gp_genre, a, sizeof a));
        else if (ngenres > 1) { gp_mode = GP_MERGE; gp_sel = 0; ui_sound(SND_OK); }
        else { ui_toast(T("genre_ui.no_other")); ui_sound(SND_ERROR); }
        break;
    default: {
        int k = gp_merge_index(gp_sel);
        if (k < 0) break;
        genre_label(gp_genre, a, sizeof a);
        genre_label(genres[k], b, sizeof b);
        char to[48];
        snprintf(to, sizeof to, "%s", genres[k]);
        games_group_genre(UI.root, gp_genre, to);
        genres_changed();
        snprintf(m, sizeof m, T("genre_ui.merged"), a, b);
        ui_toast(m);
        ui_sound(SND_OK);
        gp_mode = GP_MANAGE;
        gp_sel = 0;
    }
    }
}

/* fine scrittura del nome di un genere */
static void genre_kb_done(void) {
    for (char *c = genre_text; *c; c++) if (*c == '/' || *c == '\t') *c = '-';
    size_t e = strlen(genre_text);
    while (e > 0 && genre_text[e - 1] == ' ') genre_text[--e] = 0;
    if (!genre_text[0]) return;
    if (kb_mode == KBM_GENRE_NEW) { gp_set_game(genre_text); return; }
    char m[160], a[48];
    genre_label(gp_genre, a, sizeof a);
    games_group_genre(UI.root, gp_genre, genre_text);
    genres_changed();
    snprintf(m, sizeof m, T("genre_ui.renamed"), a, genre_text);
    ui_toast(m);
    gp_mode = GP_MANAGE;
    gp_sel = 0;
}

static void gp_draw(void) {
    theme_t *t = &UI.th;
    int rows = gp_rows(), rh = M(t->list_row_h), pad = M(0.04f);
    int maxrows = (UI.h - M(t->hintbar_h) - M(0.2f) - TTF_FontHeight(UI.f_title)) / rh;
    int vis = rows < maxrows ? rows : maxrows;
    if (vis < 1) vis = 1;
    int w = M(0.95f) < UI.w - 2 * M(t->margin) ? M(0.95f) : UI.w - 2 * M(t->margin);
    int h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.02f) + rh * vis, x, y;
    panel_box(w, h, &x, &y);
    char title[120], a[48];
    if (gp_mode == GP_GAME) snprintf(title, sizeof title, "%s", T("genre_ui.title_game"));
    else if (gp_mode == GP_MANAGE) snprintf(title, sizeof title, "%s", T("settings.genres"));
    else if (gp_mode == GP_ACTION) snprintf(title, sizeof title, "%s", genre_label(gp_genre, a, sizeof a));
    else snprintf(title, sizeof title, T("genre_ui.merge_title"), genre_label(gp_genre, a, sizeof a));
    char fit[120];
    ui_text_fit(UI.f_title, title, w - 2 * pad, fit, sizeof fit);
    ui_text(UI.f_title, fit, x + pad, y + pad, t->text, ALIGN_LEFT);
    int ry = y + pad + TTF_FontHeight(UI.f_title) + M(0.02f);
    int first = gp_sel - vis + 1 > 0 ? gp_sel - vis + 1 : 0;
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    for (int i = first; i < first + vis && i < rows; i++, ry += rh) {
        bool sel = i == gp_sel;
        if (sel) ui_rrect(x + M(0.02f), ry, w - M(0.04f), rh - M(0.008f), M(t->panel_radius), t->accent);
        color_t c = sel ? t->accent_text : t->text;
        int ty = ry + (rh - M(0.008f) - fh) / 2, lx = x + M(0.05f);
        char label[80] = "", val[32] = "";
        int gi = -1;
        switch (gp_mode) {
        case GP_GAME:
            if (i == 0) snprintf(label, sizeof label, "%s", T("genre_ui.auto"));
            else if (i <= ngenres) gi = i - 1;
            else snprintf(label, sizeof label, "%s", T("genre_ui.new"));
            break;
        case GP_MANAGE:
            if (i < ngenres) {
                gi = i;
                int cnt = 0;
                for (int k = 0; k < ngames; k++) {
                    char m[48];
                    if (games[k].missing) continue;
                    genre_main(games[k].genre, m, sizeof m);
                    cnt += !strcmp(m, genres[i]);
                }
                snprintf(val, sizeof val, "%d", cnt);
            } else snprintf(label, sizeof label, "%s", T("genre_ui.restore"));
            break;
        case GP_ACTION: snprintf(label, sizeof label, "%s", T(i ? "genre_ui.merge" : "genre_ui.rename")); break;
        default: gi = gp_merge_index(i);
        }
        if (gi >= 0) genre_label(genres[gi], label, sizeof label);
        ui_text(f, label, lx, ty, c, ALIGN_LEFT);
        if (val[0]) ui_text(UI.f_small, val, x + w - M(0.05f), ty + (fh - TTF_FontHeight(UI.f_small)) / 2, sel ? c : t->text_dim, ALIGN_RIGHT);
    }
    const char *hint[] = { "B", T("common.back"), "A", T("common.ok") };
    ui_hints(hint, 4);
}

static void settings_input(void) {
    input_t *in = &UI.in;
    int rows[16], n = page_rows(rows);
    if (in->pressed[BTN_UP]) { set_row = (set_row + n - 1) % n; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { set_row = (set_row + 1) % n; ui_sound(SND_MOVE); }
    if (set_row >= n) set_row = n - 1;
    set_sel = rows[set_row];
    if (in->pressed[BTN_B] || in->pressed[BTN_START]) {
        if (set_page >= 0 && in->pressed[BTN_B]) { settings_open(-1, set_page); ui_sound(SND_BACK); return; }
        go(is_list_layout() ? SCR_LIBRARY : (back_screen == SCR_SETTINGS ? SCR_HOME : back_screen));
        ui_sound(SND_BACK);
        return;
    }
    if (set_sel >= ROW_CAT) {   /* categoria: A o destra la aprono */
        if (in->pressed[BTN_A] || in->pressed[BTN_RIGHT]) { settings_open(set_sel - ROW_CAT, 0); ui_sound(SND_OK); }
        return;
    }
    if (set_disabled(set_sel)) {
        if (in->pressed[BTN_A] || in->pressed[BTN_LEFT] || in->pressed[BTN_RIGHT]) {
            if (set_sel == S_SCRAPE || set_sel == S_UPDATE) ui_toast(T("scrape.no_wifi"));
            ui_sound(SND_ERROR);
        }
        return;
    }
    if (in->pressed[BTN_LEFT]) set_change(set_sel, -1);
    if (in->pressed[BTN_RIGHT]) set_change(set_sel, +1);
    if (in->pressed[BTN_A]) {
        if (set_sel == S_POWER) { dlg_open = true; dlg_suspend = false; dlg_sel = 0; ui_sound(SND_OK); }
        else if (set_sel == S_CLOCK) { clock_start(); ui_sound(SND_OK); }
        else if (set_sel == S_CLEANMEDIA) clean_start();
        else if (set_sel == S_UPDATE) upd_start();
        else if (set_sel == S_GENRES) gp_show(GP_MANAGE);
        else if (set_sel == S_PADTEST) { ptest_open = true; ptest_hold = 0; ui_sound(SND_OK); }
        else if (set_sel == S_WIFI) { set_change(S_WIFI, +1); ui_sound(SND_OK); }
        else if (set_sel == S_WIFINETS) {
            if (UI.set.wifi) { wpanel_start(); ui_sound(SND_OK); }
            else ui_sound(SND_ERROR);
        }
        else if (set_sel == S_ROMCHECK) {
            quiet_begin();
            if (romcheck_start(UI.root, games, ngames)) { romcheck_open = true; ui_sound(SND_OK); }
        }
        else if (set_sel == S_SCRAPE) {
            if (!UI.set.wifi) { ui_toast(T("scrape.no_wifi")); ui_sound(SND_ERROR); }
            else if ((quiet_begin(), scraper_start(UI.root, games, ngames))) { scrape_open = true; ui_sound(SND_OK); }
        }
        else if (set_sel == S_MUSIC && UI.set.music_vol && music_count()) {
            music_next();
            ui_sound(SND_OK);
        }
        else if (set_sel == S_INFO) { sdinfo_start(); ui_sound(SND_OK); }
        else set_change(set_sel, +1);
    }
}

static void draw_switch(int x, int y, int h, bool on) {
    theme_t *t = &UI.th;
    int w = h * 9 / 5;
    ui_rrect(x, y, w, h, h / 2, on ? t->accent : with_alpha(t->text_dim, 110));
    ui_circle(on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - h / 8, rgba(255, 255, 255, 255));
}

static void settings_draw(void) {
    theme_t *t = &UI.th;
    char ttl[128];
    if (set_page < 0) snprintf(ttl, sizeof ttl, "%s", T("settings.title"));
    else { char k[64]; snprintf(k, sizeof k, "settings_cat.%s", cat_key[set_page]); snprintf(ttl, sizeof ttl, "%s", T(k)); }
    ui_topbar(ttl);
    int rows[16], nrows = page_rows(rows);
    int mx = M(t->margin), top = M(t->topbar_h) + M(0.02f), rh = M(t->list_row_h) + M(0.01f);
    int desc_h = TTF_FontHeight(UI.f_small) * 2 + M(0.03f);
    int area_h = UI.h - M(t->hintbar_h) - desc_h - top;
    area_h -= area_h % rh;   /* solo righe intere: niente voci tagliate a metà */
    float ry = (float)set_row * rh;
    float target = set_scroll;
    if (ry - target > area_h - rh) target = ry - (area_h - rh);
    if (ry < target) target = ry;
    set_scroll = ui_approach(set_scroll, target);
    set_hl = ui_approach(set_hl, ry);
    int w = UI.w - 2 * mx;
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);

    ui_clip_push(0, top - M(0.01f), UI.w, area_h + M(0.02f));
    int hy = top + (int)(set_hl - set_scroll);
    ui_shadow(mx, hy + M(0.004f), w, rh - M(0.01f), M(t->panel_radius), M(0.012f), t->shadow);
    ui_rrect(mx, hy, w, rh - M(0.01f), M(t->panel_radius), t->surface);
    selection_ring(mx, hy, w, rh - M(0.01f), M(t->panel_radius));
    for (int k = 0; k < nrows; k++) {
        int i = rows[k];
        int y = top + (int)(k * rh - set_scroll), cy = y + (rh - M(0.01f)) / 2;
        if (y + rh < top - rh || y > top + area_h) continue;
        bool sel = k == set_row;
        if (i >= ROW_CAT) {   /* categoria: nome, voci in breve e freccia */
            char key[64];
            snprintf(key, sizeof key, "settings_cat.%s", cat_key[i - ROW_CAT]);
            int lw = ui_text(f, T(key), mx + M(0.03f), cy - fh / 2, t->text, ALIGN_LEFT);
            snprintf(key, sizeof key, "settings_cat_sum.%s", cat_key[i - ROW_CAT]);
            char fit[160];
            int vx2 = mx + w - M(0.03f) - ui_text_w(f, "›") - M(0.02f);
            ui_text_fit(UI.f_small, T(key), vx2 - (mx + M(0.06f) + lw), fit, sizeof fit);
            ui_text(UI.f_small, fit, vx2, cy - TTF_FontHeight(UI.f_small) / 2, t->text_dim, ALIGN_RIGHT);
            ui_text(f, "›", mx + w - M(0.03f), cy - fh / 2, sel ? t->accent : t->text_dim, ALIGN_RIGHT);
            continue;
        }
        if (set_disabled(i)) {
            color_t grey = with_alpha(t->text_dim, 120);
            ui_text(f, set_text("settings", i), mx + M(0.03f), cy - fh / 2, grey, ALIGN_LEFT);
            if (i != S_SCRAPE) {
                char off[160];
                set_value(i, off, sizeof off);
                ui_text(f, off, mx + w - M(0.03f), cy - fh / 2, grey, ALIGN_RIGHT);
            }
            continue;
        }
        color_t lc = i == S_POWER ? (sel ? rgba(220, 70, 70, 255) : t->text) : t->text;
        ui_text(f, set_text("settings", i), mx + M(0.03f), cy - fh / 2, lc, ALIGN_LEFT);
        if (i == S_POWER || i == S_SCRAPE || i == S_ROMCHECK || i == S_CLEANMEDIA) continue;
        int vx = mx + w - M(0.03f);
        char val[160];
        set_value(i, val, sizeof val);
        if (i == S_BRIGHT || i == S_VOLUME || i == S_LEDBRIGHT) {
            int bw = M(0.32f), bh = M(0.012f), mn = i == S_VOLUME ? 0 : 1, mxv = i == S_VOLUME ? 20 : 10;
            int v = i == S_BRIGHT ? UI.set.brightness : i == S_VOLUME ? UI.set.volume : UI.set.led_bright;
            int nx = vx - ui_text_w(f, "20") - M(0.03f) - bw;
            ui_rrect(nx, cy - bh / 2, bw, bh, bh / 2, with_alpha(t->text_dim, 80));
            int fw = bw * (v - mn) / (mxv - mn);
            ui_rrect(nx, cy - bh / 2, fw, bh, bh / 2, t->accent);
            ui_circle(nx + fw, cy, bh * 3 / 2, sel ? t->accent : t->surface);
            ui_ring(nx + fw, cy, bh * 3 / 2, 2, t->accent);
            ui_text(f, val, vx, cy - fh / 2, t->text, ALIGN_RIGHT);
        } else if (i == S_WIFI || i == S_RESUME || i == S_BEZEL) {
            bool on = i == S_WIFI ? UI.set.wifi : i == S_BEZEL ? UI.set.bezel : UI.set.resume_last;
            int sh = M(0.04f);
            draw_switch(vx - sh * 9 / 5, cy - sh / 2, sh, on);
        } else {
            color_t vc = sel ? t->accent : t->text_dim;
            bool arrows = sel && i != S_INFO && i != S_CLOCK && i != S_UPDATE && i != S_GENRES && i != S_PADTEST && i != S_WIFINETS;
            int tw = ui_text(f, val, vx - (arrows ? M(0.035f) : 0), cy - fh / 2, vc, ALIGN_RIGHT);
            if (i == S_LEDCOLOR) {
                /* pallino del colore (per "tema": il colore principale del tema) */
                unsigned c = UI.set.led_color ? LED_COLORS[UI.set.led_color % NLED_COLORS].rgb
                                              : (unsigned)t->accent.r << 16 | t->accent.g << 8 | t->accent.b;
                int rr = fh / 3, px = vx - (arrows ? M(0.035f) : 0) - tw - M(0.015f) - rr;
                ui_circle(px, cy, rr, rgba(c >> 16 & 255, c >> 8 & 255, c & 255, 255));
                ui_ring(px, cy, rr, 1, with_alpha(t->text_dim, 120));
                tw += 2 * rr + M(0.015f);
            }
            if (arrows) {
                ui_text(f, "‹", vx - M(0.035f) - tw - M(0.02f), cy - fh / 2, vc, ALIGN_RIGHT);
                ui_text(f, "›", vx, cy - fh / 2, vc, ALIGN_RIGHT);
            }
        }
    }
    ui_clip_pop();
    char lines[2][160];
    const char *desc;
    if (set_sel >= ROW_CAT) { char k[64]; snprintf(k, sizeof k, "settings_cat_desc.%s", cat_key[set_sel - ROW_CAT]); desc = T(k); }
    else desc = set_text("settings_desc", set_sel);
    int n = ui_wrap(UI.f_small, desc, w, lines, 2);
    int dy = UI.h - M(t->hintbar_h) - desc_h + M(0.01f);
    for (int i = 0; i < n; i++) ui_text(UI.f_small, lines[i], mx, dy + i * TTF_FontHeight(UI.f_small), t->text_dim, ALIGN_LEFT);
    if (set_page < 0 && update_supported()) {   /* versione: utile quando si segnala un problema */
        char ver[96];
        snprintf(ver, sizeof ver, "Mosaik %s", update_current());
        ui_text(UI.f_small, ver, mx + w, UI.h - M(t->hintbar_h) - desc_h - TTF_FontHeight(UI.f_small) - M(0.01f), t->text_dim, ALIGN_RIGHT);
    }
    /* con la finestra di data e ora aperta, la barra dei tasti è quella della finestra */
    if (clock_open || sdinfo_open || wpanel_open || clean_open) return;
    if (set_page < 0) {
        const char *h[] = { "B", T("common.back"), "A", T("common.open") };
        ui_hints(h, 4);
    } else {
        const char *h[] = { "B", T("common.back"), "<>", T("common.change") };
        ui_hints(h, 4);
    }
}

/* --------------------------------------------------------- dialogo */

/* Sulla console, finché il sistema gira come app del firmware originale, c'è anche
   "Esci": torna al menu TrimUI. Pulsanti: annulla, (esci), spegni. */
static int dlg_buttons(void) { return plat_is_device() && !plat_standalone() ? 3 : 2; }

static void quit_to_menu(void) {
    settings_save(&UI.set, UI.root);
    music_quit();
    ui_shutdown_code(EXIT_QUIT);
    exit(EXIT_QUIT);
}

static void dialog_input(void) {
    input_t *in = &UI.in;
    int n = dlg_buttons();
    if (in->pressed[BTN_LEFT]) { dlg_sel = (dlg_sel + n - 1) % n; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_RIGHT]) { dlg_sel = (dlg_sel + 1) % n; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { dlg_open = false; ui_sound(SND_BACK); }
    if (in->pressed[BTN_A]) {
        if (dlg_sel == n - 1) power_off();
        if (n == 3 && dlg_sel == 1) quit_to_menu();
        dlg_open = false;
        ui_sound(SND_BACK);
    }
}

static void dialog_draw(void) {
    theme_t *t = &UI.th;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    int w = M(0.75f), h = M(0.36f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    ui_text(UI.f_title, T("dialog.power_title"), x + w / 2, y + M(0.06f), t->text, ALIGN_CENTER);
    ui_text(UI.f_small, T(dlg_buttons() == 3 ? "dialog.power_body_app" : "dialog.power_body"), x + w / 2, y + M(0.06f) + TTF_FontHeight(UI.f_title) + M(0.01f), t->text_dim, ALIGN_CENTER);
    int n = dlg_buttons(), bw = (w - M(0.03f) * (n + 1)) / n, bh = M(0.085f), by = y + h - bh - M(0.04f);
    const char *labels[3] = { T("common.cancel"), n == 3 ? T("common.quit_menu") : T("common.power_off"), T("common.power_off") };
    for (int i = 0; i < n; i++) {
        int bx = x + M(0.03f) + i * (bw + M(0.03f));
        bool sel = dlg_sel == i;
        ui_rrect(bx, by, bw, bh, bh / 2, sel ? t->accent : with_alpha(t->text_dim, 40));
        ui_text(UI.f_normal, labels[i], bx + bw / 2, by + (bh - TTF_FontHeight(UI.f_normal)) / 2, sel ? t->accent_text : t->text, ALIGN_CENTER);
    }
}

/* ------------------------------------------------------ scheda gioco */
/* Immagini a sinistra; a destra titolo, informazioni, statistiche e la storia del
   gioco (Dats/history/<rom>.txt, da ArcadeDB), che si scorre con su/giù. */

static char (*dlines)[220];
static int ndlines, dlines_game = -1, dlines_w;
static float detail_scroll, detail_scroll_target;

static void open_detail(int idx) {
    detail_idx = idx;
    detail_scroll = detail_scroll_target = 0;
    go(SCR_DETAIL);
    ui_sound(SND_OK);
}

/* toglie i caratteri che il font non ha (ideogrammi nei titoli giapponesi, byte non
   UTF-8), che altrimenti apparirebbero come quadratini; unisce gli spazi doppi rimasti */
static void strip_unsupported(TTF_Font *font, char *s) {
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        Uint32 cp = *r;
        int len = 1;
        if (cp >= 0xF0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80 && (r[3] & 0xC0) == 0x80) {
            cp = (cp & 7u) << 18 | (r[1] & 63u) << 12 | (r[2] & 63u) << 6 | (r[3] & 63u); len = 4;
        } else if (cp >= 0xE0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80) {
            cp = (cp & 15u) << 12 | (r[1] & 63u) << 6 | (r[2] & 63u); len = 3;
        } else if (cp >= 0xC0 && (r[1] & 0xC0) == 0x80) {
            cp = (cp & 31u) << 6 | (r[1] & 63u); len = 2;
        } else if (cp >= 0x80) {
            r++;
            continue;
        }
        bool keep = cp < 0x80 || (cp != 0xFFFD && TTF_GlyphIsProvided32(font, cp));
        if (keep && !(cp == ' ' && w > (unsigned char *)s && w[-1] == ' ')) {
            memmove(w, r, (size_t)len);
            w += len;
        }
        r += len;
    }
    *w = 0;
}

/* testo lungo diviso in righe larghe al massimo w pixel; le righe vuote separano i paragrafi */
static void wrap_history(const game_t *g, int w) {
    if (dlines_game == detail_idx && dlines_w == w) return;
    free(dlines);
    dlines = NULL;
    ndlines = 0;
    dlines_game = detail_idx;
    dlines_w = w;
    char p[1200];
    snprintf(p, sizeof p, "%s/Dats/history/%s.txt", UI.root, g->name);
    FILE *f = fopen(p, "r");
    if (!f) return;
    int cap = 64;
    dlines = malloc(sizeof *dlines * (size_t)cap);
    char para[4096];
    TTF_Font *font = UI.f_small;
    while (fgets(para, sizeof para, f)) {
        para[strcspn(para, "\n")] = 0;
        strip_unsupported(font, para);
        char cur[220] = "";
        char *save = NULL;
        bool any = false;
        for (char *word = strtok_r(para, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
            char test[440];
            snprintf(test, sizeof test, "%s%s%s", cur, cur[0] ? " " : "", word);
            if (cur[0] && ui_text_w(font, test) > w) {
                if (ndlines == cap) dlines = realloc(dlines, sizeof *dlines * (size_t)(cap *= 2));
                snprintf(dlines[ndlines++], 220, "%s", cur);
                snprintf(cur, sizeof cur, "%s", word);
            } else {
                snprintf(cur, sizeof cur, "%s", test);
            }
            any = true;
        }
        if (ndlines == cap) dlines = realloc(dlines, sizeof *dlines * (size_t)(cap *= 2));
        snprintf(dlines[ndlines++], 220, "%s", any ? cur : "");
    }
    fclose(f);
    while (ndlines && !dlines[ndlines - 1][0]) ndlines--;
}

static void format_duration(long secs, char *out, size_t n) {
    long h = secs / 3600, m = (secs % 3600) / 60;
    if (h) snprintf(out, n, T("detail.hours"), (int)h, (int)m);
    else snprintf(out, n, T("detail.minutes"), (int)(m ? m : (secs ? 1 : 0)));
}

static void detail_input(void) {
    input_t *in = &UI.in;
    if (detail_idx < 0) { go(back_screen); return; }
    game_t *g = &games[detail_idx];
    if (in->pressed[BTN_DOWN]) detail_scroll_target += 1;
    if (in->pressed[BTN_UP]) detail_scroll_target -= 1;
    if (in->pressed[BTN_R]) detail_scroll_target += 8;
    if (in->pressed[BTN_L]) detail_scroll_target -= 8;
    if (detail_scroll_target < 0) detail_scroll_target = 0;
    if (detail_scroll_target > (ndlines > 4 ? ndlines - 4 : 0)) detail_scroll_target = (float)(ndlines > 4 ? ndlines - 4 : 0);
    if (in->pressed[BTN_A]) launch(g);
    if (in->pressed[BTN_Y]) {
        games_toggle_fav(UI.root, g);
        ui_toast(g->fav ? T("home.fav_added") : T("home.fav_removed"));
        build_carousel();
        build_tabs();
        ui_sound(SND_OK);
    }
    if (in->pressed[BTN_X]) { gopt_open = true; gopt_sel = 0; ui_sound(SND_OK); return; }
    if (in->pressed[BTN_B]) { go(back_screen == SCR_DETAIL ? SCR_HOME : back_screen); ui_sound(SND_BACK); }
}

/* ------------------------------------------------- primo avvio guidato */
/* Solo su una scheda nuova: lingua, poi Wi-Fi facoltativo. I giochi li spiega la libreria vuota. */

static int setup_step;   /* 0 finito, 1 lingua, 2 Wi-Fi */

static void setup_finish(bool wifi) {
    setup_step = 0;
    UI.set.setup_done = true;
    settings_save(&UI.set, UI.root);
    if (wifi) {
        if (!UI.set.wifi) set_change(S_WIFI, +1);
        wpanel_start();
    }
}

static void setup_input(void) {
    input_t *in = &UI.in;
    if (setup_step == 1) {
        if (in->pressed[BTN_UP]) { set_change(S_LANG, -1); ui_sound(SND_MOVE); }
        if (in->pressed[BTN_DOWN]) { set_change(S_LANG, +1); ui_sound(SND_MOVE); }
        if (in->pressed[BTN_A]) {
            ui_sound(SND_OK);
            if (wifi_supported()) setup_step = 2;
            else setup_finish(false);
        }
    } else {
        if (in->pressed[BTN_A]) { ui_sound(SND_OK); setup_finish(true); }
        else if (in->pressed[BTN_B]) { ui_sound(SND_BACK); setup_finish(false); }
    }
}

static void setup_draw(void) {
    theme_t *t = &UI.th;
    int pad = M(0.05f), w = M(0.95f) < UI.w - 2 * M(t->margin) ? M(0.95f) : UI.w - 2 * M(t->margin), tw = w - 2 * pad;
    int fn = TTF_FontHeight(UI.f_normal), ft = TTF_FontHeight(UI.f_title), fs = TTF_FontHeight(UI.f_small), rh = fn + M(0.022f), x, y;
    if (setup_step == 1) {
        int sel = lang_index(), vis = nlangs < 6 ? nlangs : 6, first = sel - vis + 1 > 0 ? sel - vis + 1 : 0;
        panel_box(w, pad * 2 + ft + M(0.01f) + fs + M(0.025f) + rh * vis, &x, &y);
        int ty = y + pad;
        ui_text(UI.f_title, T("setup.welcome"), x + pad, ty, t->text, ALIGN_LEFT);
        ty += ft + M(0.01f);
        ui_text(UI.f_small, T("setup.language"), x + pad, ty, t->text_dim, ALIGN_LEFT);
        ty += fs + M(0.025f);
        for (int i = first; i < first + vis && i < nlangs; i++, ty += rh) {
            if (i == sel) ui_rrect(x + M(0.03f), ty, w - M(0.06f), rh - M(0.006f), M(t->panel_radius), t->accent);
            ui_text(UI.f_normal, lang_names[i], x + pad, ty + (rh - M(0.006f) - fn) / 2, i == sel ? t->accent_text : t->text, ALIGN_LEFT);
        }
        const char *hint[] = { "^v", T("common.change"), "A", T("common.ok") };
        ui_hints(hint, 4);
    } else {
        char lines[4][160];
        int n = ui_wrap(UI.f_normal, T("setup.wifi_body"), tw, lines, 4);
        panel_box(w, pad * 2 + ft + M(0.03f) + fn * n, &x, &y);
        int ty = y + pad;
        ui_text(UI.f_title, T("setup.wifi_title"), x + pad, ty, t->text, ALIGN_LEFT);
        ty += ft + M(0.03f);
        for (int i = 0; i < n; i++, ty += fn) ui_text(UI.f_normal, lines[i], x + pad, ty, t->text, ALIGN_LEFT);
        const char *hint[] = { "B", T("setup.later"), "A", T("setup.connect") };
        ui_hints(hint, 4);
    }
}

/* ------------------------------------------------- prova dei comandi */
/* Mostra ogni tasto e la posizione delle levette. B è un tasto da provare come gli altri:
   per uscire si tiene premuto. */

static double ptest_flash[3];   /* HOME, F1, F2 arrivano come impulsi: restano accesi un momento */

static void ptest_input(void) {
    last_input = UI.t;
    if (UI.in.home) ptest_flash[0] = UI.t;
    if (UI.in.f1) ptest_flash[1] = UI.t;
    if (UI.in.f2) ptest_flash[2] = UI.t;
    if (!UI.in.down[BTN_B]) { ptest_hold = 0; return; }
    ptest_hold += UI.dt;
    if (ptest_hold >= 1.0) { ptest_open = false; ui_sound(SND_BACK); }
}

static void ptest_stick(int cx, int cy, int r, int ax, int ay, const char *label) {
    theme_t *t = &UI.th;
    char val[32];
    int dot = r / 5;
    ui_circle(cx, cy, r, with_alpha(t->text, 26));
    ui_rect(cx - r, cy, 2 * r, 1, t->line);
    ui_rect(cx, cy - r, 1, 2 * r, t->line);
    ui_circle(cx + ax * (r - dot) / 32767, cy + ay * (r - dot) / 32767, dot, t->accent);
    ui_text(UI.f_small, label, cx, cy - r - TTF_FontHeight(UI.f_small) - M(0.008f), t->text_dim, ALIGN_CENTER);
    snprintf(val, sizeof val, "%+d  %+d", ax * 100 / 32767, ay * 100 / 32767);
    ui_text(UI.f_small, val, cx, cy + r + M(0.008f), t->text, ALIGN_CENTER);
}

static void ptest_draw(void) {
    static const struct { const char *name; int b; } keys[] = {
        { "UP", BTN_UP }, { "DOWN", BTN_DOWN }, { "LEFT", BTN_LEFT }, { "RIGHT", BTN_RIGHT }, { "A", BTN_A },
        { "B", BTN_B }, { "X", BTN_X }, { "Y", BTN_Y }, { "L", BTN_L }, { "R", BTN_R },
        { "L2", BTN_L2 }, { "R2", BTN_R2 }, { "SELECT", BTN_SELECT }, { "START", BTN_START }, { "MENU", BTN_MENU },
    };
    theme_t *t = &UI.th;
    int pad = M(0.04f), w = UI.w - 2 * M(t->margin), h = UI.h - M(t->hintbar_h) - 2 * M(0.03f), x, y;
    panel_box(w, h, &x, &y);
    ui_text(UI.f_title, T("ptest.title"), x + pad, y + pad, t->text, ALIGN_LEFT);
    int top = y + pad + TTF_FontHeight(UI.f_title) + M(0.04f), bottom = y + h - pad;
    int r = (bottom - top - 2 * TTF_FontHeight(UI.f_small) - M(0.04f)) / 2;
    if (r > M(0.13f)) r = M(0.13f);
    int cy = (top + bottom) / 2;
    ptest_stick(x + pad + r, cy, r, UI.in.axis[0], UI.in.axis[1], T("ptest.left"));
    ptest_stick(x + w - pad - r, cy, r, UI.in.axis[2], UI.in.axis[3], T("ptest.right"));
    int ax = x + pad + 2 * r + pad, aw = w - 2 * (2 * pad + 2 * r), gap = M(0.012f), cols = 3;
    int nk = (int)(sizeof keys / sizeof keys[0]), cw = (aw - gap * (cols - 1)) / cols, ch = TTF_FontHeight(UI.f_small) + M(0.012f);
    bool extra = has_extra_keys();
    int rows = (nk + (extra ? 4 : 1) + cols - 1) / cols;
    int gy = cy - (rows * ch + (rows - 1) * gap) / 2;
    for (int i = 0; i < nk + 4; i++) {
        const char *name;
        bool on;
        if (i < nk) { name = keys[i].name; on = UI.in.down[keys[i].b]; }
        else if (i == nk) { name = "FN"; on = UI.in.fn == 1; }
        else {
            if (!extra) break;
            static const char *en[3] = { "HOME", "F1", "F2" };
            name = en[i - nk - 1];
            on = UI.t - ptest_flash[i - nk - 1] < 0.25 && ptest_flash[i - nk - 1] > 0;
        }
        int cx = ax + (i % cols) * (cw + gap), yy = gy + (i / cols) * (ch + gap);
        ui_rrect(cx, yy, cw, ch, ch / 3, on ? t->accent : with_alpha(t->text, 20));
        ui_text(UI.f_small, name, cx + cw / 2, yy + (ch - TTF_FontHeight(UI.f_small)) / 2, on ? t->accent_text : t->text, ALIGN_CENTER);
    }
    const char *hint[] = { "B", T("ptest.exit") };
    ui_hints(hint, 2);
}

/* ------------------------------------------------- diagnosi del gioco */
/* Spiega cosa impedisce di avviare un gioco e quale file serve. Si apre dalle opzioni del
   gioco e, da sola, quando il runner non riesce ad avviarlo (System/last-failure.txt). */

static bool diag_open, diag_failed;
static rdiag_t diag;
static char diag_title[160], diag_msg[256];

static bool diag_possible(const game_t *g) { return !g->missing && strcmp(g->core, "linux"); }

static void diag_show(const game_t *g, bool failed, bool core_failed, const char *msg) {
    ui_busy(T("diag.checking"), 0);
    const emulator_t *emu = emulator_find(g->core);
    char core[1100];
    snprintf(core, sizeof core, "%s/Cores/%s.%s", UI.root, emu ? emu->core : g->core, CORE_EXT);
    plat_audio_mute();
    romcheck_diagnose(UI.root, g, core, core_failed, &diag);
    plat_audio_unmute();
    snprintf(diag_title, sizeof diag_title, "%s", g->title);
    snprintf(diag_msg, sizeof diag_msg, "%s", msg ? msg : "");
    diag_failed = failed;
    diag_open = true;
    ui_log("diagnosi %s: tipo %d (%s)", g->name, diag.kind, diag.file);
}

/* il runner ha lasciato una riga "core|game<TAB>percorso della ROM<TAB>errore del core" */
static void diag_check_failure(void) {
    char p[1100], line[2400];
    snprintf(p, sizeof p, "%s/System/last-failure.txt", UI.root);
    FILE *f = fopen(p, "r");
    if (!f) return;
    bool got = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    remove(p);
    if (!got) return;
    line[strcspn(line, "\r\n")] = 0;
    char *rom = strchr(line, '\t'), *msg = rom ? strchr(rom + 1, '\t') : NULL;
    if (!msg) return;
    *rom++ = 0;
    *msg++ = 0;
    for (int i = 0; i < ngames; i++)
        if (games[i].path && !strcmp(games[i].path, rom) && diag_possible(&games[i])) {
            diag_show(&games[i], true, !strcmp(line, "core"), msg);
            return;
        }
    ui_toast(T("common.launch_error"));
}

static void diag_input(void) {
    if (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B]) { diag_open = false; ui_sound(SND_BACK); }
}

static void diag_draw(void) {
    theme_t *t = &UI.th;
    int pad = M(0.05f), w = M(0.95f) < UI.w - 2 * M(t->margin) ? M(0.95f) : UI.w - 2 * M(t->margin), tw = w - 2 * pad;
    bool refused = diag.kind == DG_OK && diag_failed;
    char head[32], bkey[32], body[900], msg[320], foot[320], lines[5][160], mlines[2][160], flines[2][160], fit[300];
    snprintf(head, sizeof head, refused ? "diag.head_failed" : "diag.head%d", diag.kind);
    snprintf(bkey, sizeof bkey, refused ? "diag.body_failed" : "diag.body%d", diag.kind);
    snprintf(body, sizeof body, T(bkey), diag.file, diag.where);
    snprintf(msg, sizeof msg, "%s: %s", T("diag.core_said"), diag_msg);
    snprintf(foot, sizeof foot, "%s", T("diag.quick"));
    int fn = TTF_FontHeight(UI.f_normal), fs = TTF_FontHeight(UI.f_small);
    int nb = ui_wrap(UI.f_normal, body, tw, lines, 5);
    int nm = diag_failed && diag_msg[0] ? ui_wrap(UI.f_small, msg, tw, mlines, 2) : 0;
    int nf = diag.kind == DG_OK ? ui_wrap(UI.f_small, foot, tw, flines, 2) : 0;
    int h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.01f) + fs + M(0.03f) + fn + M(0.015f) + fn * nb, x, y;
    if (nm) h += M(0.02f) + fs * nm;
    if (nf) h += M(0.02f) + fs * nf;
    panel_box(w, h, &x, &y);
    int ty = y + pad;
    ui_text(UI.f_title, T("diag.title"), x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.01f);
    ui_text_fit(UI.f_small, diag_title, tw, fit, sizeof fit);
    ui_text(UI.f_small, fit, x + pad, ty, t->text_dim, ALIGN_LEFT);
    ty += fs + M(0.03f);
    ui_text(UI.f_normal, T(head), x + pad, ty, diag.kind == DG_OK && !diag_failed ? t->accent : rgba(230, 130, 20, 255), ALIGN_LEFT);
    ty += fn + M(0.015f);
    for (int i = 0; i < nb; i++, ty += fn) ui_text(UI.f_normal, lines[i], x + pad, ty, t->text, ALIGN_LEFT);
    if (nm) ty += M(0.02f);
    for (int i = 0; i < nm; i++, ty += fs) ui_text(UI.f_small, mlines[i], x + pad, ty, t->text_dim, ALIGN_LEFT);
    if (nf) ty += M(0.02f);
    for (int i = 0; i < nf; i++, ty += fs) ui_text(UI.f_small, flines[i], x + pad, ty, t->text_dim, ALIGN_LEFT);
    const char *hint[] = { "A", T("common.ok") };
    ui_hints(hint, 2);
}

/* ------------------------------------------------- conferme */

static void cfm_input(void) {
    int r = ui_confirm_input(&cfm_sel);
    if (r < 0) return;
    int k = cfm_kind;
    cfm_kind = CFM_NONE;
    if (r != 1) return;
    if (k == CFM_FORGET) {
        char m[160];
        snprintf(m, sizeof m, T("wifi.forgot"), cfm_ssid);
        if (wifi_forget(cfm_ssid)) ui_toast(m);
        wpanel_refresh(false);
    } else {
        games_clear_genre_groups(UI.root);
        genres_changed();
        ui_toast(T("genre_ui.restored"));
        gp_sel = 0;
    }
}

static void cfm_draw(void) {
    if (cfm_kind == CFM_FORGET) ui_confirm_draw(T("confirm.forget_title"), T("confirm.forget_body"), T("common.cancel"), T("wifi.forget"), cfm_sel);
    else ui_confirm_draw(T("confirm.genres_title"), T("confirm.genres_body"), T("common.cancel"), T("confirm.restore"), cfm_sel);
}

/* ------------------------------------------------- reimposta il gioco */
/* Riporta un gioco com'era la prima volta: impostazioni proprie, DIP switch, punteggi e dati
   interni. La ROM non si tocca; gli stati salvati solo se lo si chiede a parte. */

static int reset_step, reset_sel;   /* 0 chiuso, 1 conferma, 2 anche gli stati? */

static void reset_remove(const char *dir, const char *rom, const char *ext) {
    char p[1600];
    snprintf(p, sizeof p, "%s/%s.%s", dir, rom, ext);
    if (remove(p) == 0) ui_log("reimposta: tolto %s", p);
}

static void game_reset(const game_t *g, bool states) {
    const emulator_t *emu = emulator_find(g->core);
    char core[80], base[1200], sub[1300];
    snprintf(core, sizeof core, "%s", emu ? emu->core : g->core);
    char *us = strstr(core, "_libretro");
    if (us) *us = 0;
    snprintf(base, sizeof base, "%s/Saves/%s", UI.root, core);
    reset_remove(base, g->name, "cfg");
    reset_remove(base, g->name, "opt");
    if (states) { reset_remove(base, g->name, "state"); reset_remove(base, g->name, "auto.state"); }
    /* FBNeo: memoria interna e punteggi */
    snprintf(sub, sizeof sub, "%s/fbneo", base);
    reset_remove(sub, g->name, "fs");
    reset_remove(sub, g->name, "nv");
    reset_remove(sub, g->name, "hi");
    reset_remove(sub, g->name, "diff");
    snprintf(sub, sizeof sub, "%s/Bios/fbneo", UI.root);
    reset_remove(sub, g->name, "hi");
    /* MAME 2003-Plus */
    snprintf(sub, sizeof sub, "%s/mame2003-plus/nvram", base);
    reset_remove(sub, g->name, "nv");
    snprintf(sub, sizeof sub, "%s/mame2003-plus/hi", base);
    reset_remove(sub, g->name, "hi");
    snprintf(sub, sizeof sub, "%s/mame2003-plus/cfg", base);
    reset_remove(sub, g->name, "cfg");
    ui_toast(T("reset.done"));
}

static void reset_input(void) {
    int r = ui_confirm_input(&reset_sel);
    if (r < 0) return;
    if (reset_step == 1) {
        if (r != 1) { reset_step = 0; return; }
        reset_step = 2;
        reset_sel = 0;
        return;
    }
    reset_step = 0;
    if (detail_idx >= 0) game_reset(&games[detail_idx], r == 1);
}

static void reset_draw(void) {
    if (reset_step == 1) ui_confirm_draw(T("reset.title"), T("reset.body"), T("common.cancel"), T("reset.confirm"), reset_sel);
    else ui_confirm_draw(T("reset.states_title"), T("reset.states_body"), T("common.no"), T("common.yes"), reset_sel);
}

/* ------------------------------------------ opzioni del gioco (collezioni) */
/* righe: una per collezione (spunta), "nuova collezione", "nascondi/mostra" */

static void lists_changed(void) {
    mark_duplicates();
    build_cats();
    build_carousel();
    build_tabs();
    build_view();
}

static void gopt_input(void) {
    input_t *in = &UI.in;
    game_t *g = &games[detail_idx];
    int rows = NCOLLS + 3 + (diag_possible(g) ? 2 : 0);
    if (in->pressed[BTN_UP]) { gopt_sel = (gopt_sel + rows - 1) % rows; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { gopt_sel = (gopt_sel + 1) % rows; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B] || in->pressed[BTN_X]) { gopt_open = false; ui_sound(SND_BACK); return; }
    if (!in->pressed[BTN_A]) return;
    char msg[200];
    if (gopt_sel < NCOLLS) {
        bool in_c = !(g->colls >> gopt_sel & 1u);
        games_set_coll(UI.root, g, gopt_sel, in_c);
        snprintf(msg, sizeof msg, T(in_c ? "coll.added" : "coll.removed"), COLLS[gopt_sel]);
        ui_toast(msg);
        lists_changed();
        ui_sound(SND_OK);
    } else if (gopt_sel == NCOLLS) {
        coll_name[0] = 0;
        kb_mode = KBM_COLL;
        kb_open = true;
        kb_row = 1;
        kb_col = 0;
        ui_sound(SND_OK);
    } else if (gopt_sel == NCOLLS + 2) {
        gp_show(GP_GAME);
    } else if (gopt_sel == NCOLLS + 3) {
        gopt_open = false;
        ui_sound(SND_OK);
        diag_show(g, false, false, NULL);
    } else if (gopt_sel == NCOLLS + 4) {
        gopt_open = false;
        reset_step = 1;
        reset_sel = 0;
        ui_sound(SND_OK);
    } else {
        games_set_hidden(UI.root, g, !g->user_hidden);
        ui_toast(T(g->user_hidden ? "coll.hidden_toast" : "coll.shown_toast"));
        lists_changed();
        ui_sound(SND_OK);
    }
}

/* segmento spesso: un quadrilatero fatto di due triangoli */
static void thick_line(int x1, int y1, int x2, int y2, int th, color_t c) {
    float dx = (float)(x2 - x1), dy = (float)(y2 - y1), len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    int ox = (int)lroundf(-dy / len * th / 2), oy = (int)lroundf(dx / len * th / 2);
    ui_triangle(x1 + ox, y1 + oy, x2 + ox, y2 + oy, x2 - ox, y2 - oy, c);
    ui_triangle(x1 + ox, y1 + oy, x2 - ox, y2 - oy, x1 - ox, y1 - oy, c);
    ui_circle(x1, y1, th / 2, c);
    ui_circle(x2, y2, th / 2, c);
}

static void gopt_draw(void) {
    theme_t *t = &UI.th;
    if (detail_idx < 0) return;
    game_t *g = &games[detail_idx];
    int rows = NCOLLS + 3 + (diag_possible(g) ? 2 : 0), rh = M(t->list_row_h), pad = M(0.04f);
    int maxrows = (UI.h - M(t->hintbar_h) - M(0.2f) - TTF_FontHeight(UI.f_title)) / rh;
    int vis = rows < maxrows ? rows : maxrows;
    int w = M(0.95f) < UI.w - 2 * M(t->margin) ? M(0.95f) : UI.w - 2 * M(t->margin);
    int h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.02f) + rh * vis, x, y;
    panel_box(w, h, &x, &y);
    ui_text(UI.f_title, T("coll.menu_title"), x + pad, y + pad, t->text, ALIGN_LEFT);
    int ry = y + pad + TTF_FontHeight(UI.f_title) + M(0.02f);
    int first = gopt_sel - vis + 1 > 0 ? gopt_sel - vis + 1 : 0;
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    for (int i = first; i < first + vis && i < rows; i++, ry += rh) {
        bool sel = i == gopt_sel;
        if (sel) ui_rrect(x + M(0.02f), ry, w - M(0.04f), rh - M(0.008f), M(t->panel_radius), t->accent);
        color_t c = sel ? t->accent_text : t->text;
        int ty = ry + (rh - M(0.008f) - fh) / 2, bx = x + M(0.05f), box = fh * 3 / 4;
        if (i < NCOLLS) {
            /* casella di spunta */
            bool on = g->colls >> i & 1u;
            int by = ty + (fh - box) / 2;
            if (on) {
                ui_rrect(bx, by, box, box, box / 4, sel ? t->accent_text : t->accent);
                color_t tick = sel ? t->accent : t->accent_text;
                int th = box / 7 + 1;
                thick_line(bx + box * 22 / 100, by + box * 52 / 100, bx + box * 42 / 100, by + box * 72 / 100, th, tick);
                thick_line(bx + box * 42 / 100, by + box * 72 / 100, bx + box * 80 / 100, by + box * 30 / 100, th, tick);
            } else {
                ui_rborder(bx, by, box, box, box / 4, M(0.004f) + 1, sel ? c : t->text_dim);
            }
            ui_text(f, COLLS[i], bx + box + M(0.025f), ty, c, ALIGN_LEFT);
            int count = 0;
            for (int k = 0; k < ngames; k++) count += games[k].colls >> i & 1u;
            char num[16];
            snprintf(num, sizeof num, "%d", count);
            ui_text(UI.f_small, num, x + w - M(0.05f), ty + (fh - TTF_FontHeight(UI.f_small)) / 2, sel ? c : t->text_dim, ALIGN_RIGHT);
        } else if (i == NCOLLS) {
            ui_text(f, "+", bx + box / 2, ty, sel ? c : t->accent, ALIGN_CENTER);
            ui_text(f, T("coll.new"), bx + box + M(0.025f), ty, c, ALIGN_LEFT);
        } else if (i == NCOLLS + 2) {
            char gl[48];
            genre_label(g->genre, gl, sizeof gl);
            ui_text(f, T("genre_ui.row"), bx + box + M(0.025f), ty, c, ALIGN_LEFT);
            ui_text(UI.f_small, gl[0] ? gl : "—", x + w - M(0.05f), ty + (fh - TTF_FontHeight(UI.f_small)) / 2, sel ? c : t->text_dim, ALIGN_RIGHT);
        } else if (i == NCOLLS + 3) {
            ui_text(f, T("diag.title"), bx + box + M(0.025f), ty, c, ALIGN_LEFT);
        } else if (i == NCOLLS + 4) {
            ui_text(f, T("reset.row"), bx + box + M(0.025f), ty, c, ALIGN_LEFT);
        } else {
            ui_text(f, T(g->user_hidden ? "coll.unhide" : "coll.hide"), bx + box + M(0.025f), ty, c, ALIGN_LEFT);
        }
    }
    const char *hint[] = { "B", T("common.back"), "A", T("common.ok") };
    ui_hints(hint, 4);
}

static void detail_draw(void) {
    theme_t *t = &UI.th;
    if (detail_idx < 0) return;
    game_t *g = &games[detail_idx];
    ui_topbar(T("detail.title"));
    int mx = M(t->margin), top = M(t->topbar_h) + M(0.01f), bottom = UI.h - M(t->hintbar_h) - M(0.02f);
    int lw = UI.w * 40 / 100;

    /* sinistra: un cabinato, con il marquee illuminato in alto e lo schermo sotto */
    int r = M(t->tile_radius), cw = lw, cx = mx, cy = top;
    static char marquee_type[1][16] = { "marquee" };
    SDL_Texture *mq = media_image(g, marquee_type, 1);
    SDL_Texture *shot = media_image(g, t->preview_types, t->npreview);
    int mh = 0, strip = mq ? M(0.006f) : 0;
    if (mq) {
        /* altezza dell'insegna dalle proporzioni del marquee, entro limiti ragionevoli */
        int qw, qh;
        SDL_QueryTexture(mq, NULL, NULL, &qw, &qh);
        mh = qw > 0 ? cw * qh / qw : cw / 4;
        if (mh < cw / 5) mh = cw / 5;
        if (mh > cw * 30 / 100) mh = cw * 30 / 100;
    }
    int iw = g->vertical ? 3 : 4, ih = g->vertical ? 4 : 3;
    if (shot) SDL_QueryTexture(shot, NULL, NULL, &iw, &ih);
    int avail = bottom - cy - mh - strip;
    int sh = iw >= ih ? cw * ih / iw : avail;   /* i giochi verticali usano tutta l'altezza */
    if (sh > avail) sh = avail;
    int ch = mh + strip + sh, cut = mq ? r : 0;   /* sotto il marquee lo schermo ha gli angoli squadrati */
    ui_shadow(cx, cy + M(0.012f), cw, ch, r, M(0.035f), t->shadow);
    ui_rrect(cx, cy, cw, ch, r, rgba(16, 16, 20, 255));
    if (mq) {
        /* il logo resta intero; ai lati lo stesso marquee sfocato fa da insegna luminosa */
        ui_clip_push(cx, cy, cw, mh);
        ui_image_fill(ui_image_blur(mq), cx, cy, cw, mh + r, r);
        ui_rect(cx, cy, cw, mh, rgba(0, 0, 0, 140));
        ui_image_fit(mq, cx, cy, cw, mh);
        ui_clip_pop();
        ui_rect(cx, cy + mh, cw, strip, t->accent);
    }
    int sy = cy + mh + strip;
    ui_clip_push(cx, sy, cw, sh);
    if (shot && iw * sh / ih >= cw - 2) {
        ui_image_fill(shot, cx, sy - cut, cw, sh + cut, r);
    } else if (shot) {
        /* schermo verticale o più stretto del riquadro: ai lati la stessa immagine sfocata */
        ui_image_fill(ui_image_blur(shot), cx, sy - cut, cw, sh + cut, r);
        ui_rect(cx, sy, cw, sh, rgba(0, 0, 0, 120));
        ui_image_fit(shot, cx, sy, cw, sh);
    } else {
        draw_cover(g, cx, sy - cut, cw, sh + cut, r);
    }
    ui_vgradient(cx, sy, cw, sh / 3, rgba(255, 255, 255, 16), rgba(255, 255, 255, 0));   /* riflesso del vetro */
    ui_clip_pop();

    /* destra: titolo, etichette, statistiche e storia */
    int rx = mx + lw + M(0.045f), rw = UI.w - rx - mx, ry = top - M(0.008f);
    char lines[2][160], buf[300], fit[300];
    int n = ui_wrap(UI.f_title, g->title, rw, lines, 2);
    for (int i = 0; i < n; i++, ry += TTF_FontHeight(UI.f_title)) ui_text(UI.f_title, lines[i], rx, ry, t->text, ALIGN_LEFT);
    ry += M(0.012f);
    {
        char players[32] = "", genre[48];
        if (g->players > 1) snprintf(players, sizeof players, T("info.players"), g->players);
        const char *sys = strncmp(g->maker, g->system, strlen(g->system)) ? g->system : "";
        const char *tags[8] = { games_core_label(g->core), g->year, g->maker, sys, genre_label(g->genre, genre, sizeof genre), players,
                                g->vertical ? T("info.vertical") : "", g->user_hidden ? T("coll.hidden_tag") : "" };
        int fh = TTF_FontHeight(UI.f_small), ph = fh + M(0.006f), gap = M(0.012f), px = rx, rows = 0;
        for (int i = 0; i < 8; i++) {
            if (!tags[i] || !tags[i][0]) continue;
            ui_text_fit(UI.f_small, tags[i], rw - M(0.04f), fit, sizeof fit);
            int pw = ui_text_w(UI.f_small, fit) + M(0.036f);
            if (px > rx && px + pw > rx + rw) {
                if (++rows == 2) break;
                px = rx;
                ry += ph + gap;
            }
            bool core = i == 0;
            ui_rrect(px, ry, pw, ph, ph / 2, core ? t->accent : with_alpha(t->text, 20));
            ui_text(UI.f_small, fit, px + M(0.018f), ry + (ph - fh) / 2, core ? t->accent_text : t->text_dim, ALIGN_LEFT);
            px += pw + gap;
        }
        ry += ph + M(0.02f);
    }
    /* statistiche */
    if (g->missing) {
        const emulator_t *emu = emulator_find(g->core);
        snprintf(buf, sizeof buf, T("missing.toast"), g->name, emu ? emu->roms : "Roms");
    } else if (g->plays) {
        char dur[64], when[64];
        format_duration(g->seconds, dur, sizeof dur);
        time_t lp = (time_t)g->last_played;
        struct tm tmv;
        localtime_r(&lp, &tmv);
        strftime(when, sizeof when, "%d/%m/%Y", &tmv);
        snprintf(buf, sizeof buf, T("detail.stats"), g->plays, dur, when);
    } else {
        snprintf(buf, sizeof buf, "%s", T("detail.never"));
    }
    ui_text_fit(UI.f_small, buf, rw, fit, sizeof fit);
    ui_text(UI.f_small, fit, rx, ry, t->accent, ALIGN_LEFT);
    ry += TTF_FontHeight(UI.f_small);
    if (g->romstatus != RC_NONE) {
        char key[32], detail[300];
        snprintf(key, sizeof key, "rom.status%d", g->romstatus);
        romcheck_detail(UI.root, g, detail, sizeof detail);
        snprintf(buf, sizeof buf, "%s%s%s", T(key), detail[0] ? ": " : "", detail);
        ui_text_fit(UI.f_small, buf, rw, fit, sizeof fit);
        ui_text(UI.f_small, fit, rx, ry, rom_problem(g) ? rgba(230, 130, 20, 255) : t->text_dim, ALIGN_LEFT);
        ry += TTF_FontHeight(UI.f_small);
    }
    ry += M(0.02f);
    ui_rect(rx, ry, rw, 1, t->line);
    ry += M(0.015f);

    /* storia, scorrevole */
    wrap_history(g, rw - M(0.02f));
    int lh = TTF_FontHeight(UI.f_small);
    detail_scroll = ui_approach(detail_scroll, detail_scroll_target);
    ui_clip_push(rx, ry, rw, bottom - ry);
    if (!ndlines) ui_text(UI.f_small, T("detail.no_history"), rx, ry, t->text_dim, ALIGN_LEFT);
    for (int i = 0; i < ndlines; i++) {
        int ly = ry + (int)((i - detail_scroll) * lh);
        if (ly + lh < ry || ly > bottom) continue;
        ui_text(UI.f_small, dlines[i], rx, ly, t->text, ALIGN_LEFT);
    }
    ui_clip_pop();
    const char *h[] = { "^v", T("detail.scroll"), "X", T("coll.hint"), "Y", T(g->fav ? "common.unfavorite" : "common.favorite"), "B", T("common.back"),
                        "A", T("common.play") };
    ui_hints(h, 10);
}

/* ------------------------------------------------------ verifica ROM */

static void romcheck_input(void) {
    last_input = UI.t;   /* niente spegnimento automatico mentre lavora */
    const romcheck_status_t *s = romcheck_status();
    if (!s->finished && UI.in.pressed[BTN_B]) { romcheck_cancel(); ui_sound(SND_BACK); }
    if (s->finished && (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B])) {
        romcheck_join();
        romcheck_open = false;
        romcheck_load(UI.root, games, ngames);
        build_view();
        ui_sound(SND_OK);
    }
}

static void romcheck_draw(void) {
    theme_t *t = &UI.th;
    const romcheck_status_t *s = romcheck_status();
    int pad = M(0.05f), w = M(0.95f) < UI.w - 2 * M(t->margin) ? M(0.95f) : UI.w - 2 * M(t->margin);
    int h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.03f) + TTF_FontHeight(UI.f_normal) * 2 + M(0.06f) + TTF_FontHeight(UI.f_small) * 3, x, y;
    panel_box(w, h, &x, &y);
    int ty = y + pad;
    ui_text(UI.f_title, T("rom.title"), x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.03f);
    char line[300];
    snprintf(line, sizeof line, T("rom.progress"), s->done, s->total);
    ui_text(UI.f_normal, s->finished ? T(s->cancel ? "scrape.stopped" : "scrape.done") : line, x + pad, ty, t->text_dim, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_normal) + M(0.02f);
    int bw = w - 2 * pad, bh = M(0.02f);
    float frac = s->total ? (float)s->done / s->total : 1.f;
    ui_rrect(x + pad, ty, bw, bh, bh / 2, with_alpha(t->text_dim, 70));
    if (frac > 0) ui_rrect(x + pad, ty, (int)(bw * frac), bh, bh / 2, t->accent);
    ty += bh + M(0.03f);
    snprintf(line, sizeof line, T("rom.summary"), s->count[RC_OK], s->count[RC_MISSING], s->count[RC_BADCRC], s->count[RC_NOPARENT]);
    char lines[2][160];
    int nl = ui_wrap(UI.f_small, line, w - 2 * pad, lines, 2);
    for (int i = 0; i < nl; i++, ty += TTF_FontHeight(UI.f_small)) ui_text(UI.f_small, lines[i], x + pad, ty, t->text, ALIGN_LEFT);
    snprintf(line, sizeof line, T("rom.summary2"), s->count[RC_NOTINDAT], s->count[RC_UNCHECKED]);
    ui_text(UI.f_small, line, x + pad, ty, t->text_dim, ALIGN_LEFT);
    const char *hint[] = { s->finished ? "A" : "B", T(s->finished ? "common.ok" : "scrape.stop") };
    ui_hints(hint, 2);
}

/* ------------------------------------------------ filtri e ricerca */
/* SELECT apre il pannello: ricerca, genere, ordinamento. La tastiera a schermo
   filtra la libreria mentre si scrive. */

enum { FP_SEARCH, FP_GENRE, FP_COLL, FP_SORT, FP_ROMS, FP_CLEAR, FP_COUNT };
static const char *sort_keys[4] = { "sort.name", "sort.year", "sort.played", "sort.recent" };

static void filters_changed(void) {
    lib_sel = 0;
    lib_scroll_target = 0;
    build_view();
    for (int i = 0; i < ncats; i++) cat_item[i] = 0;
}

static void fpanel_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_UP]) { fpanel_sel = (fpanel_sel + FP_COUNT - 1) % FP_COUNT; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { fpanel_sel = (fpanel_sel + 1) % FP_COUNT; ui_sound(SND_MOVE); }
    int dir = in->pressed[BTN_RIGHT] ? 1 : in->pressed[BTN_LEFT] ? -1 : 0;
    if (in->pressed[BTN_A] && (fpanel_sel == FP_GENRE || fpanel_sel == FP_COLL || fpanel_sel == FP_SORT || fpanel_sel == FP_ROMS)) dir = 1;
    if (dir && fpanel_sel == FP_COLL) {
        /* giro: tutte, ogni collezione, nascosti */
        int pos = coll_sel == COLL_ALL ? 0 : coll_sel == COLL_HIDDEN ? NCOLLS + 1 : coll_sel + 1;
        pos = (pos + dir + NCOLLS + 2) % (NCOLLS + 2);
        coll_sel = pos == 0 ? COLL_ALL : pos == NCOLLS + 1 ? COLL_HIDDEN : pos - 1;
        build_tabs();
        filters_changed();
        ui_sound(SND_MOVE);
    }
    if (dir && fpanel_sel == FP_ROMS) {
        only_problems = !only_problems;
        filters_changed();
        ui_sound(SND_MOVE);
    }
    if (dir && fpanel_sel == FP_GENRE && ngenres) {
        genre_sel = genre_sel + dir;
        if (genre_sel >= ngenres) genre_sel = -1;
        if (genre_sel < -1) genre_sel = ngenres - 1;
        filters_changed();
        ui_sound(SND_MOVE);
    }
    if (dir && fpanel_sel == FP_SORT) {
        UI.set.sort_mode = (UI.set.sort_mode + dir + 4) % 4;
        settings_save(&UI.set, UI.root);
        filters_changed();
        ui_sound(SND_MOVE);
    }
    if (in->pressed[BTN_A] && fpanel_sel == FP_SEARCH) { kb_open = true; kb_mode = KBM_SEARCH; kb_row = 1; kb_col = 0; ui_sound(SND_OK); }
    if (in->pressed[BTN_A] && fpanel_sel == FP_CLEAR) {
        search[0] = 0;
        genre_sel = -1;
        coll_sel = COLL_ALL;
        only_problems = false;
        build_tabs();
        filters_changed();
        ui_sound(SND_OK);
    }
    if (in->pressed[BTN_B] || in->pressed[BTN_SELECT]) { fpanel_open = false; ui_sound(SND_BACK); }
}

static void panel_box(int w, int h, int *x, int *y) {
    theme_t *t = &UI.th;
    *x = (UI.w - w) / 2;
    *y = (UI.h - M(t->hintbar_h) - h) / 2;
    int r = M(t->panel_radius) * 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(*x, *y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(*x, *y, w, h, r, t->surface);
    if (t->surface.a < 200) ui_rrect(*x, *y, w, h, r, with_alpha(t->bg2, 235));   /* superfici trasparenti */
    int hb = M(t->hintbar_h);
    ui_rect(0, UI.h - hb, UI.w, hb, t->bg);   /* copre gli aiuti della schermata sotto */
}

static void fpanel_draw(void) {
    theme_t *t = &UI.th;
    int rh = M(t->list_row_h) + M(0.01f), pad = M(0.04f);
    int w = M(1.05f) < UI.w - 2 * M(t->margin) ? M(1.05f) : UI.w - 2 * M(t->margin);
    int h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.02f) + rh * FP_COUNT + TTF_FontHeight(UI.f_small) + M(0.02f), x, y;
    panel_box(w, h, &x, &y);
    ui_text(UI.f_title, T("filters.title"), x + pad, y + pad, t->text, ALIGN_LEFT);
    int ry = y + pad + TTF_FontHeight(UI.f_title) + M(0.02f);
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    for (int i = 0; i < FP_COUNT; i++, ry += rh) {
        bool sel = i == fpanel_sel;
        if (sel) ui_rrect(x + M(0.02f), ry, w - M(0.04f), rh - M(0.008f), M(t->panel_radius), t->accent);
        color_t c = sel ? t->accent_text : t->text;
        char val[120] = "", g[64];
        const char *label = i == FP_SEARCH ? T("filters.search") : i == FP_GENRE ? T("library.genre") : i == FP_COLL ? T("coll.filter")
                          : i == FP_SORT ? T("filters.sort") : i == FP_ROMS ? T("filters.roms") : T("filters.clear");
        if (i == FP_COLL) {
            int nh = 0;
            for (int k = 0; k < ngames; k++) nh += games[k].user_hidden;
            if (coll_sel == COLL_HIDDEN) snprintf(val, sizeof val, T("coll.hidden_view"), nh);
            else snprintf(val, sizeof val, "%s", coll_sel < 0 ? T("library.genre_all") : COLLS[coll_sel]);
        }
        if (i == FP_SEARCH) snprintf(val, sizeof val, "%s", search[0] ? search : T("filters.search_empty"));
        if (i == FP_GENRE) snprintf(val, sizeof val, "%s", genre_sel < 0 ? T("library.genre_all") : genre_label(genres[genre_sel], g, sizeof g));
        if (i == FP_SORT) snprintf(val, sizeof val, "%s", T(sort_keys[UI.set.sort_mode % 4]));
        if (i == FP_ROMS) snprintf(val, sizeof val, "%s", T(only_problems ? "filters.roms_bad" : "library.genre_all"));
        int ty = ry + (rh - M(0.008f) - fh) / 2;
        ui_text(f, label, x + M(0.05f), ty, c, ALIGN_LEFT);
        if (val[0]) {
            char shown[140];
            snprintf(shown, sizeof shown, sel && i != FP_SEARCH ? "‹ %s ›" : "%s", val);
            ui_text(f, shown, x + w - M(0.05f), ty, sel ? c : t->text_dim, ALIGN_RIGHT);
        }
    }
    char res[64];
    snprintf(res, sizeof res, T("kb.results"), nview);
    ui_text(UI.f_small, res, x + pad, ry + M(0.01f), t->text_dim, ALIGN_LEFT);
    const char *hint[] = { "B", T("common.back"), "A", T("common.ok") };
    ui_hints(hint, 4);
}

/* tastiera: 5 righe; l'ultima ha spazio, cancella e fatto. Tre disposizioni (X): maiuscole,
   minuscole e simboli, per le password Wi-Fi. */
static const char *kb_layouts[3][4] = {
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-.&" },
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.&" },
    { "1234567890", "!@#$%^&*()", "-_=+[]{};:", "'\"\\|,.<>/?" },
};
#define kb_rows kb_layouts[kb_layout % 3]
enum { KB_SPACE, KB_DEL, KB_DONE };

static char *kb_buf(size_t *cap) {
    if (kb_mode == KBM_WIFI) { if (cap) *cap = sizeof wifi_pass; return wifi_pass; }
    if (kb_mode == KBM_GENRE_NEW || kb_mode == KBM_GENRE_RENAME) { if (cap) *cap = sizeof genre_text; return genre_text; }
    if (cap) *cap = kb_mode == KBM_COLL ? sizeof coll_name : sizeof search;
    return kb_mode == KBM_COLL ? coll_name : search;
}

static void kb_type(char c) {
    size_t cap, n = strlen(kb_buf(&cap));
    char *b = kb_buf(NULL);
    if (n + 1 < cap) { b[n] = c; b[n + 1] = 0; }
    if (kb_mode == KBM_SEARCH) filters_changed();
}

static void kb_delete(void) {
    char *b = kb_buf(NULL);
    size_t n = strlen(b);
    if (n) b[n - 1] = 0;
    if (kb_mode == KBM_SEARCH) filters_changed();
}

static void lists_changed(void);

/* fine scrittura: per la ricerca chiude anche i filtri; per la collezione la crea */
static void wifi_join(const char *ssid, const char *psk);

static void genre_kb_done(void);

static void kb_done(void) {
    if (kb_mode == KBM_GENRE_NEW || kb_mode == KBM_GENRE_RENAME) { kb_open = false; genre_kb_done(); return; }
    if (kb_mode == KBM_WIFI) {
        if (strlen(wifi_pass) < 8) { ui_toast(T("wifi.short")); ui_sound(SND_ERROR); return; }
        kb_open = false;
        wifi_join(wifi_target, wifi_pass);
        return;
    }
    kb_open = false;
    if (kb_mode == KBM_SEARCH) { fpanel_open = false; return; }
    if (detail_idx < 0) return;
    int ci = games_new_coll(UI.root, coll_name, games, ngames);
    if (ci < 0) { ui_toast(T("coll.failed")); return; }
    games_set_coll(UI.root, &games[detail_idx], ci, true);
    char msg[160];
    snprintf(msg, sizeof msg, T("coll.added"), COLLS[ci]);
    ui_toast(msg);
    lists_changed();
    gopt_sel = ci;
}

static void keyboard_input(void) {
    input_t *in = &UI.in;
    int cols = kb_row < 4 ? 10 : 3;
    if (in->pressed[BTN_UP]) { kb_row = (kb_row + 4) % 5; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { kb_row = (kb_row + 1) % 5; ui_sound(SND_MOVE); }
    cols = kb_row < 4 ? 10 : 3;
    if (kb_col >= cols) kb_col = cols - 1;
    if (in->pressed[BTN_LEFT]) { kb_col = (kb_col + cols - 1) % cols; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_RIGHT]) { kb_col = (kb_col + 1) % cols; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_Y]) { kb_type(' '); ui_sound(SND_MOVE); }
    if (in->pressed[BTN_X]) { kb_layout = (kb_layout + 1) % 3; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) {
        if (kb_buf(NULL)[0]) { kb_delete(); ui_sound(SND_BACK); }
        else { kb_open = false; ui_sound(SND_BACK); }
    }
    if (in->pressed[BTN_START]) { kb_done(); ui_sound(SND_OK); }
    if (in->pressed[BTN_A]) {
        if (kb_row < 4) kb_type(kb_rows[kb_row][kb_col]);
        else if (kb_col == KB_SPACE) kb_type(' ');
        else if (kb_col == KB_DEL) kb_delete();
        else kb_done();
        ui_sound(SND_MOVE);
    }
}

static void keyboard_draw(void) {
    theme_t *t = &UI.th;
    int pad = M(0.035f), key = M(0.085f), gap = M(0.012f);
    int w = pad * 2 + 10 * key + 9 * gap;
    if (w > UI.w - M(0.04f)) { key = (UI.w - M(0.04f) - pad * 2 - 9 * gap) / 10; w = pad * 2 + 10 * key + 9 * gap; }
    int field_h = M(0.075f);
    int h = pad * 2 + field_h + M(0.02f) + 5 * key + 4 * gap + (kb_mode == KBM_SEARCH ? TTF_FontHeight(UI.f_small) + M(0.015f) : 0), x, y;
    panel_box(w, h, &x, &y);
    /* campo di testo con cursore lampeggiante */
    int fy = y + pad;
    ui_rrect(x + pad, fy, w - 2 * pad, field_h, M(t->panel_radius), with_alpha(t->text_dim, 40));
    TTF_Font *f = UI.f_normal;
    int tx = x + pad + M(0.02f), ty = fy + (field_h - TTF_FontHeight(f)) / 2;
    const char *typed = kb_buf(NULL);
    int tw = typed[0] ? ui_text(f, typed, tx, ty, t->text, ALIGN_LEFT) : 0;
    char prompt[96];
    if (kb_mode == KBM_WIFI) snprintf(prompt, sizeof prompt, T("wifi.password_for"), wifi_target);
    else if (kb_mode == KBM_GENRE_NEW || kb_mode == KBM_GENRE_RENAME) snprintf(prompt, sizeof prompt, "%s", T("genre_ui.prompt"));
    else snprintf(prompt, sizeof prompt, "%s", T(kb_mode == KBM_COLL ? "coll.name_prompt" : "kb.title"));
    if (!typed[0]) ui_text(f, prompt, tx, ty, t->text_dim, ALIGN_LEFT);
    if (fmod(UI.t, 1.0) < 0.55) ui_rect(tx + tw + 2, ty + M(0.004f), M(0.004f) + 1, TTF_FontHeight(f) - M(0.008f), t->accent);
    int ky = fy + field_h + M(0.02f);
    for (int r = 0; r < 5; r++, ky += key + gap) {
        int cols = r < 4 ? 10 : 3, kw = r < 4 ? key : (w - 2 * pad - 2 * gap) / 3;
        for (int c = 0; c < cols; c++) {
            int kx = x + pad + c * (kw + gap);
            bool sel = r == kb_row && c == kb_col;
            ui_rrect(kx, ky, kw, key, M(t->panel_radius), sel ? t->accent : with_alpha(t->text_dim, 45));
            char label[48];
            if (r < 4) snprintf(label, sizeof label, "%c", kb_rows[r][c]);
            else snprintf(label, sizeof label, "%s", T(c == KB_SPACE ? "kb.space" : c == KB_DEL ? "kb.delete" : "kb.done"));
            ui_text(f, label, kx + kw / 2, ky + (key - TTF_FontHeight(f)) / 2, sel ? t->accent_text : t->text, ALIGN_CENTER);
        }
    }
    char res[64];
    snprintf(res, sizeof res, T("kb.results"), nview);
    if (kb_mode == KBM_SEARCH) ui_text(UI.f_small, res, x + pad, ky - gap + M(0.008f), t->text_dim, ALIGN_LEFT);
    static const char *layout_keys[3] = { "kb.lower", "kb.symbols", "kb.upper" };
    const char *hint[] = { "X", T(layout_keys[kb_layout % 3]), "Y", T("kb.space"), "B", T("kb.delete"), "START", T("kb.done"), "A", T("kb.type") };
    ui_hints(hint, 10);
}

/* -------------------------------------------------- scaricamento */

static void scrape_input(void) {
    last_input = UI.t;   /* niente spegnimento automatico mentre lavora */
    const scrape_status_t *s = scraper_status();
    bool finished = s->phase == SCRAPE_DONE || s->phase == SCRAPE_FAILED;
    if (!finished && UI.in.pressed[BTN_B]) { scraper_cancel(); ui_sound(SND_BACK); }
    if (finished && (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B])) {
        scraper_join();
        scrape_open = false;
        ui_image_forget_missing();
        reload_games();   /* generi e giocatori nuovi */
        ui_sound(SND_OK);
    }
}

static void scrape_draw(void) {
    theme_t *t = &UI.th;
    const scrape_status_t *s = scraper_status();
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    int w = M(0.95f), h = M(0.42f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    if (t->surface.a < 200) ui_rrect(x, y, w, h, r, with_alpha(t->bg2, 230));   /* temi con superfici trasparenti */
    int pad = M(0.05f), ty = y + pad;
    ui_text(UI.f_title, T("scrape.title"), x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.03f);
    char line[200];
    const char *phase = s->phase == SCRAPE_INFO ? T("scrape.info") : s->phase == SCRAPE_IMAGES ? T("scrape.images")
                      : s->phase == SCRAPE_DONE ? T(s->cancel ? "scrape.stopped" : "scrape.done") : T("scrape.failed");
    ui_text(UI.f_normal, phase, x + pad, ty, t->text_dim, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_normal) + M(0.03f);
    /* barra di avanzamento */
    int bw = w - 2 * pad, bh = M(0.02f);
    float frac = s->total ? (float)s->done / s->total : (s->phase == SCRAPE_DONE ? 1.f : 0.f);
    ui_rrect(x + pad, ty, bw, bh, bh / 2, with_alpha(t->text_dim, 70));
    if (frac > 0) ui_rrect(x + pad, ty, (int)(bw * frac), bh, bh / 2, t->accent);
    ty += bh + M(0.03f);
    snprintf(line, sizeof line, T("scrape.progress"), s->done, s->total, s->saved, s->missing);
    ui_text(UI.f_small, line, x + pad, ty, t->text_dim, ALIGN_LEFT);
    bool finished = s->phase == SCRAPE_DONE || s->phase == SCRAPE_FAILED;
    int hb = M(t->hintbar_h);
    ui_rect(0, UI.h - hb, UI.w, hb, t->bg);   /* copre gli aiuti della schermata sotto */
    const char *hint[] = { finished ? "A" : "B", T(finished ? "common.ok" : "scrape.stop") };
    ui_hints(hint, 2);
}

/* ---------------------------------------------- salvaschermo, batteria */
/* Dopo il tempo scelto senza toccare i tasti: presentazione delle schermate dei giochi
   (zoom lento e dissolvenza) oppure schermo nero. Il primo tasto lo chiude e non fa
   altro. Sulla console lo schermo nero spegnerà anche la retroilluminazione. */

static bool saver_on;
static int saver_cur = -1, saver_prev = -1;
static double saver_t0;

static int saver_pick(void) {
    char p[1300];
    for (int tries = 0; tries < 40 && ngames; tries++) {
        int i = rand() % ngames;
        if (games[i].missing || games[i].hidden || i == saver_cur) continue;
        snprintf(p, sizeof p, "%s/Media/snap/%s.png", UI.root, games[i].name);
        if (ui_image(p)) return i;
    }
    return -1;
}

static void saver_check(double idle) {
    int kind = UI.set.saver % 5;
    double after = kind == 1 || kind == 3 ? 120 : 300;
    if (!saver_on && kind && idle > after && !scrape_open) {
        saver_on = true;
        saver_t0 = UI.t;
        saver_prev = -1;
        saver_cur = kind <= 2 ? saver_pick() : -1;
        ui_log("salvaschermo");
    }
}

static void saver_draw(void) {
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 255));
    if (UI.set.saver >= 3) return;   /* schermo spento */
    const double slide = 7, fadet = 1;
    double t = UI.t - saver_t0;
    if (t > slide) {
        saver_prev = saver_cur;
        saver_cur = saver_pick();
        saver_t0 = UI.t;
        t = 0;
    }
    char p[1300];
    for (int layer = 0; layer < 2; layer++) {
        int gi = layer == 0 ? saver_prev : saver_cur;
        if (gi < 0) continue;
        float a = layer == 0 ? (float)(1 - t / fadet) : (float)fmin(1.0, t / fadet);
        if (a <= 0) continue;
        snprintf(p, sizeof p, "%s/Media/snap/%s.png", UI.root, games[gi].name);
        SDL_Texture *img = ui_image(p);
        if (!img) continue;
        /* zoom lento che si sposta in diagonale (effetto "Ken Burns") */
        double k = (layer == 0 ? slide + t : t) / (slide + fadet);
        float z = 1.0f + 0.10f * (float)k;
        int w = (int)(UI.w * z), h = (int)(UI.h * z);
        int x = -(int)((w - UI.w) * (gi % 2 ? k : 1 - k)), y = -(int)((h - UI.h) * 0.5);
        SDL_SetTextureAlphaMod(img, (Uint8)(255 * a));
        ui_image_fill(img, x, y, w, h, 0);
        SDL_SetTextureAlphaMod(img, 255);
    }
    if (saver_cur >= 0) {
        game_t *g = &games[saver_cur];
        float a = (float)fmin(1.0, t / fadet);
        ui_vgradient(0, UI.h * 2 / 3, UI.w, UI.h / 3, rgba(0, 0, 0, 0), rgba(0, 0, 0, 200));
        char buf[200], info[300];
        ui_text_fit(UI.f_title, g->title, UI.w - 2 * M(0.08f), buf, sizeof buf);
        int ty = UI.h - M(0.08f) - TTF_FontHeight(UI.f_title) - TTF_FontHeight(UI.f_small);
        ui_text(UI.f_title, buf, M(0.08f), ty, rgba(255, 255, 255, (Uint8)(255 * a)), ALIGN_LEFT);
        snprintf(info, sizeof info, "%s%s%s", g->year, g->year[0] ? " · " : "", g->maker);
        ui_text(UI.f_small, info, M(0.08f), ty + TTF_FontHeight(UI.f_title), rgba(255, 255, 255, (Uint8)(190 * a)), ALIGN_LEFT);
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char clock_s[16];
    snprintf(clock_s, sizeof clock_s, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    ui_text(UI.f_big, clock_s, UI.w - M(0.08f), M(0.06f), rgba(255, 255, 255, 220), ALIGN_RIGHT);
}

/* avvisi: sotto il 15% una volta, sotto il 5% un'etichetta fissa finché non si mette in carica */
static void battery_check(void) {
    static double next_check;
    static bool warned_low;
    static int pct = 100;
    static bool charging;
    if (UI.t >= next_check) {
        pct = ui_battery(&charging);
        next_check = UI.t + 5;
        if (charging) warned_low = false;
        if (!charging && pct <= 15 && !warned_low) {
            char m[96];
            snprintf(m, sizeof m, T("battery.low"), pct);
            ui_toast(m);
            warned_low = true;
        }
    }
    if (!charging && pct <= 5 && !saver_on) {
        char m[128];
        snprintf(m, sizeof m, T("battery.critical"), pct);
        int w = ui_text_w(UI.f_small, m) + M(0.04f), h = TTF_FontHeight(UI.f_small) + M(0.015f);
        int x = (UI.w - w) / 2, y = UI.h - M(UI.th.hintbar_h) - h - M(0.015f);
        ui_rrect(x, y, w, h, h / 2, rgba(200, 40, 40, 235));
        ui_text(UI.f_small, m, UI.w / 2, y + M(0.0075f), rgba(255, 255, 255, 255), ALIGN_CENTER);
    }
}

/* ----------------------------------------------------------- main */

static bool consume_resume(void) {
    char a[1100], b[1100];
    snprintf(a, sizeof a, "%s/System/resume.txt", UI.root);
    snprintf(b, sizeof b, "%s/System/next.txt", UI.root);
    struct stat st;
    if (!UI.set.resume_last || stat(a, &st) != 0) return false;
    return rename(a, b) == 0;
}

int main(int argc, char **argv) {
    const char *root = "sdcard", *auto_script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_script = argv[++i];
    }
    char p[1100];
    snprintf(p, sizeof p, "%s/System", root);
    mkdir(p, 0755);
    if (!ui_init("Arcade", root, auto_script, 48000, true)) return 1;
    plat_cpu_fixed(CPU_ACTIVE_KHZ);
    ui_busy(T("common.loading"), 0);   /* subito qualcosa a schermo mentre si caricano i giochi */
    if (!auto_script && consume_resume()) {
        ui_log("riprendo l'ultima partita");
        plat_audio_release();
        ui_shutdown_code(EXIT_LAUNCH);
        return EXIT_LAUNCH;
    }
    ui_mark("schermata di caricamento");
    nthemes = theme_list(UI.root, theme_ids, theme_names, 16);
    nlangs = lang_list(UI.root, lang_codes, lang_names, 16);
    nfilters = filters_list(UI.root, UI.set.lang, filters, 24);
    reload_games();
    ui_mark("giochi pronti");
    music_init(UI.root);
    ui_mark("musica");
    screen = is_list_layout() ? SCR_LIBRARY : SCR_HOME;
    screen_t0 = -1;
    diag_check_failure();
    if (!UI.set.setup_done) setup_step = 1;
    bool first_frame = true, idle_logged = false;
    unsigned idle_skip = 0;

    while (!UI.in.quit) {
        ui_poll();
        music_update();
        if (UI.in.any) last_input = UI.t;
        /* salvaschermo: il tasto che lo chiude non arriva al resto del launcher */
        if (saver_on && UI.in.any) {
            saver_on = false;
            memset(UI.in.pressed, 0, sizeof UI.in.pressed);
        }
        saver_check(UI.t - last_input);
        if (UI.set.autosleep_min && UI.t - last_input > UI.set.autosleep_min * 60.0) {
            ui_log("spegnimento automatico");
            power_off();
        }
        /* tasto di accensione (sistema su SD): finestra per spegnere */
        /* tasto di accensione (sistema su SD): breve sospende, tenuto premuto chiede se spegnere */
        if (UI.in.power && !upd_busy() && !(dlg_open && dlg_suspend)) {
            dlg_open = dlg_suspend = true;
            dlg_sel = 1;
            saver_on = false;
            last_input = UI.t;
            UI.in.power = false;   /* questa pressione ha solo aperto la finestra */
            ui_sound(SND_OK);
        }
        if (UI.in.power_long && (!dlg_open || dlg_suspend) && !upd_open) { dlg_open = true; dlg_suspend = false; dlg_sel = dlg_buttons() - 1; ui_sound(SND_OK); }
        /* HOME (tasto laterale della Brick Pro): chiude finestre e torna alla schermata iniziale */
        if (UI.in.home && !romcheck_open && !scrape_open && !upd_open && !setup_step && !ptest_open) {
            dlg_suspend = false;
            reset_step = 0;
            cfm_kind = CFM_NONE;
            diag_open = kb_open = fpanel_open = dlg_open = gopt_open = clock_open = sdinfo_open = wpanel_open = clean_open = gp_open = false;
            go(is_list_layout() ? SCR_LIBRARY : SCR_HOME);
            ui_sound(SND_BACK);
        }
        /* il layout può cambiare col tema: la home a carosello non esiste nel layout a elenco */
        if (is_list_layout() && screen == SCR_HOME) screen = SCR_LIBRARY;

        /* controllo degli aggiornamenti: una volta per avvio, appena il Wi-Fi è collegato */
        static bool notify_started;
        if (!notify_started && UI.set.update_notify && update_supported() && !upd_open && wifi_status(false)->state == WIFI_CONNECTED) {
            notify_started = true;
            update_notify_start(UI.root, UI.set.update_channel);
        }
        if (dlg_open && dlg_suspend) {
            int r = ui_confirm_input(&dlg_sel);
            if (r >= 0) dlg_open = dlg_suspend = false;
            if (r == 1) { ui_suspend(); last_input = UI.t; }
        }
        else if (cfm_kind) cfm_input();
        else if (setup_step) setup_input();
        else if (ptest_open) ptest_input();
        else if (reset_step) reset_input();
        else if (diag_open) diag_input();
        else if (romcheck_open) romcheck_input();
        else if (scrape_open) scrape_input();
        else if (upd_open) upd_input();
        else if (kb_open) keyboard_input();
        else if (gp_open) gp_input();
        else if (gopt_open && screen == SCR_DETAIL) gopt_input();
        else if (fpanel_open) fpanel_input();
        else if (dlg_open) dialog_input();
        else if (clock_open) clock_input();
        else if (wpanel_open) wpanel_input();
        else if (clean_open) clean_input();
        else if (sdinfo_open) sdinfo_input();
        else if (screen == SCR_HOME && is_bar_layout()) bar_input();
        else if (screen == SCR_HOME) home_input();
        else if (screen == SCR_LIBRARY) lib_common_input(is_list_layout() ? 1 : grid_cols());
        else if (screen == SCR_DETAIL) detail_input();
        else settings_input();

        /* Fermo sullo schermo da qualche secondo: CPU bassa e meno disegni, la console scalda
           meno. Basta un tasto per tornare subito come prima; i lavori lunghi non rallentano. */
        bool held = false;
        for (int b = 0; b < BTN_COUNT; b++) held = held || UI.in.down[b];
        if (held) last_input = UI.t;
        upd_audio();
        /* l'uscita audio è aperta e assestata: ora l'amplificatore si può accendere */
        if (UI.t > 0.4) plat_audio_release();
        bool working = romcheck_open || scrape_open || upd_open || clean_open || wpanel_open || kb_open;
        bool idle = !working && !UI.automation && UI.t - last_input > IDLE_AFTER_S;
        plat_cpu_fixed(idle ? CPU_IDLE_KHZ : CPU_ACTIVE_KHZ);
        if (idle && !idle_logged && UI.t - last_input > 120) {
            int mhz, temp;
            idle_logged = true;
            plat_cpu_status(&mhz, &temp);
            ui_log("fermo da 2 minuti: CPU %d MHz, %d °C", mhz, temp);
        }
        if (idle && UI.set.autosuspend_min && plat_standalone() && UI.t - last_input > UI.set.autosuspend_min * 60.0) {
            ui_log("sospensione automatica");
            ui_suspend();
            last_input = UI.t;
            saver_on = false;
            continue;
        }
        plat_screen_off(saver_on && UI.set.saver >= 3);
        /* il salvaschermo con le immagini resta fluido; a schermo spento non serve disegnare */
        if (idle && (!saver_on || UI.set.saver >= 3) && ++idle_skip % 3) {
            SDL_Delay(16);
            continue;
        }

        ui_begin();
        if (screen == SCR_HOME && is_bar_layout()) bar_draw();
        else if (screen == SCR_HOME) home_draw();
        else if (screen == SCR_LIBRARY) {
            if (is_list_layout()) library_list_draw();
            else library_grid_draw();
        } else if (screen == SCR_DETAIL) detail_draw();
        else settings_draw();
        if (dlg_open && !dlg_suspend) dialog_draw();
        if (clock_open) clock_draw();
        if (wpanel_open) wpanel_draw();
        if (clean_open) clean_draw();
        if (sdinfo_open) sdinfo_draw();
        if (fpanel_open) fpanel_draw();
        if (gopt_open && screen == SCR_DETAIL) gopt_draw();
        if (gp_open) gp_draw();
        if (kb_open) keyboard_draw();
        if (scrape_open) scrape_draw();
        if (upd_open) upd_draw();
        if (romcheck_open) romcheck_draw();
        if (diag_open) diag_draw();
        if (reset_step) reset_draw();
        if (cfm_kind) cfm_draw();
        if (ptest_open) ptest_draw();
        if (setup_step) setup_draw();
        if (dlg_open && dlg_suspend) ui_confirm_draw(T("dialog.suspend_title"), T("dialog.suspend_body"), T("common.cancel"), T("dialog.suspend"), dlg_sel);
        battery_check();
        if (first_frame) { first_frame = false; ui_mark("prima schermata"); update_confirm_boot(UI.root); }
        /* avviso di aggiornamento, solo nella schermata iniziale */
        if (update_notify_version()[0] && !saver_on && (screen == SCR_HOME || (is_list_layout() && screen == SCR_LIBRARY)) && !dlg_open) {
            const char *msg = T("update.notice");
            int pad = M(0.02f), tw = ui_text_w(UI.f_small, msg), th = TTF_FontHeight(UI.f_small);
            int bw = tw + pad * 2, bh = th + M(0.012f), bx = (UI.w - bw) / 2, by = M(0.022f);
            ui_rrect(bx, by, bw, bh, bh / 2, UI.th.accent);
            ui_text(UI.f_small, msg, UI.w / 2, by + M(0.006f), UI.th.accent_text, ALIGN_CENTER);
        }
        if (saver_on) saver_draw();
        float fade = screen_t0 < 0 ? 0 : 1 - (float)((UI.t - screen_t0) / 0.18);
        if (fade > 0) ui_rect(0, 0, UI.w, UI.h, with_alpha(UI.th.bg, (int)(255 * fade)));
        ui_end();
    }
    settings_save(&UI.set, UI.root);
    music_quit();
    ui_shutdown_code(EXIT_QUIT);
    return EXIT_QUIT;
}

/* i tasti di ui.h (BTN_A, BTN_LEFT...) non devono essere coperti da <linux/input.h> */
#ifdef BTN_TRIGGER_HAPPY
#error "linux/input.h incluso: coprirebbe i tasti di ui.h"
#endif
