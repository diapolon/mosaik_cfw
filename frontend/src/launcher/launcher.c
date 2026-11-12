/* Launcher: home, libreria, impostazioni. Esce con 10 per avviare un gioco
   (descritto in System/next.txt), 20 per spegnere, 0 per chiudere. */
#include "../common/ui.h"
#include "../common/json.h"
#include "../common/filters.h"
#include "savesplit.h"
#include "games.h"
#include "scraper.h"
#include "romcheck.h"
#include "music.h"
#include "../common/platform.h"
#include "../common/wifi.h"
#include "update.h"
#include "stickcal.h"
#include "fileserver.h"
#include "store.h"
#include "qrcode.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <limits.h>
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
enum { SCR_HOME, SCR_LIBRARY, SCR_SETTINGS, SCR_DETAIL, SCR_PLATFORMS, SCR_APPS, SCR_STORE };
static int detail_idx = -1;   /* gioco mostrato nella scheda */
static void open_detail(int idx);
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
static int icon_sel;
static bool ring_init;

/* libreria */
/* schede: tutti, un emulatore ciascuno (TAB_EMU + indice), Linux, preferiti */
enum { TAB_ALL, TAB_LINUX, TAB_FAV, TAB_EMU = 100, TAB_COLL = 200, TAB_PLAN = 300 };
static int tabs[MAX_EMUS + 24], ntabs, tab_sel;   /* Tutti, Preferiti, emulatori, Linux, collezioni */
/* schermata Piattaforme: le stesse schede, poi le console in arrivo (TAB_PLAN + indice in
   PLANNED), in grigio e senza libreria. Le prime ntabs coincidono con tabs[]. */
static int ptabs[MAX_EMUS + 40], nptabs;
static bool is_plan(int t) { return t >= TAB_PLAN; }
static int plat_sel;   /* categoria scelta nella schermata Piattaforme */
static int *view, nview, lib_sel;
static float lib_scroll, lib_scroll_target;

/* impostazioni */
static int set_sel;              /* voce scelta (S_*) */
static int set_page = -1;        /* -1: elenco delle categorie; altrimenti la categoria aperta */
static int set_row;              /* riga scelta nella pagina */
static float set_scroll, set_hl;
static void settings_open(int page, int row);
static void sto_start(void);
static void draw_switch(int x, int y, int h, bool on);
static void plat_open(int sel);
static void quit_to_menu(void);
enum { SC_DISPLAY, SC_LOOK, SC_GAMES, SC_CONTROLS, SC_SYSTEM, SC_TOOLS, SC_CARD, SC_UPDATES, SC_COUNT };
static char theme_ids[16][64], theme_names[16][64];
static int nthemes;

/* dialogo spegnimento */
static bool dlg_open, dlg_suspend;   /* dlg_suspend: la finestra chiede di sospendere, non di spegnere */

/* scaricamento immagini e verifica ROM in corso (finestre di avanzamento) */
static bool scrape_open, romcheck_open, rcq_open;   /* rcq: scelta fra verifica rapida e completa */
static int rcq_sel;
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
enum { KBM_SEARCH, KBM_COLL, KBM_COLL_RENAME, KBM_WIFI, KBM_GENRE_NEW, KBM_GENRE_RENAME };
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
static bool is_bar_layout(void) { return !strcmp(UI.th.layout, "bar"); }
/* vetrina: illustrazione grande della voce scelta e striscia delle piattaforme in basso */
static bool is_showcase_layout(void) { return !strcmp(UI.th.layout, "vetrina"); }
static void vt_draw(void);
static void vt_input(void);
/* galleria: schede Preferiti/Recenti/Piattaforme in alto e un carosello di schede con produttore e anno */
static bool is_gallery_layout(void) { return !strcmp(UI.th.layout, "galleria"); }
static void gl_draw(void);
static void gl_input(void);

static void go(int scr) {
    if (scr == screen) return;
    back_screen = screen;
    screen = scr;
    screen_t0 = UI.t;
    ring_init = false;
}

/* ------------------------------------------------------------ dati */

/* Giochi doppi: lo stesso nome ROM nel set FBNeo e in quello MAME. Secondo
   l'impostazione si mostrano entrambi o solo quello del core preferito. Solo fra gli arcade:
   un gioco NES e uno Mega Drive con lo stesso nome file sono giochi diversi. */
static bool core_console(const char *core) {
    const emulator_t *e = emulator_find(core);
    return e && e->console;
}

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
                if (strcmp(games[ix[k]].core, keep) && strcmp(games[ix[k]].core, "linux") && !core_console(games[ix[k]].core))
                    games[ix[k]].hidden = true;
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


/* cartella dei salvataggi di un gioco: Saves/<core senza _libretro> per gli arcade,
   Saves/<console> per le console (due console possono usare lo stesso core, issue #5) */
static void save_base(const game_t *g, char *out, size_t n, char *core, size_t cn) {
    const emulator_t *emu = emulator_find(g->core);
    snprintf(core, cn, "%s", emu ? emu->core : g->core);
    char *us = strstr(core, "_libretro");
    if (us) *us = 0;
    snprintf(out, n, "%s/Saves/%s", UI.root, emu && emu->console ? g->core : core);
}

static bool game_autosave(int gi, time_t *when);

/* Partita da riprendere: l'ultimo gioco avviato, se ha un salvataggio automatico. Nella home
   a carosello è la prima scheda, con l'immagine del punto in cui si era rimasti. */
static int resume_idx = -1;
static time_t resume_when;
static char resume_png[1400];
static int resume_rot;   /* quarti di giro dell'anteprima, come il gioco a schermo */

static void find_resume(void) {
    resume_idx = -1;
    for (int i = 0; i < ngames; i++) {
        const game_t *g = &games[i];
        if (g->recent != 1 || g->missing || !strcmp(g->core, "linux")) continue;
        char base[1200], core[80], st[1300];
        struct stat sb;
        save_base(g, base, sizeof base, core, sizeof core);
        snprintf(st, sizeof st, "%s/%s.auto.state", base, g->key);
        if (stat(st, &sb) != 0 || sb.st_size <= 0) return;
        resume_idx = i;
        resume_when = sb.st_mtime;
        snprintf(resume_png, sizeof resume_png, "%s.png", st);
        char rp[1400];
        snprintf(rp, sizeof rp, "%s.rot", st);
        FILE *rf = fopen(rp, "r");
        resume_rot = 0;
        if (rf) { if (fscanf(rf, "%d", &resume_rot) != 1) resume_rot = 0; fclose(rf); }
        resume_rot &= 3;
        return;
    }
}

/* Home: la partita da riprendere e gli accessi a Libreria, Preferiti, Temi, Impostazioni e
   Spegni. Le voci del menu (Cabinato, Onda) e i giochi del carosello (Tessera). */
enum { HK_RESUME, HK_LAST, HK_FAV, HK_RECENT };
static int hgame[16], hkind[16], nhgames;     /* carosello: giochi e perché ci sono */
enum { HM_GAME, HM_LIBRARY, HM_FAV, HM_APPS, HM_STORE, HM_SETTINGS, HM_POWER };   /* i temi si scelgono dalle impostazioni */
static int hmenu[8], nhmenu;                  /* menu: la prima voce è il gioco, se c'è */
/* App: programmi che non sono giochi, in Arcade/Apps/<app>/launch.sh (app.json e icon.png
   facoltativi). Partono come i giochi Linux, ma non vanno in libreria, negli ultimi giochi né
   nel tempo di gioco; la voce "App" nella schermata iniziale c'è solo se ne esiste almeno una. */
typedef struct { char name[64], title[96], desc[240], author[64], path[1100], icon[1100]; } app_t;
static app_t *apps;
static int napps, app_sel;
static float app_scroll;
static int hsel, hrow;                        /* scelta nel carosello o nel menu; riga (Tessera) */
static int hbtn;                              /* pulsante scelto nella fila (Tessera) */

static void build_home_rows(void) {
    nhgames = 0;
    int last = -1;
    for (int i = 0; i < ngames && last < 0; i++)
        if (games[i].recent == 1 && !games[i].hidden && !games[i].missing) last = i;
    if (last >= 0) { hgame[0] = last; hkind[0] = last == resume_idx ? HK_RESUME : HK_LAST; nhgames = 1; }
    for (int i = 0; i < ngames && nhgames < 12; i++)
        if (games[i].fav && !games[i].hidden && !games[i].missing && i != last) { hgame[nhgames] = i; hkind[nhgames++] = HK_FAV; }
    for (int r = 2; r <= 20 && nhgames < 6; r++)
        for (int i = 0; i < ngames; i++)
            if (games[i].recent == r && !games[i].hidden && !games[i].missing && !games[i].fav) { hgame[nhgames] = i; hkind[nhgames++] = HK_RECENT; break; }
    nhmenu = 0;
    if (last >= 0) hmenu[nhmenu++] = HM_GAME;
    for (int m = HM_LIBRARY; m <= HM_POWER; m++)
        if (m != HM_APPS || napps) hmenu[nhmenu++] = m;
    if (hsel >= (is_list_layout() || is_bar_layout() ? nhmenu : nhgames)) hsel = 0;
}

static void build_carousel(void) {
    find_resume();
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
    build_home_rows();
}

static const char *tab_label(int t) {
    if (is_plan(t)) return PLANNED[t - TAB_PLAN].name;
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
    tabs[ntabs++] = TAB_FAV;   /* subito dopo "Tutti": in fondo, dopo le console, si perdeva */
    int cand[MAX_EMUS + 2], nc = 0;
    for (int e = 0; e < NEMUS && nc < MAX_EMUS; e++) cand[nc++] = TAB_EMU + e;
    cand[nc++] = TAB_LINUX;
    for (int c = 0; c < nc; c++) {
        bool has = false;
        for (int i = 0; i < ngames && !has; i++) has = in_tab(&games[i], cand[c]);
        if (has) tabs[ntabs++] = cand[c];
    }
    for (int c = 0; c < NCOLLS && ntabs < (int)(sizeof tabs / sizeof *tabs); c++) {
        bool has = false;
        for (int i = 0; i < ngames && !has; i++) has = in_tab(&games[i], TAB_COLL + c);
        if (has) tabs[ntabs++] = TAB_COLL + c;
    }
    if (tab_sel >= ntabs) tab_sel = 0;
    nptabs = 0;
    for (int i = 0; i < ntabs; i++) ptabs[nptabs++] = tabs[i];
    for (int i = 0; i < NPLANNED && nptabs < (int)(sizeof ptabs / sizeof *ptabs); i++) ptabs[nptabs++] = TAB_PLAN + i;
}

static bool rom_problem(const game_t *g) {
    return g->romstatus == RC_MISSING || g->romstatus == RC_BADCRC || g->romstatus == RC_NOPARENT || g->romstatus == RC_DAMAGED;
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

static int app_cmp(const void *a, const void *b) { return strcasecmp(((const app_t *)a)->title, ((const app_t *)b)->title); }

static void apps_scan(void) {
    free(apps);
    apps = NULL;
    napps = 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Apps", UI.root);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sh[1200], meta[1200];
        struct stat sb;
        snprintf(sh, sizeof sh, "%s/%s/launch.sh", dir, e->d_name);
        if (stat(sh, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
        app_t *na = realloc(apps, sizeof(app_t) * (size_t)(napps + 1));
        if (!na) break;
        apps = na;
        app_t *a = &apps[napps++];
        memset(a, 0, sizeof *a);
        snprintf(a->name, sizeof a->name, "%s", e->d_name);
        snprintf(a->path, sizeof a->path, "%s", sh);
        snprintf(meta, sizeof meta, "%s/%s/app.json", dir, e->d_name);
        jval *j = json_load(meta);
        /* nome e descrizione nella lingua della console, se l'app li ha ("texts": {"it": {...}}) */
        char k[64];
        snprintf(k, sizeof k, "texts.%s.title", UI.set.lang);
        snprintf(a->title, sizeof a->title, "%s", json_str(j, k, json_str(j, "texts.en.title", json_str(j, "title", e->d_name))));
        snprintf(k, sizeof k, "texts.%s.description", UI.set.lang);
        snprintf(a->desc, sizeof a->desc, "%s", json_str(j, k, json_str(j, "texts.en.description", json_str(j, "description", ""))));
        snprintf(a->author, sizeof a->author, "%s", json_str(j, "author", ""));
        json_free(j);
        snprintf(a->icon, sizeof a->icon, "%s/%s/icon.png", dir, e->d_name);
        if (stat(a->icon, &sb) != 0) a->icon[0] = 0;
    }
    closedir(d);
    if (napps > 1) qsort(apps, (size_t)napps, sizeof(app_t), app_cmp);
    if (app_sel >= napps) app_sel = napps ? napps - 1 : 0;
    ui_log("app trovate: %d", napps);
}

static void reload_games(void) {
    apps_scan();
    games_free(games, ngames);
    emulators_load(UI.root);
    ngames = games_scan(UI.root, &games, UI.set.show_missing);
    ui_mark("lettura dei giochi");
    /* salvataggi delle console da Saves/<core> a Saves/<console> (issue #5): una volta per
       avvio, prima di cercare la partita da riprendere e gli slot */
    static bool split_done;
    if (!split_done) { saves_split(UI.root, games, ngames); split_done = true; }
    games_mark_lists(UI.root, games, ngames);
    romcheck_load(UI.root, games, ngames);
    mark_duplicates();
    build_genres();
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
    snprintf(meta, sizeof meta, "%.*s/%s", (int)(strrchr(g->path, '/') - g->path), g->path, !strcmp(g->core, "app") ? "app.json" : "game.json");
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
/* "Reimposta impostazioni" e "Cancella tutto" (più avanti): stato del lavoro in sottofondo */
enum { RST_NONE, RST_RUN, RST_DONE, RST_FAILED };
static atomic_int rst_state;

static void quiet_begin(void) {
    if (audio_quiet) return;
    audio_quiet = true;
    ui_log("altoparlante spento (lavoro su SD o Wi-Fi)");
    plat_audio_mute();
}

/* conferme delle azioni che fanno perdere qualcosa (Annulla già selezionato) */
enum { CFM_NONE, CFM_FORGET, CFM_GENRES, CFM_COLL, CFM_STICKS, CFM_RESTART, CFM_RESETSET, CFM_RESETSET2, CFM_ERASE, CFM_ERASE2, CFM_ERASE3, CFM_STORE, CFM_PKG, CFM_RESTPKG };
static double cfm_hold;
/* pacchetti dello Store copiati sulla scheda (domanda all'avvio), e invito allo Store senza emulatori */
static bool pkg_checked, store_asked;
static char pkg_path[1300], pkg_name[64], pkg_label[32];   /* ultima conferma di "Cancella tutto": A tenuto premuto da quando */
#define CFM_HOLD_S 3.0
static int cfm_kind, cfm_sel;
static char cfm_ssid[33];

static bool have_games(void) {
    for (int i = 0; i < ngames; i++)
        if (!games[i].missing) return true;
    return false;
}

static void launch_ex(game_t *g, bool resume) {
    char path[1100], core[1100];
    if (g->missing) {
        const emulator_t *emu = emulator_find(g->core);
        char msg[300];
        snprintf(msg, sizeof msg, T("missing.toast"), g->name, emu ? emu->roms : "Roms");
        ui_toast(msg);
        ui_sound(SND_ERROR);
        return;
    }
    bool sh = !strcmp(g->core, "linux") || !strcmp(g->core, "app");   /* script: giochi Linux e app */
    /* emulatore a sé (non libretro): parte come un gioco Linux, il suo script riceve il gioco */
    const emulator_t *xemu = sh ? NULL : emulator_find(g->core);
    char xscript[1100] = "";
    if (xemu && xemu->launch[0]) {
        snprintf(xscript, sizeof xscript, "%s/%s", UI.root, xemu->launch);
        if (access(xscript, R_OK) != 0) {
            ui_log("emulatore %s: manca %s", xemu->id, xscript);
            ui_toast(T("common.launch_error"));
            ui_sound(SND_ERROR);
            return;
        }
    }
    if (strcmp(g->core, "app")) games_add_recent(UI.root, g);          /* le app non sono giochi */
    if (sh) write_linux_env(g);
    if (xscript[0]) {
        char envp[1100];
        snprintf(envp, sizeof envp, "%s/System/next.env", UI.root);
        FILE *ef = fopen(envp, "w");
        if (ef) {   /* fra apici singoli: un apice nel nome diventa '\'' */
            fputs("export ARCADE_ROM='", ef);
            for (const char *c = g->path; *c; c++) { if (*c == '\'') fputs("'\\''", ef); else fputc(*c, ef); }
            fputs("'\n", ef);
            if (xemu->nomenu) fputs("export GAMEWATCH_NOMENU=1\n", ef);
            fclose(ef);
        }
    }
    if (sh || xscript[0]) snprintf(core, sizeof core, "-");
    else {
        const emulator_t *emu = emulator_find(g->core);
        snprintf(core, sizeof core, "%s/Cores/%s.%s", UI.root, emu ? emu->core : g->core, CORE_EXT);
    }
    snprintf(path, sizeof path, "%s/System/next.txt", UI.root);
    FILE *f = fopen(path, "w");
    bool written = f != NULL;
    if (f) {
        /* ultimo campo: la console, per i salvataggi in Saves/<console> (issue #5); "-" per gli arcade */
        /* sesto campo: la console ("-" per gli arcade); settimo: l'identità del gioco per i suoi
           file, diversa dal nome solo per due ROM omonime (audit prelancio R04) */
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", sh || xscript[0] ? "linux" : "libretro", core, xscript[0] ? xscript : g->path, g->title,
                resume ? "resume" : "-", core_console(g->core) ? g->core : "-", g->key[0] ? g->key : "-");
        written = !ferror(f) && fflush(f) == 0;
        if (fclose(f) != 0) written = false;
    }
    if (!written) {
        /* un file a metà non deve avviare qualcos'altro: si resta nel menu */
        ui_log("impossibile scrivere %s: %s", path, strerror(errno));
        unlink(path);
        ui_toast(T("common.launch_error"));
        ui_sound(SND_ERROR);
        return;
    }
    ui_log("avvio %s (%s)", g->title, g->core);
    ui_sound(SND_OK);
    char msg[200];
    snprintf(msg, sizeof msg, T("common.starting"), g->title);
    ui_busy(msg, 0.35);
    music_quit();
    ui_shutdown_code(EXIT_LAUNCH);
    exit(EXIT_LAUNCH);
}

static void launch(game_t *g) { launch_ex(g, false); }

static void leave_system(int code, const char *msg) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/resume.txt", UI.root);
    unlink(p);   /* spento dal menu: al riavvio non c'è una partita da riprendere */
    settings_save(&UI.set, UI.root);
    ui_busy(msg, 0.5);
    music_quit();
    /* Riavvio, o uscita dopo un lavoro con l'altoparlante spento (ripristino, Cancella tutto):
       la chiusura dell'audio lì si blocca e system-init ci fermava dopo 10 s. Tanto si riparte:
       si esce subito, come dopo un aggiornamento. */
    if (plat_is_device() && (code == EXIT_REBOOT || audio_quiet)) {
        plat_amp_restore();
        ui_exit_now(code);
    }
    ui_shutdown_code(code);
    exit(code);
}

/* Col caricatore collegato U-Boot del firmware, ripartendo, entra nella sua modalità di sola
   carica e il sistema non parte (schermo nero, serve il reset forzato): provato sulla Smart Pro.
   Al posto del riavvio la console si spegne e si riaccende con il tasto. system-init fa lo
   stesso controllo, per chi arriva al riavvio da un'altra strada. */
static bool on_charger(void) {
    bool c = false;
    ui_battery(&c);   /* TRIMUI_FAKE_CHARGING lo simula sul Mac */
    return c;
}

static void power_off(void) { leave_system(EXIT_POWEROFF, T("common.shutting_down")); }
static void restart_system(void) {
    if (on_charger()) {
        ui_log("riavvio con il caricatore collegato: la console si spegne");
        settings_save(&UI.set, UI.root);
        ui_busy(T("common.restart_charger"), 4);
        leave_system(EXIT_POWEROFF, T("common.shutting_down"));
    }
    leave_system(EXIT_REBOOT, T("common.restarting"));
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

/* Immagine del gioco: per le console la scatola (Media/box) se l'impostazione è accesa,
   poi Media/<tipo>/<rom>.png nei tipi scelti dal tema (snap, title, marquee, flyer), poi
   Media/<core>/<rom>.png messa a mano; per Linux cover.png. *box dice se è la scatola, che
   va mostrata intera (è verticale) e non ritagliata come le schermate. */
/* Media/<tipo>/<console>/<rom>.png per le console (issue #5: due console possono avere un gioco
   con lo stesso nome), poi Media/<tipo>/<rom>.png, dove le scaricavano le versioni prima della
   0.9.18 e dove stanno quelle degli arcade */
static SDL_Texture *media_typed(const game_t *g, const char *type, bool console) {
    char p[1400];
    if (console) {
        snprintf(p, sizeof p, "%s/Media/%s/%s/%s.png", UI.root, type, g->core, g->name);
        SDL_Texture *img = ui_image(p);
        if (img) return img;
    }
    snprintf(p, sizeof p, "%s/Media/%s/%s.png", UI.root, type, g->name);
    return ui_image(p);
}

static SDL_Texture *media_image_ex(const game_t *g, char types[][16], int n, bool *box) {
    char p[1400];
    if (box) *box = false;
    if (!strcmp(g->core, "linux")) {
        snprintf(p, sizeof p, "%.*s/cover.png", (int)(strrchr(g->path, '/') - g->path), g->path);
        return ui_image(p);
    }
    bool console = core_console(g->core);
    if (UI.set.box_covers && console) {
        SDL_Texture *img = media_typed(g, "box", true);
        if (img) { if (box) *box = true; return img; }
    }
    for (int i = 0; i < n; i++) {
        SDL_Texture *img = media_typed(g, types[i], console);
        if (img) return img;
    }
    snprintf(p, sizeof p, "%s/Media/%s/%s.png", UI.root, g->core, g->name);
    return ui_image(p);
}

static SDL_Texture *media_image(const game_t *g, char types[][16], int n) { return media_image_ex(g, types, n, NULL); }

/* Copertina: l'immagine scaricata se c'è, altrimenti una generata con colore per sistema e titolo. */
static void draw_cover(const game_t *g, int x, int y, int w, int s, int r) {
    bool box;
    SDL_Texture *img = media_image_ex(g, UI.th.cover_types, UI.th.ncover, &box);
    if (img && (UI.th.cover_fit || box)) {
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

enum { IC_ALL, IC_FAV, IC_SETTINGS, IC_POWER, IC_APPS, IC_STORE, IC_COUNT };

/* ----------------------------------------------------------- home */

static void home_activate_icon(void) {
    switch (icon_sel) {
    case IC_ALL: plat_open(tab_sel); break;   /* prima le piattaforme, poi i giochi */
    case IC_FAV:
        for (int i = 0; i < ntabs; i++)
            if (tabs[i] == TAB_FAV) tab_sel = i;
        build_view();
        go(SCR_LIBRARY);
        break;
    case IC_SETTINGS: settings_open(-1, 0); go(SCR_SETTINGS); break;
    case IC_POWER: dlg_open = true; dlg_suspend = false; dlg_sel = 0; break;
    case IC_APPS: go(SCR_APPS); break;
    case IC_STORE: sto_start(); break;
    }
    ui_sound(SND_OK);
}

static bool matches_search(const game_t *g);

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
    if (in->pressed[BTN_A] && nview) {
        if (game_autosave(view[lib_sel], NULL)) launch_ex(&games[view[lib_sel]], true);
        else launch(&games[view[lib_sel]]);
    }
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
    if (in->pressed[BTN_B]) { plat_open(tab_sel); ui_sound(SND_BACK); }
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

/* ------------------------------------------------------------------ libreria */
/* Struttura comune ai temi (tavole di docs/mockup/revisione): barra in alto col nome del tema,
   categorie a pulsanti, "LIBRERIA · N TITOLI", contenuto, nota su START e barra dei tasti su
   due righe. Il contenuto cambia col tema: schede affiancate (Tessera, layout "carousel"),
   elenco con anteprima (Cabinato, "list"), anteprima grande con riquadri sotto (Onda, "bar"). */

enum { LS_CARDS, LS_LIST, LS_SPOT };
static int lib_style(void) { return is_list_layout() ? LS_LIST : is_bar_layout() ? LS_SPOT : LS_CARDS; }

static void format_duration(long secs, char *out, size_t n);

/* salvataggio automatico del gioco (per "Riprendi"): si rilegge solo quando cambia gioco */
static bool game_autosave(int gi, time_t *when) {
    static int last = -2;
    static bool has;
    static time_t at;
    if (gi != last) {
        last = gi;
        has = false;
        const game_t *g = &games[gi];
        if (strcmp(g->core, "linux") && !g->missing) {
            char base[1200], core[80], st[1300];
            struct stat sb;
            save_base(g, base, sizeof base, core, sizeof core);
            snprintf(st, sizeof st, "%s/%s.auto.state", base, g->key);
            if (stat(st, &sb) == 0 && sb.st_size > 0) { has = true; at = sb.st_mtime; }
        }
    }
    if (when) *when = at;
    return has;
}

/* copertina, o il riquadro "Senza immagine" che tiene il posto */
static void cover_box(const game_t *g, int x, int y, int w, int h, int r) {
    theme_t *t = &UI.th;
    if (media_image(g, t->cover_types, t->ncover)) { draw_cover(g, x, y, w, h, r); return; }
    ui_rrect(x, y, w, h, r, with_alpha(t->text, 28));
    char fit[80];
    ui_text_fit(UI.f_small, T("lib.no_image"), w - M(0.02f), fit, sizeof fit);
    ui_text(UI.f_small, fit, x + w / 2, y + h / 2 - TTF_FontHeight(UI.f_small) / 2, t->text_dim, ALIGN_CENTER);
}

/* riquadro con la sola immagine; senza immagine, il nome del gioco al centro */
static void cover_tile(const game_t *g, int x, int y, int w, int h, int r) {
    theme_t *t = &UI.th;
    if (media_image(g, t->cover_types, t->ncover)) { draw_cover(g, x, y, w, h, r); return; }
    ui_rrect(x, y, w, h, r, t->surface);
    ui_rborder(x, y, w, h, r, 2, t->line);
    char lines[3][160];
    TTF_Font *f = UI.f_small;
    int n = ui_wrap(f, g->title, w - M(0.03f), lines, 3), fh = TTF_FontHeight(f), ty = y + (h - n * fh) / 2;
    for (int k = 0; k < n; k++) ui_text(f, lines[k], x + w / 2, ty + k * fh, t->text_dim, ALIGN_CENTER);
}

/* in alto: titolo dell'interfaccia e, accanto, il nome del tema */
static void topbar_label(const char *label);
static void topbar_theme(void) { topbar_label(UI.th.name); }
static void topbar_label(const char *label) {
    theme_t *t = &UI.th;
    ui_topbar(t->title);
    char tt[128];
    snprintf(tt, sizeof tt, "%s", t->title);
    if (t->title_upper)
        for (char *p = tt; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
    int x = M(t->margin) + ui_text_w(UI.f_title, tt) + M(0.035f), cy = M(t->topbar_h) / 2;
    ui_text(UI.f_normal, label, x, cy - TTF_FontHeight(UI.f_normal) / 2, t->text_dim, ALIGN_LEFT);
    ui_rect(M(t->margin), M(t->topbar_h) - 1, UI.w - 2 * M(t->margin), 1, t->line);
}

/* categorie a pulsanti; quella scelta piena. Se non stanno tutte, scorrono. */
static int pills(int y) {
    theme_t *t = &UI.th;
    TTF_Font *f = UI.f_normal;
    int mx = M(t->margin), fh = TTF_FontHeight(f), h = fh + M(0.022f), pad = M(0.022f), gap = M(0.016f);
    static float off, off_target;
    int xs[MAX_EMUS + 24], ws[MAX_EMUS + 24], tot = 0;
    for (int i = 0; i < ntabs; i++) {
        ws[i] = ui_text_w(f, tab_label(tabs[i])) + 2 * pad;
        xs[i] = tot;
        tot += ws[i] + gap;
    }
    int avail = UI.w - 2 * mx;
    if (ntabs) {
        if (xs[tab_sel] + ws[tab_sel] - off_target > avail) off_target = (float)(xs[tab_sel] + ws[tab_sel] - avail);
        if (xs[tab_sel] < off_target) off_target = (float)xs[tab_sel];
    }
    off = ui_approach(off, off_target);
    ui_clip_push(mx - 2, y - 2, avail + 4, h + 4);
    for (int i = 0; i < ntabs; i++) {
        int x = mx + xs[i] - (int)off;
        bool sel = i == tab_sel;
        if (sel) ui_rrect(x, y, ws[i], h, M(0.012f), t->accent);
        else {
            ui_rrect(x, y, ws[i], h, M(0.012f), t->surface);
            ui_rborder(x, y, ws[i], h, M(0.012f), 2, t->line);
        }
        ui_text(f, tab_label(tabs[i]), x + ws[i] / 2, y + (h - fh) / 2, sel ? t->accent_text : t->text, ALIGN_CENTER);
    }
    ui_clip_pop();
    return h;
}

static void small_caps(const char *s, int x, int y, color_t c) {
    char up[200];
    size_t k = 0;
    for (const char *p = s; *p && k < sizeof up - 1; p++) up[k++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    up[k] = 0;
    ui_text(UI.f_small, up, x, y, c, ALIGN_LEFT);
}

/* riquadro degli stati vuoti: titolo, due righe e l'azione con A */
static void empty_panel(int x, int y, int w, int h, const char *title, const char *l1, const char *l2, const char *action) {
    theme_t *t = &UI.th;
    int r = M(t->panel_radius) * 2, pad = M(0.035f);
    ui_rrect(x, y, w, h, r, t->surface);
    ui_rborder(x, y, w, h, r, 2, t->line);
    int ty = y + pad;
    ui_text(UI.f_title, title, x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.025f);
    char lines[3][160];
    const char *src[2] = { l1, l2 };
    for (int s = 0; s < 2; s++) {
        if (!src[s] || !src[s][0]) continue;
        int n = ui_wrap(UI.f_normal, src[s], w - 2 * pad, lines, 3);
        for (int k = 0; k < n; k++, ty += TTF_FontHeight(UI.f_normal)) ui_text(UI.f_normal, lines[k], x + pad, ty, t->text_dim, ALIGN_LEFT);
    }
    if (action) {
        int g = M(0.045f);
        ty += M(0.03f);
        ui_rrect(x + pad, ty, g, g, M(0.01f), t->accent);
        ui_text(UI.f_small, "A", x + pad + g / 2, ty + g / 2 - TTF_FontHeight(UI.f_small) / 2, t->accent_text, ALIGN_CENTER);
        ui_text(UI.f_small, action, x + pad + g + M(0.012f), ty + g / 2 - TTF_FontHeight(UI.f_small) / 2, t->text, ALIGN_LEFT);
    }
}

/* che cosa fa A quando la categoria è vuota */
enum { LE_NONE, LE_REFRESH, LE_OPEN_ALL };
static int lib_empty_kind(void) {
    if (nview) return LE_NONE;
    if (!have_games()) return LE_REFRESH;
    if (ntabs && tabs[tab_sel] == TAB_FAV) return LE_OPEN_ALL;
    return LE_NONE;
}

/* anteprima a destra (Cabinato) o in alto (Onda): immagine, titolo intero, emulatore, giocatori */
static void game_panel(const game_t *g, int gi, int x, int y, int w, int h, bool image_left) {
    theme_t *t = &UI.th;
    int r = M(t->panel_radius) * 2, pad = M(0.018f);
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    char lines[4][160], buf[200];
    int tx, ty, tw, tmax;
    if (image_left) {   /* Onda: immagine a sinistra, testo a destra, senza riquadro */
        int iw = w * 54 / 100, ih = h;
        if (iw * 3 / 4 < ih) ih = iw * 3 / 4;
        cover_box(g, x, y, iw, ih, r / 2);
        tx = x + iw + M(0.03f);
        tw = x + w - tx;
        ty = y + M(0.01f);
        tmax = y + ih;
    } else {           /* Cabinato: riquadro con l'immagine sopra */
        ui_rrect(x, y, w, h, r, t->surface);
        ui_rborder(x, y, w, h, r, 2, t->line);
        int iw = w - 2 * pad, ih = iw * 3 / 4;
        if (ih > h * 45 / 100) ih = h * 45 / 100;
        cover_box(g, x + pad, y + pad, iw, ih, r / 2);
        tx = x + pad;
        tw = iw;
        ty = y + pad + ih + M(0.016f);
        tmax = y + h - pad;
    }
    int bottom_lines = 2 + (gi >= 0 && game_autosave(gi, NULL));
    int n = ui_wrap(fn, g->title, tw, lines, 4);
    int room = (tmax - ty - bottom_lines * fsh) / fnh;
    if (n > room) n = room > 0 ? room : 0;
    for (int k = 0; k < n; k++, ty += fnh) ui_text(fn, lines[k], tx, ty, t->text, ALIGN_LEFT);
    int by = tmax - bottom_lines * fsh;
    ui_text_fit(fs, games_core_label(g->core), tw, buf, sizeof buf);
    ui_text(fs, buf, tx, by, t->text_dim, ALIGN_LEFT);
    by += fsh;
    if (g->players > 1) snprintf(buf, sizeof buf, T("info.players"), g->players);
    else snprintf(buf, sizeof buf, "%s", g->year);
    ui_text(fs, buf, tx, by, t->text_dim, ALIGN_LEFT);
    if (gi >= 0 && game_autosave(gi, NULL)) ui_text(fs, T("lib.autosave"), tx, by + fsh, t->accent, ALIGN_LEFT);
}

/* griglia di riquadri con la sola immagine (Tessera): quattro per riga, si scorre in verticale */
enum { GRID_COLS = 4 };
static void card_grid(int y, int h) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx, gap = M(0.02f), r = M(t->tile_radius) * 2;
    int cw = (W - (GRID_COLS - 1) * gap) / GRID_COLS, ch = cw * 3 / 4;
    /* a righe intere: in alto mai una riga tagliata, in basso si intravede la successiva */
    int vis = (h + gap) / (ch + gap), row = lib_sel / GRID_COLS, first = (int)(lib_scroll_target / (ch + gap) + 0.5f);
    if (vis < 1) vis = 1;
    if (row < first) first = row;
    if (row >= first + vis) first = row - vis + 1;
    lib_scroll_target = (float)first * (ch + gap);
    lib_scroll = ui_approach(lib_scroll, lib_scroll_target);
    int ring = M(t->select_border) + M(0.012f);
    ui_clip_push(mx - ring, y - ring, W + 2 * ring, h + 2 * ring);
    for (int i = 0; i < nview; i++) {
        int x = mx + (i % GRID_COLS) * (cw + gap), yy = y + (int)((i / GRID_COLS) * (ch + gap) - lib_scroll);
        if (yy + ch < y - ring || yy > y + h + ring) continue;
        ui_shadow(x, yy + M(0.004f), cw, ch, r, M(0.012f), t->shadow);
        cover_tile(&games[view[i]], x, yy, cw, ch, r);
        if (games[view[i]].fav) {   /* stellina dei preferiti nell'angolo */
            int sr = M(0.022f);
            ui_circle(x + cw - sr - M(0.008f), yy + sr + M(0.008f), sr, with_alpha(t->surface, 230));
            ui_star(x + cw - sr - M(0.008f), yy + sr + M(0.008f), sr * 2 / 3, t->accent);
        }
    }
    if (nview) selection_ring(mx + (lib_sel % GRID_COLS) * (cw + gap), y + (int)((lib_sel / GRID_COLS) * (ch + gap) - lib_scroll), cw, ch, r);
    ui_clip_pop();
}

/* Onda: elenco verticale come nella prima versione. Il gioco scelto resta in cima, più grande e
   con le informazioni; quelli già passati spariscono sopra. */
static void onda_list(int x, int y, int w, int h) {
    theme_t *t = &UI.th;
    int rh = M(t->list_row_h) * 13 / 10, r = M(t->panel_radius) * 2;
    static float anim;
    if (anim > nview) anim = 0;
    anim = ui_approach(anim, (float)lib_sel);
    char buf[200], info[200];
    ui_clip_push(x - 4, y - 4, w + 8, h + 8);
    for (int i = 0; i < nview; i++) {
        float d = i - anim;
        int yy = y + (int)(d * rh);
        if (d < -0.5f) continue;
        if (yy > y + h) break;
        const game_t *g = &games[view[i]];
        bool sel = i == lib_sel;
        if (sel) ui_rrect(x, yy, w, rh, r, t->surface);
        int th = rh * 78 / 100, tw = th * 4 / 3, ix = x + M(0.012f), iy = yy + (rh - th) / 2;
        SDL_Texture *img = media_image(g, t->cover_types, t->ncover);
        if (img) ui_image_fill(img, ix, iy, tw, th, M(0.006f));
        else ui_rrect(ix, iy, tw, th, M(0.006f), with_alpha(t->text, 40));
        if (g->fav) ui_star(ix + tw - M(0.015f), iy + M(0.015f), M(0.011f), rgba(255, 210, 60, 255));
        int tx = ix + tw + M(0.022f), avail = x + w - tx - M(0.012f);
        if (sel) {
            TTF_Font *f = UI.f_title;
            ui_text_fit(f, g->title, avail, buf, sizeof buf);
            ui_text(f, buf, tx, yy + rh / 2 - TTF_FontHeight(f) + M(0.008f), t->text, ALIGN_LEFT);
            snprintf(info, sizeof info, "%s", games_core_label(g->core));
            if (g->year[0]) { size_t l = strlen(info); snprintf(info + l, sizeof info - l, " · %s", g->year); }
            if (g->players > 1) { size_t l = strlen(info); char pl[48]; snprintf(pl, sizeof pl, T("info.players"), g->players); snprintf(info + l, sizeof info - l, " · %s", pl); }
            ui_text_fit(UI.f_small, info, avail, buf, sizeof buf);
            ui_text(UI.f_small, buf, tx, yy + rh / 2 + M(0.004f), t->accent, ALIGN_LEFT);
        } else {
            TTF_Font *f = UI.f_normal;
            ui_text_fit(f, g->title, avail, buf, sizeof buf);
            ui_text(f, buf, tx, yy + (rh - TTF_FontHeight(f)) / 2, with_alpha(t->text, 170), ALIGN_LEFT);
        }
    }
    ui_clip_pop();
}

/* elenco dei titoli (Cabinato) */
static void title_list(int x, int y, int w, int h) {
    theme_t *t = &UI.th;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), rh = fnh + fsh + M(0.018f), gap = M(0.01f);
    int rows = (h + gap) / (rh + gap);
    if (rows < 1) rows = 1;
    float ry = (float)lib_sel * (rh + gap), area = (float)(rows * (rh + gap) - gap);
    if (ry - lib_scroll_target > area - rh) lib_scroll_target = ry - (area - rh);
    if (ry < lib_scroll_target) lib_scroll_target = ry;
    lib_scroll = ui_approach(lib_scroll, lib_scroll_target);
    int r = M(0.008f);
    ui_clip_push(x - 4, y - 4, w + 8, (int)area + 8);
    for (int i = 0; i < nview; i++) {
        int yy = y + (int)(i * (rh + gap) - lib_scroll);
        if (yy > y + h || yy + rh < y) continue;
        const game_t *g = &games[view[i]];
        ui_rrect(x, yy, w, rh, r, t->surface);
        if (i == lib_sel) ui_rborder(x, yy, w, rh, r, M(0.004f) < 2 ? 2 : M(0.004f), t->accent);
        else ui_rborder(x, yy, w, rh, r, 1, t->line);
        char fit[200];
        ui_text_fit(fn, g->title, w - 2 * M(0.016f), fit, sizeof fit);
        ui_text(fn, fit, x + M(0.016f), yy + M(0.006f), t->text, ALIGN_LEFT);
        const char *sub = media_image(g, t->cover_types, t->ncover) ? games_core_label(g->core) : T("lib.no_image");
        ui_text_fit(fs, sub, w - 2 * M(0.016f), fit, sizeof fit);
        ui_text(fs, fit, x + M(0.016f), yy + M(0.006f) + fnh, t->text_dim, ALIGN_LEFT);
    }
    ui_clip_pop();
}

/* "oggi, 21:42" / "ieri, 21:42" / "09/11 21:42" */
static void when_text(time_t when, char *out, size_t n) {
    struct tm a, b;
    time_t now = time(NULL);
    localtime_r(&when, &a);
    localtime_r(&now, &b);
    char hm[16];
    strftime(hm, sizeof hm, "%H:%M", &a);
    time_t y = now - 86400;
    struct tm c;
    localtime_r(&y, &c);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(out, n, T("home.today"), hm);
    else if (a.tm_year == c.tm_year && a.tm_yday == c.tm_yday) snprintf(out, n, T("home.yesterday"), hm);
    else strftime(out, n, "%d/%m %H:%M", &a);
}

/* immagine del gioco della home: per la partita da riprendere, il punto in cui si era rimasti */
static void home_image(int gi, int x, int y, int w, int h, int r) {
    SDL_Texture *shot = gi == resume_idx ? ui_image(resume_png) : NULL;
    if (shot && resume_rot) {
        int tw, th;
        SDL_QueryTexture(shot, NULL, NULL, &tw, &th);
        bool odd = resume_rot & 1;
        float vw = (float)(odd ? th : tw), vh = (float)(odd ? tw : th), sc = fminf(w / vw, h / vh);
        int dw = (int)(vw * sc), dh = (int)(vh * sc), rw = odd ? dh : dw, rh = odd ? dw : dh;
        ui_rrect(x, y, w, h, r, rgba(14, 14, 18, 255));
        SDL_Rect d = { x + (w - rw) / 2, y + (h - rh) / 2, rw, rh };
        SDL_RenderCopyEx(UI.ren, shot, NULL, &d, -90.0 * resume_rot, NULL, SDL_FLIP_NONE);
    } else if (shot) ui_image_fill(shot, x, y, w, h, r);
    else cover_box(&games[gi], x, y, w, h, r);
}

/* simboli dei pulsanti rotondi della home di Tessera */
static void draw_icon_glyph(int kind, int cx, int cy, int s, color_t c, color_t bg) {
    int q = s / 6;
    switch (kind) {
    case IC_ALL:
        for (int i = 0; i < 4; i++) ui_rrect(cx - q - q / 4 + (i % 2) * (q + q / 2), cy - q - q / 4 + (i / 2) * (q + q / 2), q, q, q / 4, c);
        break;
    case IC_FAV: ui_star(cx, cy, s / 4, c); break;
    case IC_SETTINGS: ui_gear(cx, cy, s * 27 / 100, c); break;
    case IC_STORE: {   /* borsa della spesa con la freccia di scaricamento */
        int bw = s * 44 / 100, bh = s * 36 / 100, bx = cx - bw / 2, by = cy - bh / 2 + q / 2;
        ui_rrect(bx, by, bw, bh, q / 2, c);
        ui_ring(cx, by, bw / 4, q / 3 + 1, c);
        ui_rect(cx - bw / 4 - q / 2, by, bw / 2 + q, bw / 4 + 1, c);   /* la metà sotto del manico sparisce nella borsa */
        int aw = q / 2 + 1, ay0 = by + bh / 6, ay1 = by + bh * 5 / 8;
        ui_rect(cx - aw / 2, ay0, aw, ay1 - ay0, bg);
        for (int k = 0; k < bw / 5; k++) ui_rect(cx - bw / 5 + k, ay1 + k - bw / 5 + 1, (bw / 5 - k) * 2, 1, bg);
        break;
    }
    case IC_APPS:   /* nove puntini, come il cassetto delle app di un telefono */
        for (int i = 0; i < 9; i++) ui_circle(cx + (i % 3 - 1) * q * 5 / 4, cy + (i / 3 - 1) * q * 5 / 4, q / 3 + 1, c);
        break;
    case IC_POWER:
        ui_ring(cx, cy + q / 6, s / 4, q / 3 + 1, c);
        ui_rect(cx - q / 2, cy - s / 4 - q / 3, q, q, bg);
        ui_rrect(cx - q / 6, cy - s / 4 - q / 4, q / 3 + 1, s / 4 + q / 4, q / 6, c);
        break;
    }
}

static const char *hm_label(int m) {
    switch (m) {
    case HM_GAME: return T(resume_idx >= 0 ? "home.resume_game" : "home.last");
    case HM_LIBRARY: return T("home.btn_library");
    case HM_FAV: return T("home.favorites");
    case HM_SETTINGS: return T("common.settings");
    case HM_APPS: return T("apps.title");
    case HM_STORE: return T("store.title");
    default: return T("common.power_off");
    }
}

static int home_last_game(void) { return nhmenu && hmenu[0] == HM_GAME && nhgames ? hgame[0] : -1; }

static void home_play(int gi) {
    if (gi == resume_idx) launch_ex(&games[gi], true);
    else launch(&games[gi]);
}

static void home_do(int m) {
    static const int icon_of[] = { 0, IC_ALL, IC_FAV, IC_APPS, IC_STORE, IC_SETTINGS, IC_POWER };
    if (m == HM_GAME) { int g = home_last_game(); if (g >= 0) home_play(g); return; }
    icon_sel = icon_of[m];
    home_activate_icon();
}

/* pulsanti rotondi di Tessera: le voci del menu senza il gioco */
static int home_btns(int *out) {
    int n = 0;
    for (int i = 0; i < nhmenu; i++)
        if (hmenu[i] != HM_GAME) out[n++] = hmenu[i];
    return n;
}

/* gioco su cui agiscono X e Y nella home, -1 nessuno */
static int home_focus_game(void) {
    if (is_list_layout() || is_bar_layout()) return nhmenu && hmenu[hsel] == HM_GAME ? home_last_game() : -1;
    return hrow == 0 && hsel < nhgames ? hgame[hsel] : -1;
}

static void home_input(void) {
    if (is_showcase_layout()) { vt_input(); return; }
    if (is_gallery_layout()) { gl_input(); return; }
    input_t *in = &UI.in;
    bool cards = !is_list_layout() && !is_bar_layout();
    int old = hsel * 16 + hrow * 4 + hbtn;
    if (cards) {
        if (in->pressed[BTN_UP] && hrow == 1 && nhgames) hrow = 0;
        if (in->pressed[BTN_DOWN] && hrow == 0) hrow = 1;
        if (!nhgames) hrow = 1;
        if (hrow == 0) {
            if (in->pressed[BTN_LEFT] && hsel > 0) hsel--;
            if (in->pressed[BTN_RIGHT] && hsel < nhgames - 1) hsel++;
        } else {
            if (in->pressed[BTN_LEFT] && hbtn > 0) hbtn--;
            int bl[6];
            if (in->pressed[BTN_RIGHT] && hbtn < home_btns(bl) - 1) hbtn++;
        }
    } else {
        bool vertical = is_list_layout();
        if (in->pressed[vertical ? BTN_UP : BTN_LEFT] && hsel > 0) hsel--;
        if (in->pressed[vertical ? BTN_DOWN : BTN_RIGHT] && hsel < nhmenu - 1) hsel++;
    }
    if (hsel * 16 + hrow * 4 + hbtn != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_A]) {
        if (cards && hrow == 0 && hsel < nhgames) home_play(hgame[hsel]);
        else if (cards) { int bl[6], nb = home_btns(bl); if (hbtn < nb) home_do(bl[hbtn]); }
        else if (nhmenu) home_do(hmenu[hsel]);
        return;
    }
    int g = home_focus_game();
    if (in->pressed[BTN_X] && g >= 0) { open_detail(g); return; }
    if (in->pressed[BTN_Y] && g >= 0) {
        games_toggle_fav(UI.root, &games[g]);
        ui_toast(games[g].fav ? T("home.fav_added") : T("home.fav_removed"));
        build_tabs();
        build_carousel();
        ui_sound(SND_OK);
    }
    if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); }
}

/* pulsante a pillola: pieno se scelto */
static int pill_f(TTF_Font *f, const char *label, int x, int y, bool sel) {
    theme_t *t = &UI.th;
    int fh = TTF_FontHeight(f), h = fh + M(0.022f), w = ui_text_w(f, label) + 2 * M(f == UI.f_small ? 0.018f : 0.022f);
    if (sel) ui_rrect(x, y, w, h, M(0.012f), t->accent);
    else { ui_rrect(x, y, w, h, M(0.012f), t->surface); ui_rborder(x, y, w, h, M(0.012f), 2, t->line); }
    ui_text(f, label, x + w / 2, y + (h - fh) / 2, sel ? t->accent_text : t->text, ALIGN_CENTER);
    return w;
}

static void home_title(const char *s, int y, color_t c) {
    char up[160];
    snprintf(up, sizeof up, "%s", s);
    if (UI.th.title_upper)
        for (char *p = up; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
    ui_text(UI.f_title, up, M(UI.th.margin), y, c, ALIGN_LEFT);
}

/* sottotitolo del gioco: "Salvataggio automatico · ieri, 21:42" o l'emulatore */
static void home_game_sub(int gi, char *out, size_t n) {
    if (gi == resume_idx) {
        char w[48];
        when_text(resume_when, w, sizeof w);
        snprintf(out, n, T("home.autosave"), w);
    } else snprintf(out, n, "%s", games_core_label(games[gi].core));
}

static void home_keys(void) {
    int g = home_focus_game();
    const char *act = T("lib.confirm");
    if (g >= 0) act = T(g == resume_idx ? "home.resume" : "common.play");
    const char *h1[] = { "A", act, NULL, "X", T("common.info"), "", "Y", T(g >= 0 && games[g].fav ? "common.unfavorite" : "common.favorite"), "",
                         "START", T("common.settings"), NULL };
    const char *h2[] = { "A", act, NULL, "START", T("common.settings"), NULL };
    if (g >= 0) ui_keybar(h1, 12); else ui_keybar(h2, 6);
}

/* Tessera: una fila di riquadri con la sola immagine, il nome del gioco scelto sopra, i pulsanti
   rotondi sotto */
static void home_draw_cards(int y, int bottom) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx, r = M(t->tile_radius) * 2;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    char fit[200];
    int bl[6], nb = home_btns(bl);
    if (hbtn >= nb) hbtn = nb - 1;
    int is = M(t->icon_size), step = is + M(0.06f);
    int iy = bottom - fsh - M(0.02f) - is / 2;
    int lab_h = fnh + M(0.018f), avail = iy - is / 2 - M(0.05f) - y - lab_h;
    int tw = W * 36 / 100, th = tw * 3 / 4;
    if (th > avail) { th = avail; tw = th * 4 / 3; }
    int ty = y + lab_h + (avail - th) / 2;
    if (!nhgames) {
        if (!NEMUS) empty_panel(mx, y, W, iy - is / 2 - M(0.05f) - y, T("store.ask_title"), T("store.ask_body"), NULL, NULL);
        else if (!have_games()) empty_panel(mx, y, W, iy - is / 2 - M(0.05f) - y, T("lib.empty_title"), T("lib.empty_text"), NULL, NULL);
        else empty_panel(mx, y, W, iy - is / 2 - M(0.05f) - y, T("home.no_game_title"), T("home.no_game_text"), NULL, NULL);
    } else {
        /* il gioco scelto è più grande degli altri; le misure cambiano con un'animazione */
        int gap = M(0.022f), sw = tw * 72 / 100;
        static float hs, hs_t, grow[16];
        float pos[16], acc = 0, sel_pos = (float)hsel * (sw + gap);
        for (int i = 0; i < nhgames && i < 16; i++) {
            grow[i] = ui_approach(grow[i], i == hsel && hrow == 0 ? 1.0f : 0.0f);
            pos[i] = acc;
            acc += sw + (tw - sw) * grow[i] + gap;
        }
        if (sel_pos - hs_t > W - tw) hs_t = sel_pos - (W - tw);
        if (sel_pos < hs_t) hs_t = sel_pos;
        hs = ui_approach(hs, hs_t);
        int ring = M(t->select_border) + M(0.012f), sx0 = mx, sel_w = tw, sel_h = th, sel_y = ty;
        ui_clip_push(0, ty - ring, UI.w, th + 2 * ring);
        for (int i = 0; i < nhgames && i < 16; i++) {
            int w = sw + (int)((tw - sw) * grow[i]), h = w * 3 / 4, x = mx + (int)(pos[i] - hs), yy = ty + (th - h) / 2;
            if (x > UI.w || x + w < 0) continue;
            ui_shadow(x, yy + M(0.006f), w, h, r, M(0.018f), t->shadow);
            home_image(hgame[i], x, yy, w, h, r);
            if (i == hsel) { sx0 = x; sel_w = w; sel_h = h; sel_y = yy; }
        }
        if (hrow == 0) selection_ring(sx0, sel_y, sel_w, sel_h, r);
        ui_clip_pop();
        if (hrow == 0) {   /* nome del gioco scelto, e accanto perché è qui */
            const char *sub = hkind[hsel] == HK_RESUME ? T("home.resume_hint") : hkind[hsel] == HK_FAV ? T("home.fav_kind")
                            : hkind[hsel] == HK_LAST ? T("home.last") : T("home.recent_kind");
            int lx = sx0 < mx ? mx : sx0, ly = ty - lab_h;
            ui_text_fit(fn, games[hgame[hsel]].title, mx + W - lx, fit, sizeof fit);
            int w1 = ui_text(fn, fit, lx, ly, t->accent, ALIGN_LEFT);
            if (lx + w1 + M(0.02f) + ui_text_w(fs, sub) < mx + W)
                ui_text(fs, sub, lx + w1 + M(0.02f), ly + fnh - fsh - M(0.003f), t->text_dim, ALIGN_LEFT);
        }
    }
    /* pulsanti rotondi: libreria, preferiti, impostazioni, spegni; il nome sotto quello scelto */
    static const int icon_of[] = { 0, IC_ALL, IC_FAV, IC_APPS, IC_STORE, IC_SETTINGS, IC_POWER };
    int ix0 = UI.w / 2 - (nb - 1) * step / 2;
    for (int b = 0; b < nb; b++) {
        int cx = ix0 + b * step, ic = icon_of[bl[b]];
        ui_shadow(cx - is / 2, iy - is / 2 + M(0.004f), is, is, is / 2, M(0.012f), t->shadow);
        ui_circle(cx, iy, is / 2, t->surface);
        draw_icon_glyph(ic, cx, iy, is, ic == IC_POWER ? t->text : t->accent, t->surface);
    }
    if (hrow == 1) {
        selection_ring(ix0 + hbtn * step - is / 2, iy - is / 2, is, is, is / 2);
        ui_text(fs, hm_label(bl[hbtn]), ix0 + hbtn * step, iy + is / 2 + M(0.016f), t->accent, ALIGN_CENTER);
    }
}

static void home_draw_menu(int y, int bottom) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx, lw = W * 32 / 100, r = M(0.008f);
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int nr = nhmenu > 6 ? nhmenu : 6, gap = M(0.012f), rh = (bottom - y - (nr - 1) * gap) / nr;
    if (rh > fnh + M(0.05f)) rh = fnh + M(0.05f);
    for (int i = 0; i < nhmenu; i++) {
        int yy = y + i * (rh + gap);
        bool sel = i == hsel;
        if (sel) ui_rrect(mx, yy, lw, rh, r, t->accent);
        else { ui_rrect(mx, yy, lw, rh, r, t->surface); ui_rborder(mx, yy, lw, rh, r, 2, t->line); }
        char num[8], fit[120];
        snprintf(num, sizeof num, "%02d", i + 1);
        ui_text(fn, num, mx + M(0.016f), yy + (rh - fnh) / 2, sel ? t->accent_text : t->text, ALIGN_LEFT);
        int lx = mx + M(0.016f) + ui_text_w(fn, "00") + M(0.022f);
        ui_text_fit(fn, hm_label(hmenu[i]), mx + lw - lx - M(0.012f), fit, sizeof fit);
        ui_text(fn, fit, lx, yy + (rh - fnh) / 2, sel ? t->accent_text : t->text, ALIGN_LEFT);
    }
    int px = mx + lw + M(0.025f), pw = mx + W - px, ph = bottom - y, pad = M(0.018f);
    ui_rrect(px, y, pw, ph, r, t->surface);
    ui_rborder(px, y, pw, ph, r, 2, t->line);
    int gi = home_last_game();
    if (gi < 0) {
        ui_text(fn, T(!NEMUS ? "store.ask_title" : have_games() ? "home.no_game_title" : "lib.empty_title"), px + pad, y + pad, t->text, ALIGN_LEFT);
        return;
    }
    small_caps(T(gi == resume_idx ? "home.in_progress" : "home.last"), px + pad, y + pad, t->accent);
    int iy = y + pad + fsh + M(0.012f), iw = pw - 2 * pad, ih = ph - (iy - y) - pad - fnh - fsh - M(0.03f);
    if (ih > iw * 3 / 4) ih = iw * 3 / 4;
    home_image(gi, px + pad, iy, iw, ih, r);
    char up[200], fit[200], sub[120];
    snprintf(up, sizeof up, "%s", games[gi].title);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    ui_text_fit(fn, up, iw, fit, sizeof fit);
    int ty = iy + ih + M(0.016f);
    ui_text(fn, fit, px + pad, ty, t->text, ALIGN_LEFT);
    home_game_sub(gi, sub, sizeof sub);
    ui_text_fit(fs, sub, iw, fit, sizeof fit);
    ui_text(fs, fit, px + pad, ty + fnh, t->text_dim, ALIGN_LEFT);
}

static void home_draw_bar(int y, int bottom) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    /* sezioni in orizzontale: col carattere normale, o col piccolo se non stanno in una riga */
    TTF_Font *pf = fn;
    int need = 0;
    for (int i = 0; i < nhmenu; i++)
        need += ui_text_w(fn, hmenu[i] == HM_GAME ? T(resume_idx >= 0 ? "home.resume" : "home.last") : hm_label(hmenu[i])) + 2 * M(0.022f) + M(0.014f);
    if (need > W) pf = fs;
    int x = mx;
    for (int i = 0; i < nhmenu; i++)
        x += pill_f(pf, hmenu[i] == HM_GAME ? T(resume_idx >= 0 ? "home.resume" : "home.last") : hm_label(hmenu[i]), x, y, i == hsel) + M(0.014f);
    y += TTF_FontHeight(pf) + M(0.022f) + M(0.03f);
    int iw = W * 61 / 100, ih = bottom - y;
    if (ih > iw * 2 / 3) ih = iw * 2 / 3;
    int gi = home_last_game(), m = nhmenu ? hmenu[hsel] : HM_LIBRARY;
    if (m == HM_APPS || m == HM_STORE) {   /* riquadro con il simbolo delle app o dello Store */
        ui_rrect(mx, y, iw, ih, M(0.008f), t->tile);
        draw_icon_glyph(m == HM_APPS ? IC_APPS : IC_STORE, mx + iw / 2, y + ih / 2, ih / 2, t->accent, t->tile);
    } else if (gi >= 0 && m != HM_LIBRARY) home_image(gi, mx, y, iw, ih, M(0.008f));
    else draw_all_tile(mx, y, iw, ih, M(0.008f));
    int tx = mx + iw + M(0.025f), tw = mx + W - tx, ty = y + M(0.006f);
    char buf[200], lines[4][160];
    small_caps(hm_label(m), tx, ty, t->accent);
    ty += fsh + M(0.02f);
    const char *title = NULL, *sub = NULL, *action = T("lib.confirm");
    char subbuf[160];
    switch (m) {
    case HM_GAME: title = games[gi].title; home_game_sub(gi, subbuf, sizeof subbuf); sub = subbuf; action = T(gi == resume_idx ? "home.resume" : "common.play"); break;
    case HM_LIBRARY: title = T("home.btn_library"); snprintf(subbuf, sizeof subbuf, T("info.games"), ngames); sub = subbuf; break;
    case HM_FAV: {
        int nf = 0;
        for (int i = 0; i < ngames; i++) nf += games[i].fav && !games[i].hidden;
        title = T("home.favorites"); snprintf(subbuf, sizeof subbuf, T("home.favs_count"), nf); sub = subbuf; break;
    }
    case HM_SETTINGS: title = T("common.settings"); sub = T("home.settings_desc"); break;
    case HM_APPS: title = T("apps.title"); snprintf(subbuf, sizeof subbuf, T("apps.count"), napps); sub = subbuf; break;
    case HM_STORE: title = T("store.title"); sub = T("settings_desc.store"); break;
    default: title = T("common.power_off"); sub = T("home.power_desc"); break;
    }
    int n = ui_wrap(fn, title, tw, lines, 3);
    for (int k = 0; k < n; k++, ty += fnh) ui_text(fn, lines[k], tx, ty, t->text, ALIGN_LEFT);
    ty += M(0.012f);
    n = ui_wrap(fs, sub, tw, lines, 3);
    for (int k = 0; k < n; k++, ty += fsh) ui_text(fs, lines[k], tx, ty, t->text_dim, ALIGN_LEFT);
    int g = M(0.045f), ay = y + ih - g;
    ui_rrect(tx, ay, g, g, M(0.01f), t->accent);
    ui_text(fs, "A", tx + g / 2, ay + g / 2 - fsh / 2, t->accent_text, ALIGN_CENTER);
    ui_text(fs, action, tx + g + M(0.012f), ay + g / 2 - fsh / 2, t->text, ALIGN_LEFT);
    (void)buf;
}

static void home_draw(void) {
    if (is_showcase_layout()) { vt_draw(); return; }
    if (is_gallery_layout()) { gl_draw(); return; }
    theme_t *t = &UI.th;
    topbar_theme();
    int y = M(t->topbar_h) + M(0.025f), bottom = UI.h - ui_keybar_h() - M(0.03f);
    bool cards = !is_list_layout() && !is_bar_layout();
    if (is_bar_layout()) {   /* Tessera e Cabinato senza titolo: si va subito ai giochi */
        home_title(T("home.title_bar"), y, t->text);
        y += TTF_FontHeight(UI.f_title) + M(0.03f);
    }
    if (cards) home_draw_cards(y, bottom);
    else if (is_list_layout()) home_draw_menu(y, bottom);
    else home_draw_bar(y, bottom);
    home_keys();
}

/* ------------------------------------------------------ piattaforme */
/* Prima della libreria: le categorie (Tutti, emulatori, Linux, Preferiti, collezioni) come
   illustrazioni disegnate. Tessera e Onda: schede colorate a righe; Cabinato: una vetrina di
   cabinati con la barra del nome in basso. */

static float plat_anim;

static color_t hexc(unsigned c) { return rgba(c >> 16 & 255, c >> 8 & 255, c & 255, 255); }

static color_t emu_color(const char *id);
static color_t plat_color(int tab) {
    if (is_plan(tab)) return hexc(0x6A6D74);
    if (tab == TAB_ALL) return hexc(0x3A4E5C);
    if (tab == TAB_LINUX) return hexc(0x4B3C86);
    if (tab == TAB_FAV) return hexc(0x8C5A1C);
    if (tab >= TAB_COLL) { static const unsigned pal[] = { 0x2F6B45, 0x6B2F5E, 0x2F4F6B, 0x6B5A2F }; return hexc(pal[(tab - TAB_COLL) % 4]); }
    return emu_color(EMUS[tab - TAB_EMU].id);
}

/* colore di una piattaforma dal suo emulatore (anche per i pacchetti non installati dello Store) */
static color_t emu_color(const char *id) {
    if (strstr(id, "fbneo")) return hexc(0x0E6A6C);
    if (strstr(id, "mame")) return hexc(0x8E2A45);
    if (!strcmp(id, "nes")) return hexc(0xA8322D);
    if (!strcmp(id, "md")) return hexc(0x2B4C9B);
    if (!strcmp(id, "sms")) return hexc(0x8A2F3B);
    if (!strcmp(id, "gg")) return hexc(0x2F6F8F);
    if (!strcmp(id, "pce")) return hexc(0xA4471F);
    if (!strcmp(id, "pcecd")) return hexc(0x8C3F6E);
    if (!strcmp(id, "segacd")) return hexc(0x1F3F7A);
    if (!strcmp(id, "a2600")) return hexc(0x7A4E26);
    if (!strcmp(id, "a7800")) return hexc(0x3E3E4A);
    if (!strcmp(id, "lynx")) return hexc(0x2E4F6E);
    if (!strcmp(id, "coleco")) return hexc(0x2E3E57);
    if (!strcmp(id, "intv")) return hexc(0x7A5A2E);
    if (!strcmp(id, "ws")) return hexc(0x4E6A3A);
    if (!strcmp(id, "wsc")) return hexc(0x2F5E8C);
    if (!strcmp(id, "pokemini")) return hexc(0x6E2F5E);
    if (!strcmp(id, "vb")) return hexc(0x9E1F1F);
    if (!strcmp(id, "scummvm")) return hexc(0x2E7D4F);
    if (!strcmp(id, "ngp")) return hexc(0x2D6A4F);
    if (!strcmp(id, "gb")) return hexc(0x6F7F2C);
    if (!strcmp(id, "gbc")) return hexc(0x1E7F86);
    if (!strcmp(id, "gba")) return hexc(0x4A3FA0);
    if (!strcmp(id, "snes")) return hexc(0x6B5B95);
    if (!strcmp(id, "ps")) return hexc(0x3A4E6C);
    if (!strcmp(id, "dc")) return hexc(0xC0571F);
    static const unsigned pal[] = { 0x2A5C8E, 0x5C7A2A, 0x8E5C2A };
    unsigned h = 0;
    for (const char *c = id; *c; c++) h = h * 31 + (unsigned char)*c;
    return hexc(pal[h % 3]);
}

static int plat_count(int tab) {
    int n = 0;
    for (int i = 0; i < ngames; i++) n += in_tab(&games[i], tab);
    return n;
}
static void plat_count_text(int tab, char *out, size_t sz) {
    if (is_plan(tab)) { snprintf(out, sz, "%s", T("plat.coming")); return; }
    int n = plat_count(tab);
    if (n == 1) snprintf(out, sz, "%s", T("plat.one"));
    else snprintf(out, sz, T("info.games"), n);
}

static void plat_line(int x1, int y1, int x2, int y2, int th, color_t c) {
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
    c = ui_shade(c);
    SDL_SetRenderDrawColor(UI.ren, c.r, c.g, c.b, c.a);
    for (int k = 0; k < th; k++) SDL_RenderDrawLine(UI.ren, x1 + k, y1, x2 + k, y2);
}

static void plat_quad(int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4, color_t c) {
    ui_triangle(x1, y1, x2, y2, x3, y3, c);
    ui_triangle(x1, y1, x3, y3, x4, y4, c);
}

static void plat_text(const char *s, int cx, int y, int maxw, int maxh, color_t c) {
    TTF_Font *f = TTF_FontHeight(UI.f_normal) <= maxh && ui_text_w(UI.f_normal, s) <= maxw ? UI.f_normal : UI.f_small;
    if (TTF_FontHeight(f) > maxh + maxh / 3) return;   /* troppo piccolo per leggerlo: meglio niente */
    char fit[100];
    ui_text_fit(f, s, maxw, fit, sizeof fit);
    ui_text(f, fit, cx, y + (maxh - TTF_FontHeight(f)) / 2, rgba(0xF6, 0xEC, 0xE2, 255), ALIGN_CENTER);
}

/* cabinato da sala giochi: fianco a righe, insegna col nome, schermo con un tramonto, comandi */
static void draw_cabinet(int x, int y, int w, int h, color_t c, const char *name) {
    color_t dark = hexc(0x161A24), side = mix(c, rgba(0, 0, 0, 255), 0.3f), gold = hexc(0xF2C05C);
    int bw = w * 80 / 100, st = h / 70 + 1, r = w / 28 + 1;
    int sx0 = x + bw - st, sx1 = x + w;
    plat_quad(sx0, y + h * 3 / 100, sx1, y + h * 8 / 100, sx1, y + h * 96 / 100, sx0, y + h, side);
    for (int k = 0; k < 10; k++) {
        int ly = y + h * 13 / 100 + k * h * 8 / 100;
        plat_line(sx0 + (sx1 - sx0) / 4, ly, sx1 - (sx1 - sx0) / 6, ly + h / 45, 1, with_alpha(gold, 180));
    }
    ui_rrect(x, y, bw, h, r, c);
    ui_rrect(x + st, y + st, bw - 2 * st, h - 2 * st, r, dark);
    /* insegna */
    int mp = bw * 7 / 100, mx = x + mp, mw = bw - 2 * mp, my = y + h * 4 / 100, mh = h * 11 / 100;
    ui_rrect(mx, my, mw, mh, r / 2 + 1, c);
    char up[64];
    snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    plat_text(up, mx + mw / 2, my, mw - mw / 10, mh, rgba(255, 255, 255, 255));
    /* schermo */
    int scy = my + mh + h * 4 / 100, sch = h * 31 / 100, ip = mw * 6 / 100;
    ui_rrect(mx, scy, mw, sch, r / 2 + 1, hexc(0x30353F));
    int ix = mx + ip, iy = scy + ip, iw = mw - 2 * ip, ih = sch - 2 * ip;
    ui_rect(ix, iy, iw, ih, hexc(0x1B172C));
    int dot = iw / 80 + 1;
    for (int k = 0; k < 12; k++) ui_rect(ix + (k * 37 % 97) * iw / 100, iy + (k * 53 % 47) * ih / 100, dot, dot, with_alpha(gold, 190));
    int sr = ih * 22 / 100, scx = ix + iw * 68 / 100, scy2 = iy + ih * 38 / 100;
    ui_circle(scx, scy2, sr, mix(gold, c, 0.12f));
    for (int k = 1; k <= 3; k++) ui_rect(scx - sr, scy2 + k * sr / 3, 2 * sr + 1, sr / 7 + 1, hexc(0x1B172C));
    color_t mtn = mix(c, hexc(0xFF8C5A), 0.35f);
    ui_triangle(ix, iy + ih * 80 / 100, ix + iw * 24 / 100, iy + ih * 56 / 100, ix + iw * 48 / 100, iy + ih * 80 / 100, mtn);
    ui_triangle(ix + iw * 34 / 100, iy + ih * 80 / 100, ix + iw * 62 / 100, iy + ih * 50 / 100, ix + iw, iy + ih * 80 / 100, mtn);
    ui_rect(ix, iy + ih * 80 / 100, iw, ih - ih * 80 / 100, hexc(0x14121F));
    for (int yy = iy; yy < iy + ih; yy += 3) ui_rect(ix, yy, iw, 1, rgba(0, 0, 0, 55));
    /* comandi */
    int py0 = scy + sch + h * 4 / 100, ph = h * 12 / 100;
    plat_quad(mx, py0, mx + mw, py0, x + bw - st, py0 + ph, x + st, py0 + ph, c);
    ui_rect(x + st, py0 + ph, bw - 2 * st, h * 4 / 100, side);
    int jx = mx + mw * 22 / 100, jy = py0 + ph * 62 / 100, jr = ph * 22 / 100 + 1;
    ui_circle(jx, jy, jr, hexc(0x111111));
    plat_line(jx, jy, jx - jr / 3, py0 - ph * 25 / 100, st / 2 + 1, hexc(0xB8B8C0));
    ui_circle(jx - jr / 3, py0 - ph * 25 / 100, jr * 8 / 10 + 1, gold);
    for (int k = 0; k < 6; k++)
        ui_circle(mx + mw * (54 + (k % 3) * 13) / 100 + (k / 3) * mw * 4 / 100, py0 + ph * (35 + (k / 3) * 32) / 100, ph / 9 + 1, gold);
    /* porta delle monete */
    int dw = bw * 30 / 100, dh = h * 13 / 100, dx = x + (bw - dw) / 2, dy = py0 + ph + h * 10 / 100;
    ui_rrect(dx, dy, dw, dh, r / 2 + 1, hexc(0x2A2E38));
    ui_rborder(dx, dy, dw, dh, r / 2 + 1, 1, hexc(0x5A5F6A));
    for (int k = 0; k < 2; k++) ui_rrect(dx + dw * (24 + k * 34) / 100, dy + dh * 28 / 100, dw * 18 / 100 + 1, dh * 26 / 100 + 1, 1, hexc(0xC03A4A));
}

/* console da casa: cartuccia inserita con il nome sull'etichetta, corpo con fessura e spia,
   controller davanti */
static void draw_console(int x, int y, int w, int h, color_t c, const char *name) {
    color_t body = hexc(0x2B2E38), body_l = hexc(0x454A58), cart = hexc(0xCFC6B4), pad = hexc(0x1E2028);
    int r = w / 24 + 1;
    /* cartuccia: la parte bassa entra nella fessura */
    int cw = w * 66 / 100, ch = h * 46 / 100, cx = x + (w - cw) / 2, cy = y;
    ui_rrect(cx, cy, cw, ch, r, cart);
    for (int k = 0; k < 3; k++)
        ui_rect(cx + cw * 12 / 100, cy + ch * (7 + k * 6) / 100, cw * 76 / 100, ch / 40 + 1, mix(cart, rgba(0, 0, 0, 255), 0.18f));
    int lx = cx + cw * 6 / 100, ly = cy + ch * 30 / 100, lw = cw * 88 / 100, lh = ch * 40 / 100;
    ui_rrect(lx, ly, lw, lh, r / 2 + 1, c);
    char up[64];
    snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    plat_text(up, lx + lw / 2, ly, lw - lw / 10, lh, rgba(255, 255, 255, 255));
    /* corpo */
    int by = y + h * 38 / 100, bh = h * 42 / 100;
    ui_rrect(x, by, w, bh, r, body);
    ui_rect(cx - w * 2 / 100, by, cw + w * 4 / 100, bh * 10 / 100 + 1, hexc(0x14161C));
    ui_rect(x + w * 6 / 100, by + bh * 60 / 100, w * 88 / 100, bh * 7 / 100 + 1, c);
    ui_circle(x + w * 10 / 100, by + bh * 32 / 100, bh / 14 + 1, hexc(0xE04848));
    for (int k = 0; k < 2; k++) ui_rrect(x + w * (66 + k * 13) / 100, by + bh * 24 / 100, w * 10 / 100, bh * 15 / 100, 2, body_l);
    /* controller collegato col cavo */
    int pw = w * 46 / 100, ph = h * 22 / 100, px = x + w * 48 / 100, py = y + h - ph;
    plat_line(x + w * 30 / 100, by + bh, px + pw * 20 / 100, py + ph / 3, w / 120 + 1, pad);
    ui_rrect(px, py, pw, ph, ph / 3, pad);
    int dx = px + pw * 24 / 100, dy = py + ph / 2, arm = ph * 30 / 100, th = arm * 2 / 3 + 1;
    ui_rect(dx - arm, dy - th / 2, 2 * arm, th, body_l);
    ui_rect(dx - th / 2, dy - arm, th, 2 * arm, body_l);
    for (int k = 0; k < 2; k++) ui_circle(px + pw * (66 + k * 16) / 100, dy + (k ? -1 : 1) * ph / 10, ph / 7 + 1, c);
}

/* console portatile: verticale (schermo sopra, comandi sotto) o orizzontale (schermo al
   centro, comandi ai lati), col nome sotto lo schermo */
static void draw_handheld(int x, int y, int w, int h, color_t c, const char *name, bool wide) {
    color_t body = mix(c, hexc(0xD9D5CD), 0.62f), edge = mix(body, rgba(0, 0, 0, 255), 0.22f);
    color_t bezel = hexc(0x2E3140), dark = hexc(0x1E2028);
    int r = (wide ? h : w) / 9 + 1;
    ui_rrect(x, y + h / 40, w, h, r, edge);
    ui_rrect(x, y, w, h - h / 40, r, body);
    int bx, byy, bw, bh;   /* cornice dello schermo */
    if (wide) { bw = w * 52 / 100; bh = h * 70 / 100; bx = x + (w - bw) / 2; byy = y + h * 9 / 100; }
    else { bw = w * 82 / 100; bh = h * 46 / 100; bx = x + (w - bw) / 2; byy = y + h * 6 / 100; }
    ui_rrect(bx, byy, bw, bh, r / 2 + 1, bezel);
    int lh = bh * 20 / 100, sp = bw * 9 / 100;
    int sx = bx + sp, sy = byy + bh * 9 / 100, sw = bw - 2 * sp, sh = bh - bh * 9 / 100 - lh - bh * 4 / 100;
    color_t scr = mix(c, hexc(0xC9D6A0), 0.55f);
    ui_rect(sx, sy, sw, sh, scr);
    /* una scena piccola: terreno, una collina, un personaggio */
    color_t ink = mix(scr, rgba(0, 0, 0, 255), 0.55f);
    ui_rect(sx, sy + sh * 78 / 100, sw, sh - sh * 78 / 100, ink);
    ui_triangle(sx + sw * 52 / 100, sy + sh * 78 / 100, sx + sw * 74 / 100, sy + sh * 46 / 100, sx + sw * 96 / 100, sy + sh * 78 / 100, mix(scr, ink, 0.5f));
    int pq = sh / 8 + 1;
    ui_rect(sx + sw * 24 / 100, sy + sh * 78 / 100 - 2 * pq, pq, 2 * pq, ink);
    ui_circle(sx + sw * 24 / 100 + pq / 2, sy + sh * 78 / 100 - 2 * pq - pq / 2, pq / 2 + 1, ink);
    ui_circle(sx + sw * 14 / 100, sy + sh * 20 / 100, sh / 10 + 1, mix(scr, rgba(255, 255, 255, 255), 0.5f));
    char up[64];
    snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    plat_text(up, bx + bw / 2, byy + bh - lh - bh * 2 / 100, bw - bw / 8, lh, rgba(255, 255, 255, 255));
    /* croce e due tasti */
    int dx, dy, ax, ay, arm;
    if (wide) { arm = h * 11 / 100; dx = x + w * 12 / 100; dy = y + h * 42 / 100; ax = x + w * 86 / 100; ay = y + h * 40 / 100; }
    else { arm = w * 11 / 100; dx = x + w * 24 / 100; dy = y + h * 66 / 100; ax = x + w * 72 / 100; ay = y + h * 65 / 100; }
    int th = arm * 2 / 3 + 1, br = arm * 55 / 100 + 1;
    ui_rect(dx - arm, dy - th / 2, 2 * arm, th, dark);
    ui_rect(dx - th / 2, dy - arm, th, 2 * arm, dark);
    ui_circle(ax - br, ay + br * 6 / 10, br, c);
    ui_circle(ax + br, ay - br * 6 / 10, br, c);
    /* select e start */
    int pw = (wide ? w * 6 : w * 13) / 100, pt = (wide ? h : w) * 3 / 100 + 1;
    int py = wide ? y + h * 74 / 100 : y + h * 81 / 100, pcx = wide ? dx : x + w / 2;
    for (int k = 0; k < 2; k++) ui_rrect(pcx + (k ? pw / 4 : -pw - pw / 4), py, pw, pt, pt / 2, edge);
    /* griglia dell'altoparlante */
    if (!wide)
        for (int k = 0; k < 4; k++)
            plat_line(x + w * (70 + k * 6) / 100, y + h * 93 / 100, x + w * (78 + k * 6) / 100, y + h * 82 / 100, w / 90 + 1, edge);
    else
        for (int k = 0; k < 3; k++) ui_circle(x + w * (82 + k * 4) / 100, y + h * 80 / 100, h / 60 + 1, edge);
}

/* Console da casa riconoscibili dalla forma, senza marchi: NES (scatola con sportello),
   Super Nintendo (arrotondata, cursori, controller a osso), Mega Drive (nera con la cupola),
   Master System (cuneo nero con le righe), PC Engine (cubetto bianco con la HuCard). */
enum { CON_NES, CON_SNES, CON_MD, CON_SMS, CON_PCE };

#define PX(p) (x + w * (p) / 100)
#define PY(p) (y + h * (p) / 100)

static void con_label(int lx, int ly, int lw, int lh, int r, color_t c, const char *name) {
    ui_rrect(lx, ly, lw, lh, r, c);
    char up[64];
    snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    plat_text(up, lx + lw / 2, ly, lw - lw / 10, lh, rgba(255, 255, 255, 255));
}

/* cartuccia vista da sopra: righe in alto, etichetta col nome */
static void con_cart(int cx, int cy, int cw, int ch, int r, color_t shell, color_t c, const char *name) {
    ui_rrect(cx, cy, cw, ch, r, shell);
    for (int k = 0; k < 3; k++)
        ui_rect(cx + cw * 12 / 100, cy + ch * (8 + k * 7) / 100, cw * 76 / 100, ch / 40 + 1, mix(shell, rgba(0, 0, 0, 255), 0.2f));
    con_label(cx + cw * 7 / 100, cy + ch * 34 / 100, cw * 86 / 100, ch * 40 / 100, r / 2 + 1, c, name);
}

static void con_dpad(int dx, int dy, int arm, color_t col) {
    int th = arm * 2 / 3 + 1;
    ui_rect(dx - arm, dy - th / 2, 2 * arm, th, col);
    ui_rect(dx - th / 2, dy - arm, th, 2 * arm, col);
}

static void draw_home_console(int kind, int x, int y, int w, int h, color_t c, const char *name) {
    color_t black = hexc(0x1C1D22), cable = hexc(0x22242B);
    int r = w / 24 + 1, lw1 = w / 120 + 1;
    switch (kind) {
    case CON_NES: {
        color_t light = hexc(0xCFCBC4), dark = hexc(0x45454D), flap = hexc(0xB5B1AA);
        int by = PY(20), bh = h * 50 / 100, split = bh * 56 / 100;
        ui_rrect(x, by + h / 50, w, bh, r / 2 + 1, mix(dark, black, 0.5f));
        ui_rrect(x, by, w, bh, r / 2 + 1, dark);
        ui_rrect(x, by, w, split, r / 2 + 1, light);
        ui_rect(x, by + split - r, w, r, light);
        for (int k = 0; k < 4; k++) ui_rect(PX(70), by + bh * (9 + k * 9) / 100, w * 24 / 100, h / 140 + 1, mix(light, black, 0.18f));
        int fx = PX(5), fy = by + bh * 8 / 100, fw = w * 60 / 100, fh = bh * 42 / 100;
        ui_rrect(fx, fy, fw, fh, 2, flap);
        con_label(fx + fw * 7 / 100, fy + fh * 18 / 100, fw * 86 / 100, fh * 64 / 100, 2, c, name);
        for (int k = 0; k < 2; k++) ui_rrect(PX(7) + k * w * 12 / 100, by + bh * 68 / 100, w * 9 / 100, bh * 14 / 100, 2, light);
        ui_circle(PX(33), by + bh * 75 / 100, bh / 24 + 1, hexc(0xE04848));
        for (int k = 0; k < 2; k++) ui_rrect(PX(62) + k * w * 15 / 100, by + bh * 66 / 100, w * 11 / 100, bh * 20 / 100, 2, hexc(0x2A2A30));
        int pw = w * 48 / 100, ph = h * 20 / 100, px = PX(48), py = y + h - ph;
        plat_line(PX(30), by + bh, px + pw * 15 / 100, py + ph / 2, lw1, cable);
        ui_rrect(px, py, pw, ph, ph / 10 + 1, light);
        ui_rrect(px + pw * 5 / 100, py + ph * 16 / 100, pw * 90 / 100, ph * 68 / 100, ph / 12 + 1, hexc(0x2B2B30));
        con_dpad(px + pw * 20 / 100, py + ph / 2, ph * 24 / 100, hexc(0x8A8A90));
        for (int k = 0; k < 2; k++) ui_rrect(px + pw * (40 + k * 11) / 100, py + ph * 54 / 100, pw * 9 / 100, ph / 9 + 1, 1, light);
        for (int k = 0; k < 2; k++) ui_circle(px + pw * (72 + k * 14) / 100, py + ph * 56 / 100, ph * 13 / 100 + 1, hexc(0xD0403A));
        break;
    }
    case CON_SNES: {
        color_t light = hexc(0xD5D3DA), band = hexc(0xA9A6B2), shell = hexc(0x8E8B98);
        int by = PY(30), bh = h * 42 / 100, cw = w * 42 / 100, ch = h * 32 / 100, cx = x + (w - cw) / 2;
        con_cart(cx, PY(3), cw, ch, r, shell, c, name);
        ui_rrect(x, by + h / 50, w, bh, w / 9, mix(band, black, 0.3f));
        ui_rrect(x, by, w, bh, w / 9, light);
        ui_rrect(x + w / 30, by + bh * 62 / 100, w - w / 15, bh * 30 / 100, w / 16, band);
        ui_rect(cx - w / 60, by, cw + w / 30, bh * 12 / 100 + 1, hexc(0x3A3842));
        int sy = by + bh * 24 / 100, sh = bh * 14 / 100;
        ui_rrect(PX(9), sy, w * 13 / 100, sh, sh / 2, hexc(0x8B8896));
        ui_rrect(PX(9), sy, w * 6 / 100, sh, sh / 2, c);
        ui_rrect(PX(78), sy, w * 13 / 100, sh, sh / 2, hexc(0x8B8896));
        ui_rrect(PX(85), sy, w * 6 / 100, sh, sh / 2, hexc(0xBDBAC6));
        int pw = w * 50 / 100, ph = h * 21 / 100, px = PX(46), py = y + h - ph;
        plat_line(PX(30), by + bh, px + pw * 15 / 100, py + ph / 2, lw1, cable);
        ui_rrect(px, py, pw, ph, ph / 2, light);
        con_dpad(px + pw * 20 / 100, py + ph / 2, ph * 26 / 100, hexc(0x3A3842));
        static const unsigned bcol[4] = { 0x3466B5, 0xD8B830, 0xC83B3B, 0x3E9E4F };
        int bx = px + pw * 78 / 100, byy = py + ph / 2, d = ph * 22 / 100, br = ph / 10 + 1;
        ui_circle(bx, byy - d, br, hexc(bcol[0])); ui_circle(bx + d, byy, br, hexc(bcol[2]));
        ui_circle(bx, byy + d, br, hexc(bcol[1])); ui_circle(bx - d, byy, br, hexc(bcol[3]));
        for (int k = 0; k < 2; k++) ui_rrect(px + pw * (40 + k * 10) / 100, py + ph * 50 / 100, pw * 8 / 100, ph / 9 + 1, 2, hexc(0x5A5866));
        break;
    }
    case CON_MD: {
        color_t ring = hexc(0x34353D), dome = hexc(0x26272D);
        int by = PY(30), bh = h * 42 / 100, cw = w * 56 / 100, ch = h * 34 / 100, cx = x + (w - cw) / 2;
        con_cart(cx, PY(2), cw, ch, r, hexc(0x2C2D33), c, name);
        ui_rrect(x, by + h / 50, w, bh, r, hexc(0x0E0F12));
        ui_rrect(x, by, w, bh, r, black);
        int dr = bh * 52 / 100, dcx = x + w / 2, dcy = by + bh * 36 / 100;
        ui_circle(dcx, dcy, dr, ring);
        ui_circle(dcx, dcy, dr * 86 / 100, dome);
        ui_rect(cx - w / 60, by, cw + w / 30, bh * 10 / 100 + 1, hexc(0x0E0F12));
        ui_circle(PX(9), by + bh * 30 / 100, bh / 20 + 1, hexc(0xE04848));
        ui_rrect(PX(6), by + bh * 56 / 100, w * 12 / 100, bh * 10 / 100, 2, ring);
        for (int k = 0; k < 4; k++) ui_rect(PX(80), by + bh * (30 + k * 12) / 100, w * 14 / 100, h / 140 + 1, ring);
        ui_rect(PX(14), by + bh * 84 / 100, w * 72 / 100, h / 140 + 1, c);
        int pw = w * 50 / 100, ph = h * 22 / 100, px = PX(46), py = y + h - ph;
        plat_line(PX(30), by + bh, px + pw * 15 / 100, py + ph / 2, lw1, cable);
        int lr = ph * 50 / 100;
        ui_circle(px + lr, py + ph - lr, lr, black);
        ui_circle(px + pw - lr, py + ph - lr, lr, black);
        ui_rrect(px + lr / 2, py, pw - lr, ph * 78 / 100, ph / 3, black);
        con_dpad(px + pw * 20 / 100, py + ph * 50 / 100, ph * 24 / 100, hexc(0x55565E));
        for (int k = 0; k < 3; k++) ui_circle(px + pw * (60 + k * 12) / 100, py + ph * (62 - k * 10) / 100, ph / 9 + 1, hexc(0x6A6B74));
        ui_rrect(px + pw * 44 / 100, py + ph * 26 / 100, pw * 9 / 100, ph / 9 + 1, 2, c);
        break;
    }
    case CON_SMS: {
        color_t top = hexc(0x2A2B31);
        int by = PY(32), bh = h * 40 / 100, cw = w * 36 / 100, ch = h * 30 / 100, cx = PX(16);
        con_cart(cx, PY(6), cw, ch, r, hexc(0x3A3B42), c, name);
        plat_quad(PX(4), by, PX(96), by, x + w, by + bh, x, by + bh, black);
        plat_quad(PX(4), by, PX(96), by, PX(98), by + bh * 30 / 100, PX(2), by + bh * 30 / 100, top);
        ui_rect(cx - w / 60, by, cw + w / 30, bh * 10 / 100 + 1, hexc(0x0E0F12));
        for (int k = 0; k < 3; k++) ui_rect(PX(54), by + bh * (46 + k * 14) / 100, w * 40 / 100, bh / 16 + 1, c);
        ui_rect(PX(8), by + bh * 56 / 100, w * 32 / 100, bh / 18 + 1, hexc(0x0A0A0C));
        ui_rrect(PX(8), by + bh * 74 / 100, w * 9 / 100, bh * 12 / 100, 2, c);
        int pw = w * 46 / 100, ph = h * 20 / 100, px = PX(48), py = y + h - ph;
        plat_line(PX(30), by + bh, px + pw * 15 / 100, py + ph / 2, lw1, cable);
        ui_rrect(px, py, pw, ph, ph / 7 + 1, black);
        con_dpad(px + pw * 22 / 100, py + ph / 2, ph * 26 / 100, hexc(0x5A5B63));
        int bs = ph * 26 / 100;
        for (int k = 0; k < 2; k++) ui_rrect(px + pw * (62 + k * 18) / 100 - bs / 2, py + ph / 2 - bs / 2, bs, bs, 2, hexc(0x8A8B93));
        break;
    }
    default: {   /* CON_PCE */
        color_t white = hexc(0xECEAE4), shade = hexc(0xC9C6BE);
        int bw = w * 56 / 100, bh = h * 46 / 100, bx = PX(6), by = PY(34);
        int cw = bw * 78 / 100, ch = h * 30 / 100;
        ui_rrect(bx + (bw - cw) / 2, by - ch * 74 / 100, cw, ch, 3, mix(c, rgba(255, 255, 255, 255), 0.15f));
        con_label(bx + (bw - cw) / 2 + cw * 8 / 100, by - ch * 64 / 100, cw * 84 / 100, ch * 44 / 100, 2, c, name);
        ui_rrect(bx, by + h / 50, bw, bh, r / 2 + 1, shade);
        ui_rrect(bx, by, bw, bh, r / 2 + 1, white);
        ui_rect(bx + bw * 10 / 100, by, bw * 80 / 100, bh * 8 / 100 + 1, hexc(0x2C2C30));
        ui_rect(bx, by + bh * 70 / 100, bw, bh * 30 / 100, shade);
        ui_rrect(bx + bw * 8 / 100, by + bh * 30 / 100, bw * 20 / 100, bh * 12 / 100, 2, c);
        ui_rrect(bx + bw - bw * 8 / 100 - bw * 18 / 100, by + bh * 76 / 100, bw * 18 / 100, bh * 14 / 100, 2, hexc(0x3A3A40));
        ui_rrect(bx + bw, by + bh * 20 / 100, w * 6 / 100, bh * 60 / 100, 2, hexc(0x3A3A40));
        int pw = w * 40 / 100, ph = h * 19 / 100, px = PX(58), py = y + h - ph;
        plat_line(bx + bw / 2, by + bh, px + pw * 15 / 100, py + ph / 2, lw1, cable);
        ui_rrect(px, py, pw, ph, ph / 6 + 1, white);
        con_dpad(px + pw * 22 / 100, py + ph / 2, ph * 26 / 100, hexc(0x2C2C30));
        for (int k = 0; k < 2; k++) ui_circle(px + pw * (66 + k * 16) / 100, py + ph * (58 - k * 10) / 100, ph / 9 + 1, hexc(0x2C2C30));
        for (int k = 0; k < 2; k++) ui_rrect(px + pw * (60 + k * 16) / 100, py + ph * 18 / 100, pw * 9 / 100, ph / 10 + 1, 1, c);
        break;
    }
    }
}
#undef PX
#undef PY

/* console a disco: coperchio rotondo con il disco, nome su una striscia del corpo, controller */
static void draw_disc_console(int x, int y, int w, int h, color_t c, const char *name, bool white) {
    /* white: corpo bianco, coperchio più piccolo e controller con la memoria al centro (Dreamcast) */
    color_t body = white ? hexc(0xE8E7E2) : mix(c, hexc(0xC9C7C2), 0.55f), edge = mix(body, rgba(0, 0, 0, 255), 0.25f), pad = white ? hexc(0xD9D8D2) : hexc(0x2A2D36);
    int r = w / 22 + 1, by = y + h * 8 / 100, bh = h * 58 / 100;
    ui_rrect(x, by + h / 40, w, bh, r, edge);
    ui_rrect(x, by, w, bh, r, body);
    /* coperchio e disco */
    int lr = bh * (white ? 30 : 36) / 100, lx = x + w * (white ? 30 : 34) / 100, ly = by + bh * 44 / 100;
    ui_circle(lx, ly, lr + lr / 8, edge);
    ui_circle(lx, ly, lr, mix(body, rgba(255, 255, 255, 255), 0.25f));
    ui_circle(lx, ly, lr * 72 / 100, hexc(0xB9C3D0));
    ui_circle(lx, ly, lr * 50 / 100, hexc(0xD8DEE6));
    ui_circle(lx, ly, lr * 18 / 100, edge);
    /* striscia col nome e tasti */
    int sx = x + w * 60 / 100, sw = w * 35 / 100, sh = bh * 36 / 100, sy = by + bh * 10 / 100;
    ui_rrect(sx, sy, sw, sh, r / 2 + 1, c);
    char up[64];
    snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    plat_text(up, sx + sw / 2, sy, sw - sw / 10, sh, rgba(255, 255, 255, 255));
    for (int k = 0; k < 2; k++) ui_circle(sx + sw * (25 + k * 50) / 100, by + bh * 64 / 100, bh / 11 + 1, edge);
    ui_rect(sx, by + bh * 84 / 100, sw, bh / 30 + 1, edge);
    /* controller */
    int pw = w * 46 / 100, ph = h * 24 / 100, px = x + w * 50 / 100, py = y + h - ph;
    plat_line(x + w * 20 / 100, by + bh, px + pw * 15 / 100, py + ph / 3, w / 120 + 1, pad);
    ui_rrect(px, py, pw, ph, ph / 3, pad);
    int dx = px + pw * 24 / 100, dy = py + ph / 2, arm = ph * 28 / 100, th = arm * 2 / 3 + 1;
    ui_rect(dx - arm, dy - th / 2, 2 * arm, th, edge);
    ui_rect(dx - th / 2, dy - arm, th, 2 * arm, edge);
    for (int k = 0; k < 4; k++)
        ui_circle(px + pw * 74 / 100 + (k == 1 ? ph / 4 : k == 3 ? -ph / 4 : 0), dy + (k == 0 ? -ph / 4 : k == 2 ? ph / 4 : 0), ph / 10 + 1, c);
    if (white) ui_rrect(px + pw * 42 / 100, py + ph * 12 / 100, pw * 18 / 100, ph * 46 / 100, 2, c);   /* memoria */
}

/* portatile per i giochi Linux */
/* PC da scrivania con un'avventura punta e clicca (ScummVM): monitor a tubo, scena con un
   personaggio e il cursore a freccia, tastiera */
static void draw_pc(int x, int y, int w, int h, color_t c) {
    color_t shell = hexc(0xD8D0BE), shell_d = hexc(0xA89F8C), scr = hexc(0x1C2A4A);
    int r = w / 28 + 1, mw = w * 72 / 100, mh = h * 62 / 100, mx = x + (w - mw) / 2;
    ui_rrect(mx, y, mw, mh, r, shell);
    int sx = mx + mw * 9 / 100, sy = y + mh * 9 / 100, sw = mw * 82 / 100, sh = mh * 70 / 100;
    ui_rrect(sx, sy, sw, sh, r, scr);
    ui_rect(sx, sy + sh * 62 / 100, sw, sh * 38 / 100, mix(c, scr, 0.35f));   /* terreno */
    ui_circle(sx + sw * 78 / 100, sy + sh * 24 / 100, sh / 10 + 1, hexc(0xF2E6A8));   /* luna */
    ui_rect(sx + sw * 30 / 100, sy + sh * 34 / 100, sw / 14 + 1, sh * 30 / 100, hexc(0xE9C9A0));   /* personaggio */
    ui_circle(sx + sw * 30 / 100 + sw / 28, sy + sh * 30 / 100, sw / 22 + 1, hexc(0xE9C9A0));
    int ax = sx + sw * 58 / 100, ay = sy + sh * 48 / 100, as = sh / 4;
    ui_triangle(ax, ay, ax, ay + as, ax + as * 7 / 10, ay + as * 7 / 10, hexc(0xFFFFFF));   /* cursore */
    ui_rect(mx + mw * 8 / 100, y + mh * 86 / 100, mw * 12 / 100, mh / 20 + 1, shell_d);
    int stand_w = mw * 30 / 100;
    ui_rect(x + (w - stand_w) / 2, y + mh, stand_w, h * 5 / 100, shell_d);
    int kw = w * 86 / 100, kh = h * 20 / 100, kx = x + (w - kw) / 2, ky = y + h - kh;
    ui_rrect(kx, ky, kw, kh, r / 2 + 1, shell);
    for (int row = 0; row < 3; row++)
        for (int k = 0; k < 14; k++) ui_rect(kx + kw * 4 / 100 + k * kw * 92 / 100 / 14, ky + kh * (18 + row * 26) / 100, kw * 92 / 100 / 14 - 2, kh / 6 + 1, shell_d);
}

static void draw_laptop(int x, int y, int w, int h) {
    color_t shell = hexc(0xB9AE9C), shell_d = hexc(0x8E8474), scr = hexc(0x1C1A28);
    int r = w / 30 + 1, lx = x + w * 9 / 100, lw = w * 82 / 100, lh = h * 64 / 100;
    ui_rrect(lx, y, lw, lh, r, shell);
    int bx = lx + lw * 8 / 100, by = y + lh * 10 / 100, bw = lw * 84 / 100, bh = lh * 76 / 100;
    ui_rrect(bx, by, bw, bh, r, hexc(0x3A3646));
    ui_rrect(bx + bw / 30 + 1, by + bh / 25 + 1, bw - 2 * (bw / 30 + 1), bh - 2 * (bh / 25 + 1), r / 2 + 1, scr);
    int fh = bh * 26 / 100;
    TTF_Font *f = TTF_FontHeight(UI.f_title) <= fh ? UI.f_title : TTF_FontHeight(UI.f_normal) <= fh ? UI.f_normal : UI.f_small;
    ui_text(f, "LINUX", bx + bw / 10, by + bh * 16 / 100, hexc(0xD0C8E8), ALIGN_LEFT);
    if (TTF_FontHeight(UI.f_small) < bh * 42 / 100) {
        char fit[64];
        ui_text_fit(UI.f_small, T("plat.native"), bw * 8 / 10, fit, sizeof fit);
        ui_text(UI.f_small, fit, bx + bw / 10, by + bh * 16 / 100 + TTF_FontHeight(f) + bh / 20, hexc(0x9AE6A0), ALIGN_LEFT);
    }
    int ky = y + lh, kh = h * 24 / 100;
    plat_quad(lx - w * 2 / 100, ky, lx + lw + w * 2 / 100, ky, x + w, ky + kh, x, ky + kh, shell);
    for (int row = 0; row < 3; row++) {
        float tt = (row + 0.5f) / 3.5f;
        int rx0 = (int)(lx + (x - lx) * tt) + w * 6 / 100, rx1 = (int)(lx + lw + (x + w - lx - lw) * tt) - w * 6 / 100;
        int ry = ky + kh * (14 + row * 27) / 100, kw = (rx1 - rx0) / 13;
        for (int k = 0; k < 12; k++) ui_rect(rx0 + k * (rx1 - rx0) / 12, ry, kw, kh / 6 + 1, hexc(0x4A4652));
    }
    ui_rrect(x, ky + kh, w, h * 5 / 100 + 1, 2, shell_d);
}

/* icone in un riquadro: tutti (quattro quadrati), preferiti (stella), collezione (schede) */
static void draw_plat_icon(int kind, int x, int y, int w, int h, color_t c) {
    int s = (w < h ? w : h) * 70 / 100, bx = x + (w - s) / 2, by = y + (h - s) / 2, r = s / 10;
    color_t beige = hexc(0xD2BC92), panel = mix(c, rgba(255, 255, 255, 255), 0.08f);
    ui_rrect(bx, by, s, s, r, panel);
    ui_rborder(bx, by, s, s, r, s / 60 + 1, mix(c, beige, 0.45f));
    int cx = bx + s / 2, cy = by + s / 2;
    if (kind == TAB_ALL) {
        int q = s * 22 / 100, g = s * 6 / 100;
        for (int k = 0; k < 4; k++) ui_rrect(cx - q - g / 2 + (k % 2) * (q + g), cy - q - g / 2 + (k / 2) * (q + g), q, q, q / 6 + 1, beige);
    } else if (kind == TAB_FAV) {
        ui_star(cx, cy, s * 34 / 100, beige);
    } else {
        for (int k = 2; k >= 0; k--) {
            int cw = s * 50 / 100, ch = s * 36 / 100;
            ui_rrect(cx - cw / 2 + k * s / 14, cy - ch / 2 - k * s / 14, cw, ch, s / 30 + 1, k ? mix(beige, c, 0.35f * k) : beige);
        }
    }
}

/* l'illustrazione di un emulatore (console, portatile, lettore di dischi o cabinato) nel riquadro
   dato, con le sue proporzioni; serve anche ai pacchetti dello Store non ancora installati */
static void draw_emu_art(const emulator_t *em, color_t c, const char *label, int x, int y, int w, int h) {
    const char *art = em->art;
    if (!strcmp(art, "handheld")) {
        int ah = h * 92 / 100, aw = ah * 60 / 100;
        if (aw > w) { aw = w; ah = w * 100 / 60; }
        draw_handheld(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c, em->shortname, false);
    } else if (!strcmp(art, "handheld-wide")) {
        int aw = w, ah = w * 46 / 100;
        if (ah > h * 70 / 100) { ah = h * 70 / 100; aw = ah * 100 / 46; }
        draw_handheld(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c, em->shortname, true);
    } else if (!strcmp(art, "pc")) {
        int aw = w, ah = w * 84 / 100;
        if (ah > h * 86 / 100) { ah = h * 86 / 100; aw = ah * 100 / 84; }
        draw_pc(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c);
    } else if (!strcmp(art, "disc") || !strcmp(art, "disc-white")) {
        int aw = w, ah = w * 80 / 100;
        if (ah > h * 80 / 100) { ah = h * 80 / 100; aw = ah * 100 / 80; }
        draw_disc_console(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c, em->shortname, !strcmp(art, "disc-white"));
    } else if (em && em->console) {
        int aw = w, ah = w * 86 / 100;
        if (ah > h * 80 / 100) { ah = h * 80 / 100; aw = ah * 100 / 86; }
        static const char *const kinds[] = { "nes", "snes", "md", "sms", "pce" };
        int kind = -1;
        for (int k = 0; k < 5; k++) if (!strcmp(art, kinds[k])) kind = k;
        if (kind >= 0) draw_home_console(kind, x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c, em->shortname);
        else draw_console(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c, em->shortname);
    } else {
        int ah = h, aw = h * 62 / 100;
        if (aw > w) { aw = w; ah = w * 100 / 62; }
        draw_cabinet(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah, c, label);
    }
}

/* l'illustrazione di una categoria dentro il riquadro dato, con le sue proporzioni */
static void draw_plat_art(int tab, int x, int y, int w, int h) {
    color_t c = plat_color(tab);
    const emulator_t *em = is_plan(tab) ? &PLANNED[tab - TAB_PLAN] : tab >= TAB_EMU && tab < TAB_COLL ? &EMUS[tab - TAB_EMU] : NULL;
    if (em) draw_emu_art(em, c, tab_label(tab), x, y, w, h);
    else if (tab == TAB_LINUX) {
        int aw = w, ah = w * 80 / 100;
        if (ah > h * 70 / 100) { ah = h * 70 / 100; aw = ah * 100 / 80; }
        draw_laptop(x + (w - aw) / 2, y + (h - ah) / 2, aw, ah);
    } else draw_plat_icon(tab == TAB_ALL || tab == TAB_FAV ? tab : TAB_COLL, x, y, w, h, c);
}

/* Tessera e Onda: schede colorate a righe, quella scelta più grande */
static void plat_draw_cards(int y, int bottom) {
    theme_t *t = &UI.th;
    int avail = bottom - y, ch = avail * 94 / 100, cw = ch * 82 / 100;
    if (cw > UI.w * 30 / 100) { cw = UI.w * 30 / 100; ch = cw * 100 / 82; }
    int sw = cw * 64 / 100, gap = M(0.02f), cy = y + avail / 2;
    int order[MAX_EMUS + 40], n = nptabs;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int a = 0; a < n; a++)   /* prima i lontani, poi i vicini: la scelta sta sopra */
        for (int b = a + 1; b < n; b++)
            if (fabsf(order[b] - plat_anim) > fabsf(order[a] - plat_anim)) { int tmp = order[a]; order[a] = order[b]; order[b] = tmp; }
    char cnt[48];
    for (int o = 0; o < n; o++) {
        int i = order[o];
        float d = i - plat_anim, ad = fabsf(d), k = ad < 1 ? 1 - ad : 0;
        int w = sw + (int)((cw - sw) * k), h = w * 100 / 82;
        float off = ad <= 1 ? ad * ((cw + sw) / 2.0f + gap) : (cw + sw) / 2.0f + gap + (ad - 1) * (sw + gap);
        int x = UI.w / 2 + (int)(d < 0 ? -off : off) - w / 2, yy = cy - h / 2;
        if (x > UI.w || x + w < 0) continue;
        color_t c = plat_color(ptabs[i]);
        ui_shadow(x, yy + M(0.008f), w, h, M(0.006f), M(0.02f), t->shadow);
        ui_rect(x, yy, w, h, c);
        ui_clip_push(x, yy, w, h);
        int sp = M(0.032f), lt = M(0.005f) + 1;
        for (int s = -h; s < w; s += sp) plat_line(x + s, yy, x + s + h, yy + h, lt, mix(c, rgba(0, 0, 0, 255), 0.32f));
        ui_clip_pop();
        int fh = h * 23 / 100, pad = w * 8 / 100;
        draw_plat_art(ptabs[i], x + pad, yy + pad, w - 2 * pad, h - fh - pad - pad / 2);
        if (is_plan(ptabs[i])) ui_rect(x, yy, w, h - fh, with_alpha(c, 120));   /* in arrivo: velato */
        ui_rect(x, yy + h - fh, w, fh, hexc(0x151822));
        TTF_Font *fnm = k > 0.5f ? UI.f_normal : UI.f_small;
        char fit[120];
        ui_text_fit(fnm, tab_label(ptabs[i]), w - pad, fit, sizeof fit);
        int ty = yy + h - fh + (fh - TTF_FontHeight(fnm) - TTF_FontHeight(UI.f_small)) / 2;
        ui_text(fnm, fit, x + w / 2, ty, rgba(0xF2, 0xF2, 0xF2, 255), ALIGN_CENTER);
        plat_count_text(ptabs[i], cnt, sizeof cnt);
        ui_text(UI.f_small, cnt, x + w / 2, ty + TTF_FontHeight(fnm), rgba(0xC8, 0xCC, 0xD4, 255), ALIGN_CENTER);
        /* il raggio del bordo non può essere più piccolo dello spessore */
        int bt = M(0.006f) + 1, o = M(0.004f);
        if (i == plat_sel) ui_rborder(x - o, yy - o, w + 2 * o, h + 2 * o, bt + 2, bt, t->accent);
        else ui_rborder(x, yy, w, h, 3, 2, with_alpha(rgba(255, 255, 255, 255), 70));
    }
}

/* Cabinato: vetrina di cabinati, i nomi sotto quelli di lato, la barra della scelta in basso */
static void plat_draw_showcase(int y, int bottom) {
    theme_t *t = &UI.th;
    TTF_Font *ft = UI.f_title, *fn = UI.f_normal, *fs = UI.f_small;
    int mx = M(t->margin), W = UI.w - 2 * mx, fsh = TTF_FontHeight(fs), fth = TTF_FontHeight(ft);
    char buf[64];
    snprintf(buf, sizeof buf, "%d / %d", plat_sel + 1, nptabs);
    ui_text(fs, buf, mx, y, t->text_dim, ALIGN_LEFT);
    int bar_h = fth + M(0.035f), bar_y = bottom - bar_h, top = y + fsh + M(0.01f), art_b = bar_y - M(0.03f);
    int ah = art_b - top, step = UI.w * 34 / 100;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nptabs; i++) {
            float d = i - plat_anim, ad = fabsf(d), k = ad < 1 ? 1 - ad : 0;
            if ((pass == 1) != (k > 0.5f)) continue;   /* la scelta per ultima, sopra le altre */
            int h = (int)(ah * (0.58f + 0.42f * k)) - (k > 0.5f ? 0 : fsh + M(0.015f));
            int w = h * 62 / 100;
            if (ptabs[i] == TAB_LINUX || ptabs[i] < TAB_EMU || (ptabs[i] >= TAB_COLL && !is_plan(ptabs[i]))) w = h;
            int cx = UI.w / 2 + (int)(d * step), x = cx - w / 2, yy = art_b - h - (k > 0.5f ? 0 : fsh + M(0.015f));
            if (x > UI.w || x + w < 0) continue;
            draw_plat_art(ptabs[i], x, yy, w, h);
            if (k <= 0.5f) {
                char fit[100];
                ui_text_fit(fs, tab_label(ptabs[i]), step - M(0.02f), fit, sizeof fit);
                ui_text(fs, fit, cx, art_b - fsh, t->text, ALIGN_CENTER);
            }
        }
    color_t c = plat_color(ptabs[plat_sel]);
    ui_rrect(mx, bar_y, W, bar_h, M(t->panel_radius), c);
    char fit[120], cnt[48];
    plat_count_text(ptabs[plat_sel], cnt, sizeof cnt);
    int cwid = ui_text_w(fn, cnt);
    ui_text_fit(ft, tab_label(ptabs[plat_sel]), W - cwid - M(0.08f), fit, sizeof fit);
    ui_text(ft, fit, mx + M(0.025f), bar_y + (bar_h - fth) / 2, rgba(0xF6, 0xEC, 0xE2, 255), ALIGN_LEFT);
    ui_text(fn, cnt, mx + W - M(0.025f), bar_y + (bar_h - TTF_FontHeight(fn)) / 2, rgba(0xF6, 0xEC, 0xE2, 255), ALIGN_RIGHT);
}

static void plat_open(int sel) {
    plat_sel = sel >= 0 && sel < nptabs ? sel : 0;
    plat_anim = (float)plat_sel;
    go(SCR_PLATFORMS);
}

static void plat_draw(void) {
    theme_t *t = &UI.th;
    topbar_label(T("plat.title"));
    if (plat_sel >= nptabs) plat_sel = nptabs ? nptabs - 1 : 0;
    plat_anim = ui_approach(plat_anim, (float)plat_sel);
    int y = M(t->topbar_h) + M(0.03f), bottom = UI.h - ui_keybar_h() - M(0.03f);
    if (nptabs) {
        if (is_list_layout()) plat_draw_showcase(y, bottom);
        else plat_draw_cards(y, bottom);
    }
    /* console in arrivo: niente da aprire */
    bool plan = nptabs && is_plan(ptabs[plat_sel]);
    const char *h[] = { "A", T("plat.open"), NULL, "B", T("common.back"), NULL, "START", T("common.settings"), NULL };
    ui_keybar(plan ? h + 3 : h, plan ? 6 : 9);
}

/* ------------------------------------------------------ vetrina (layout "vetrina")
   La home come la copertina di un fumetto: in alto l'illustrazione grande della voce scelta, su
   una fascia obliqua del suo colore, con il nome in grande; in basso la striscia delle voci
   (ultima partita, Tutti, Preferiti, piattaforme, collezioni, poi App, Store, Impostazioni,
   Spegni) e il contatore. Le illustrazioni sono quelle disegnate per le Piattaforme. */
enum { VK_GAME, VK_TAB, VK_MENU };
typedef struct { int kind, v; } vt_entry_t;
static int vt_sel;
static float vt_off, vt_off_target;

static int vt_entries(vt_entry_t *e, int max) {
    int n = 0;
    if (home_last_game() >= 0 && n < max) e[n++] = (vt_entry_t){ VK_GAME, 0 };
    for (int i = 0; i < nptabs && n < max; i++) e[n++] = (vt_entry_t){ VK_TAB, i };
    for (int i = 0; i < nhmenu && n < max; i++)
        if (hmenu[i] == HM_APPS || hmenu[i] == HM_STORE || hmenu[i] == HM_SETTINGS || hmenu[i] == HM_POWER) e[n++] = (vt_entry_t){ VK_MENU, hmenu[i] };
    return n;
}

/* la piattaforma dell'ultima partita (TAB_EMU + emulatore, TAB_LINUX) e se ha un'immagine */
static int vt_game_tab(int gi) {
    if (gi < 0) return -1;
    if (!strcmp(games[gi].core, "linux")) return TAB_LINUX;
    for (int i = 0; i < NEMUS; i++) if (!strcmp(EMUS[i].id, games[gi].core)) return TAB_EMU + i;
    return -1;
}
static bool vt_game_image(int gi) {
    return gi >= 0 && ((gi == resume_idx && ui_image(resume_png)) || media_image(&games[gi], UI.th.cover_types, UI.th.ncover));
}

static color_t vt_color(const vt_entry_t *e) {
    if (e->kind == VK_TAB) return plat_color(ptabs[e->v]);
    if (e->kind == VK_GAME) {
        int tab = vt_game_tab(home_last_game());
        return tab >= 0 ? plat_color(tab) : mix(UI.th.accent, rgba(0, 0, 0, 255), 0.45f);
    }
    switch (e->v) {
    case HM_APPS: return hexc(0x4B3C86);
    case HM_STORE: return hexc(0x1F6E8C);
    case HM_SETTINGS: return hexc(0x4A525E);
    default: return hexc(0x8E2A2A);
    }
}

static const char *vt_label(const vt_entry_t *e) {
    if (e->kind == VK_TAB) return tab_label(ptabs[e->v]);
    return hm_label(e->kind == VK_GAME ? HM_GAME : e->v);
}

static int vt_icon(int m) { return m == HM_APPS ? IC_APPS : m == HM_STORE ? IC_STORE : m == HM_SETTINGS ? IC_SETTINGS : IC_POWER; }

/* l'immagine di una voce dentro un riquadro: illustrazione, gioco o simbolo */
static void vt_art(const vt_entry_t *e, int x, int y, int w, int h, int r) {
    color_t c = vt_color(e);
    if (e->kind == VK_TAB) draw_plat_art(ptabs[e->v], x, y, w, h);
    else if (e->kind == VK_GAME) {   /* senza immagine: l'illustrazione della sua piattaforma */
        int gi = home_last_game(), tab = vt_game_tab(gi);
        if (vt_game_image(gi) || tab < 0) home_image(gi, x, y, w, h, r);
        else draw_plat_art(tab, x, y, w, h);
    }
    else {
        int sz = w < h ? w : h;
        draw_icon_glyph(vt_icon(e->v), x + w / 2, y + h / 2, sz * 9 / 10, rgba(0xF2, 0xF2, 0xF2, 255), c);
    }
}

/* righe oblique nel colore più scuro, dentro un rettangolo */
static void vt_stripes(int x, int y, int w, int h, color_t c, int sp) {
    ui_clip_push(x, y, w, h);
    int lt = M(0.005f) + 1;
    for (int s = -h; s < w; s += sp) plat_line(x + s, y, x + s + h, y + h, lt, mix(c, rgba(0, 0, 0, 255), 0.3f));
    ui_clip_pop();
}

/* il nome in grande, come un titolo di fumetto: contorno scuro, ombra e riempimento */
static void vt_title(const char *s, int x, int y, int maxw, color_t fill, color_t edge) {
    TTF_Font *f = ui_font(UI.th.title_font_path, M(0.105f));
    if (!f) f = UI.f_big;
    char l1[96], l2[96] = "";
    snprintf(l1, sizeof l1, "%s", s);
    if (ui_text_w(f, l1) > maxw) {   /* due righe, spezzando allo spazio più vicino al centro */
        size_t n = strlen(l1), best = 0;
        for (size_t i = 1; i < n; i++)
            if (l1[i] == ' ' && (best == 0 || (i > n / 2 ? i - n / 2 : n / 2 - i) < (best > n / 2 ? best - n / 2 : n / 2 - best))) best = i;
        if (best) { snprintf(l2, sizeof l2, "%s", l1 + best + 1); l1[best] = 0; }
        if (ui_text_w(f, l1) > maxw || ui_text_w(f, l2) > maxw) {
            f = UI.f_big;
            if (!best) { char fit[96]; ui_text_fit(f, s, maxw, fit, sizeof fit); snprintf(l1, sizeof l1, "%s", fit); }
        }
    }
    int fh = TTF_FontHeight(f), o = M(0.004f) + 1, sh = M(0.009f) + 1;
    const char *lines[2] = { l1, l2 };
    for (int k = 0; k < 2 && lines[k][0]; k++) {
        int yy = y + k * (fh * 88 / 100);
        ui_text(f, lines[k], x + sh, yy + sh, with_alpha(edge, 120), ALIGN_LEFT);
        for (int dx = -o; dx <= o; dx += o)
            for (int dy = -o; dy <= o; dy += o)
                if (dx || dy) ui_text(f, lines[k], x + dx, yy + dy, edge, ALIGN_LEFT);
        ui_text(f, lines[k], x, yy, fill, ALIGN_LEFT);
    }
}

static void vt_draw(void) {
    theme_t *t = &UI.th;
    vt_entry_t e[MAX_EMUS + 48];
    int n = vt_entries(e, MAX_EMUS + 48);
    if (vt_sel >= n) vt_sel = n ? n - 1 : 0;
    int mx = M(t->margin), kb = ui_keybar_h(), fsh = TTF_FontHeight(UI.f_small);
    int th = M(0.17f), tw = th * 5 / 4, gap = M(0.016f), big = th * 118 / 100, bw = big * 5 / 4;
    int cnt_y = UI.h - kb - fsh - M(0.012f), strip_b = cnt_y - M(0.01f), strip_t = strip_b - big;
    int hero_b = strip_t - M(0.02f);
    /* fascia obliqua del colore della voce, con le righe, sotto l'illustrazione */
    if (n) {
        const vt_entry_t *s = &e[vt_sel];
        color_t c = vt_color(s), edge = mix(c, rgba(0, 0, 0, 255), 0.55f);
        int xa = UI.w * 44 / 100, xb = UI.w * 34 / 100;   /* in alto e in basso */
        plat_quad(xa, 0, UI.w, 0, UI.w, hero_b, xb, hero_b, c);
        {   /* righe oblique solo dentro la fascia (lo sfondo può essere sfumato: niente coperture) */
            int lt = M(0.005f) + 1, sp = M(0.04f);
            float k = 1.0f - (float)(xb - xa) / (float)hero_b;   /* > 1: il bordo sinistro va verso sinistra */
            color_t dark = mix(c, rgba(0, 0, 0, 255), 0.3f);
            ui_clip_push(xb, 0, UI.w - xb, hero_b);
            for (int x0 = xb - hero_b; x0 < UI.w; x0 += sp) {
                float y0 = (xa - x0) / k;
                if (y0 < 0) y0 = 0;
                if (y0 >= hero_b) continue;
                plat_line(x0 + (int)y0 + lt, (int)y0, x0 + hero_b, hero_b, lt, dark);
            }
            ui_clip_pop();
        }
        int bt = M(0.008f) + 1;
        plat_line(xa - bt / 2, 0, xb - bt / 2, hero_b, bt, rgba(0xF2, 0xF2, 0xF2, 255));
        ui_rect(xb, hero_b - bt / 2, UI.w - xb, bt, rgba(0xF2, 0xF2, 0xF2, 255));
        /* l'illustrazione, grande, nella fascia */
        int top = M(t->topbar_h), ah = hero_b - top - M(0.03f), aw = UI.w - xa - mx;
        if (s->kind == VK_GAME && vt_game_image(home_last_game())) {
            int gw = aw, gh = gw * 3 / 4;
            if (gh > ah) { gh = ah; gw = gh * 4 / 3; }
            int gx = xa + (UI.w - xa - mx - gw) / 2 + M(0.02f), gy = top + (ah - gh) / 2, b = M(0.008f) + 1;
            ui_shadow(gx, gy + M(0.01f), gw, gh, M(0.008f), M(0.03f), t->shadow);
            ui_rrect(gx - b, gy - b, gw + 2 * b, gh + 2 * b, M(0.01f), rgba(0xF2, 0xF2, 0xF2, 255));
            vt_art(s, gx, gy, gw, gh, M(0.008f));
        } else vt_art(s, xa + M(0.02f), top, aw, ah, 0);
        /* il nome in grande e sotto quanti giochi o di che gioco si tratta */
        int tx = mx, ty = top + M(0.05f), maxw = xb - mx - M(0.02f);
        vt_title(vt_label(s), tx, ty, maxw, t->highlight, edge);
        char sub[160] = "";
        if (s->kind == VK_TAB) plat_count_text(ptabs[s->v], sub, sizeof sub);
        else if (s->kind == VK_GAME) snprintf(sub, sizeof sub, "%s", games[home_last_game()].title);
        if (sub[0]) {
            char fit[160];
            ui_text_fit(UI.f_normal, sub, maxw, fit, sizeof fit);
            ui_text(UI.f_normal, fit, tx, hero_b - TTF_FontHeight(UI.f_normal) - M(0.04f), t->text, ALIGN_LEFT);
        }
    }
    {   /* orologio, Wi-Fi e batteria sopra l'illustrazione, su una pillola del colore di fondo */
        int ph = TTF_FontHeight(UI.f_normal) + M(0.02f), pw = M(0.31f) + ui_text_w(UI.f_normal, "100%");
        ui_rrect(UI.w - mx - pw + M(0.015f), M(t->topbar_h) / 2 - ph / 2, pw, ph, ph / 2, with_alpha(t->bg, 220));
    }
    ui_topbar(NULL);
    /* la striscia: la voce scelta più grande; scorre per restare in vista */
    int xs = 0, sel_x = 0;
    for (int i = 0; i < n; i++) { if (i == vt_sel) sel_x = xs; xs += (i == vt_sel ? bw : tw) + gap; }
    int avail = UI.w - 2 * mx;
    if (sel_x + bw - vt_off_target > avail) vt_off_target = (float)(sel_x + bw - avail);
    if (sel_x < vt_off_target) vt_off_target = (float)sel_x;
    if (vt_off_target < 0) vt_off_target = 0;
    vt_off = ui_approach(vt_off, vt_off_target);
    int x = mx - (int)vt_off;
    for (int i = 0; i < n; i++) {
        bool sel = i == vt_sel;
        int w = sel ? bw : tw, h = sel ? big : th, y = strip_b - h;
        if (x < UI.w && x + w > 0) {
            color_t c = vt_color(&e[i]);
            ui_shadow(x, y + M(0.006f), w, h, M(0.006f), M(0.016f), t->shadow);
            int lh = TTF_FontHeight(UI.f_small) + M(0.012f);
            if (e[i].kind == VK_GAME && vt_game_image(home_last_game())) vt_art(&e[i], x, y, w, h - lh, 0);
            else {
                ui_rect(x, y, w, h, c);
                vt_stripes(x, y, w, h - lh, c, M(0.03f));
                int pad = w / 10;
                vt_art(&e[i], x + pad, y + pad / 2, w - 2 * pad, h - lh - pad, 0);
                if (e[i].kind == VK_TAB && is_plan(ptabs[e[i].v])) ui_rect(x, y, w, h - lh, with_alpha(c, 120));
            }
            ui_rect(x, y + h - lh, w, lh, hexc(0x151822));
            char fit[96];
            ui_text_fit(UI.f_small, vt_label(&e[i]), w - M(0.012f), fit, sizeof fit);
            ui_text(UI.f_small, fit, x + w / 2, y + h - lh + (lh - TTF_FontHeight(UI.f_small)) / 2, rgba(0xF2, 0xF2, 0xF2, 255), ALIGN_CENTER);
            int bt = M(0.006f) + 1, o = M(0.004f);
            if (sel) ui_rborder(x - o, y - o, w + 2 * o, h + 2 * o, bt + 2, bt, t->accent);
            else ui_rborder(x, y, w, h, 3, 2, with_alpha(rgba(255, 255, 255, 255), 70));
        }
        x += w + gap;
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%02d/%02d", n ? vt_sel + 1 : 0, n);
    ui_text(UI.f_small, buf, UI.w - mx, cnt_y, t->text_dim, ALIGN_RIGHT);
    /* tasti: X e Y solo sull'ultima partita */
    bool game = n && e[vt_sel].kind == VK_GAME;
    int g = game ? home_last_game() : -1;
    const char *act = T(game ? (g == resume_idx ? "home.resume" : "common.play") : n && e[vt_sel].kind == VK_TAB ? "plat.open" : "common.open");
    const char *h1[] = { "A", act, NULL, "X", T("common.info"), "", "Y", T(g >= 0 && games[g].fav ? "common.unfavorite" : "common.favorite"), "",
                         "START", T("common.settings"), NULL };
    const char *h2[] = { "A", act, NULL, "START", T("common.settings"), NULL };
    if (g >= 0) ui_keybar(h1, 12); else ui_keybar(h2, 6);
}

static void vt_input(void) {
    input_t *in = &UI.in;
    vt_entry_t e[MAX_EMUS + 48];
    int n = vt_entries(e, MAX_EMUS + 48), old = vt_sel;
    if (in->pressed[BTN_LEFT] && vt_sel > 0) vt_sel--;
    if (in->pressed[BTN_RIGHT] && vt_sel < n - 1) vt_sel++;
    if (in->pressed[BTN_L]) vt_sel = vt_sel > 5 ? vt_sel - 5 : 0;
    if (in->pressed[BTN_R]) vt_sel = vt_sel + 5 < n ? vt_sel + 5 : (n ? n - 1 : 0);
    if (vt_sel != old) ui_sound(SND_MOVE);
    if (!n) { if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); } return; }
    const vt_entry_t *s = &e[vt_sel];
    int g = s->kind == VK_GAME ? home_last_game() : -1;
    if (in->pressed[BTN_A]) {
        if (s->kind == VK_GAME) { if (g >= 0) home_play(g); }
        else if (s->kind == VK_MENU) home_do(s->v);
        else if (is_plan(ptabs[s->v])) ui_sound(SND_ERROR);
        else if (s->v < ntabs) {
            tab_sel = s->v;
            lib_sel = 0;
            lib_scroll = lib_scroll_target = 0;
            build_view();
            go(SCR_LIBRARY);
            ui_sound(SND_OK);
        }
        return;
    }
    if (in->pressed[BTN_X] && g >= 0) { open_detail(g); return; }
    if (in->pressed[BTN_Y] && g >= 0) {
        games_toggle_fav(UI.root, &games[g]);
        ui_toast(games[g].fav ? T("home.fav_added") : T("home.fav_removed"));
        build_tabs();
        build_carousel();
        ui_sound(SND_OK);
    }
    if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); }
}

/* ------------------------------------------------------ galleria (layout "galleria")
   Come il menu di una console: in alto il titolo e le schede (L/R) Preferiti, Recenti,
   Piattaforme; a sinistra il nome grande della scelta e quanti giochi; al centro un carosello di
   schede: la piattaforma col suo disegno, sotto produttore e anno (o il gioco con la sua
   immagine); in fondo, sulla fascia del cielo, i tasti. Con Piattaforme anche App, Store,
   Impostazioni e Spegni, come nella vetrina. */
enum { GT_FAV, GT_RECENT, GT_PLAT, GT_N };
static int gl_tab = GT_PLAT, gl_sel[GT_N];
static float gl_off, gl_off_target;

static int gl_games(int tab, int *out, int max) {
    int n = 0;
    if (tab == GT_RECENT) {
        for (int r = 1; r <= 40 && n < max; r++)
            for (int i = 0; i < ngames; i++)
                if (games[i].recent == r && !games[i].hidden && !games[i].missing) { out[n++] = i; break; }
    } else
        for (int i = 0; i < ngames && n < max; i++)
            if (games[i].fav && !games[i].hidden && !games[i].missing) out[n++] = i;
    return n;
}

/* una scheda: cornice scura, nome in alto, immagine al centro, sotto produttore e anno */
static void gl_card(int x, int y, int w, int h, bool sel, const char *name, const char *maker, const char *year,
                    const vt_entry_t *e, int gi) {
    theme_t *t = &UI.th;
    color_t frame = hexc(0x14172A), light = rgba(0xF2, 0xF2, 0xF2, 255);
    int r = M(0.012f), pad = w * 6 / 100, nh = TTF_FontHeight(UI.f_normal), sh = TTF_FontHeight(UI.f_small);
    ui_shadow(x, y + M(0.008f), w, h, r, M(0.024f), rgba(0, 0, 0, 140));
    ui_rrect(x, y, w, h, r, frame);
    int top = y + pad / 2, ih = h - nh - sh - pad * 2 - pad / 2, iy = top + nh + pad / 3;
    char fit[96];
    ui_text_fit(UI.f_normal, name, w - 2 * pad, fit, sizeof fit);
    ui_text(UI.f_normal, fit, x + w / 2, top, sel ? t->highlight : light, ALIGN_CENTER);
    int gtab = gi >= 0 && !media_image(&games[gi], t->cover_types, t->ncover) ? vt_game_tab(gi) : -1;
    if (gi >= 0 && gtab < 0) cover_box(&games[gi], x + pad, iy, w - 2 * pad, ih, r / 2);
    else if (gi >= 0) {   /* gioco senza immagine: il disegno della sua piattaforma */
        color_t c = plat_color(gtab);
        ui_rrect(x + pad, iy, w - 2 * pad, ih, r / 2, c);
        vt_stripes(x + pad, iy, w - 2 * pad, ih, c, M(0.026f));
        draw_plat_art(gtab, x + pad * 2, iy + pad / 2, w - 4 * pad, ih - pad);
    } else {
        color_t c = vt_color(e);
        ui_rrect(x + pad, iy, w - 2 * pad, ih, r / 2, c);
        vt_stripes(x + pad, iy, w - 2 * pad, ih, c, M(0.026f));
        vt_art(e, x + pad * 2, iy + pad / 2, w - 4 * pad, ih - pad, 0);
        if (e->kind == VK_TAB && is_plan(ptabs[e->v])) ui_rrect(x + pad, iy, w - 2 * pad, ih, r / 2, with_alpha(c, 130));
    }
    int by = iy + ih + pad / 2;
    ui_text_fit(UI.f_small, maker, w * 55 / 100, fit, sizeof fit);
    ui_text(UI.f_small, fit, x + pad, by + (sh + M(0.01f) - sh) / 2, rgba(0xB4, 0xBA, 0xCC, 255), ALIGN_LEFT);
    if (year && year[0]) {   /* l'anno in un'etichetta chiara, come un bollino */
        int yw = ui_text_w(UI.f_small, year) + M(0.016f), yh = sh + M(0.008f);
        ui_rrect(x + w - pad - yw, by, yw, yh, M(0.006f), hexc(0x8C93A8));
        ui_text(UI.f_small, year, x + w - pad - yw / 2, by + M(0.004f), hexc(0x14172A), ALIGN_CENTER);
    }
    int bt = M(0.005f) + 1;
    if (sel) ui_rborder(x - bt, y - bt, w + 2 * bt, h + 2 * bt, r + bt, bt, t->accent);
    else ui_rborder(x, y, w, h, r, 2, with_alpha(light, 60));
}

static void gl_draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin);
    /* titolo e schede */
    int cy = M(t->topbar_h) / 2, fth = TTF_FontHeight(UI.f_title), fn = TTF_FontHeight(UI.f_normal);
    char tt[64];
    snprintf(tt, sizeof tt, "%s", t->title);
    if (t->title_upper) for (char *p = tt; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    int x = mx + ui_text(UI.f_title, tt, mx, cy - fth / 2, t->highlight, ALIGN_LEFT) + M(0.03f);
    ui_button_glyph("L", x + M(0.014f), cy, M(0.03f));
    x += M(0.04f);
    static const char *const tk[GT_N] = { "home.favorites", "home.recent", "plat.title" };
    for (int k = 0; k < GT_N; k++) {
        int w = ui_text(UI.f_normal, T(tk[k]), x, cy - fn / 2, k == gl_tab ? t->text : t->text_dim, ALIGN_LEFT);
        if (k == gl_tab) ui_rrect(x, cy + fn / 2 + M(0.004f), w, M(0.006f) + 1, M(0.003f), t->accent);
        x += w + M(0.03f);
    }
    ui_button_glyph("R", x + M(0.008f), cy, M(0.03f));
    ui_topbar(NULL);
    /* il contenuto della scheda */
    vt_entry_t e[MAX_EMUS + 48];
    int gl[64], n = gl_tab == GT_PLAT ? vt_entries(e, MAX_EMUS + 48) : gl_games(gl_tab, gl, 64);
    if (gl_tab == GT_PLAT && n && e[0].kind == VK_GAME) { memmove(e, e + 1, sizeof *e * (size_t)(n - 1)); n--; }   /* l'ultima partita è in Recenti */
    int *sel = &gl_sel[gl_tab];
    if (*sel >= n) *sel = n ? n - 1 : 0;
    int iy = M(t->topbar_h) + M(0.03f);
    char big[96] = "", sub[160] = "";
    if (n && gl_tab == GT_PLAT) {
        const vt_entry_t *s = &e[*sel];
        const emulator_t *em = s->kind == VK_TAB && ptabs[s->v] >= TAB_EMU && ptabs[s->v] < TAB_COLL ? &EMUS[ptabs[s->v] - TAB_EMU] : NULL;
        snprintf(big, sizeof big, "%s", em && em->shortname[0] ? em->shortname : vt_label(s));
        if (s->kind == VK_TAB) plat_count_text(ptabs[s->v], sub, sizeof sub);
    } else if (n) {
        snprintf(big, sizeof big, "%s", games[gl[*sel]].title);
        snprintf(sub, sizeof sub, "%s", games_core_label(games[gl[*sel]].core));
    }
    if (big[0]) {
        char fit[96];
        ui_text_fit(UI.f_big, big, UI.w - 2 * mx, fit, sizeof fit);
        ui_text(UI.f_big, fit, mx, iy, t->text, ALIGN_LEFT);
        int sy = iy + TTF_FontHeight(UI.f_big) + M(0.006f), d = TTF_FontHeight(UI.f_small) * 7 / 10;
        if (sub[0]) {
            ui_rrect(mx, sy + (TTF_FontHeight(UI.f_normal) - d) / 2, d, d, d / 4, t->accent);
            ui_text(UI.f_normal, sub, mx + d + M(0.014f), sy, t->text, ALIGN_LEFT);
        }
    }
    /* il carosello: la scheda scelta più grande, tenuta al secondo posto da sinistra */
    int ch = UI.h * 33 / 100, cw = ch * 82 / 100, bigh = ch * 116 / 100, bigw = cw * 116 / 100, gap = M(0.018f);
    int row_mid = UI.h * 58 / 100;
    if (!n) {
        const char *msg = gl_tab == GT_FAV ? T("library.empty_fav") : T("gallery.no_recent");
        ui_text(UI.f_normal, msg, UI.w / 2, row_mid - fn / 2, t->text_dim, ALIGN_CENTER);
    } else {
        int sel_x = *sel * (cw + gap);
        gl_off_target = (float)(sel_x - (cw + gap));
        if (gl_off_target < 0) gl_off_target = 0;
        gl_off = ui_approach(gl_off, gl_off_target);
        int xx = mx - (int)gl_off;
        for (int i = 0; i < n; i++) {
            bool sl = i == *sel;
            int w = sl ? bigw : cw, h = sl ? bigh : ch, y = row_mid - h / 2;
            if (xx < UI.w && xx + w > 0) {
                if (gl_tab == GT_PLAT) {
                    const vt_entry_t *s = &e[i];
                    const emulator_t *em = s->kind == VK_TAB && ptabs[s->v] >= TAB_EMU && ptabs[s->v] < TAB_COLL ? &EMUS[ptabs[s->v] - TAB_EMU] : NULL;
                    char cnt[48] = "";
                    if (!em && s->kind == VK_TAB) plat_count_text(ptabs[s->v], cnt, sizeof cnt);
                    gl_card(xx, y, w, h, sl, vt_label(s), em ? em->maker : cnt, em ? em->year : "", s, -1);
                } else {
                    const game_t *g = &games[gl[i]];   /* il produttore e l'anno del gioco, se l'indice li ha */
                    gl_card(xx, y, w, h, sl, g->title, g->maker[0] ? g->maker : games_core_label(g->core), g->year, NULL, gl[i]);
                }
            }
            xx += w + gap;
        }
    }
    /* tasti, sulla fascia in basso */
    bool game = n && gl_tab != GT_PLAT;
    int g = game ? gl[*sel] : -1;
    const char *act = T(game ? (g == resume_idx ? "home.resume" : "common.play") : n && e[*sel].kind == VK_TAB ? "plat.open" : "common.open");
    const char *h1[] = { "A", act, NULL, "X", T("common.info"), "", "Y", T(g >= 0 && games[g].fav ? "common.unfavorite" : "common.favorite"), "",
                         "START", T("common.settings"), NULL };
    const char *h2[] = { "A", act, NULL, "START", T("common.settings"), NULL };
    if (g >= 0) ui_keybar(h1, 12); else ui_keybar(h2, n ? 6 : 3);
}

static void gl_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_L] || in->pressed[BTN_R]) {
        gl_tab = (gl_tab + (in->pressed[BTN_R] ? 1 : GT_N - 1)) % GT_N;
        gl_off = gl_off_target = 0;
        ui_sound(SND_MOVE);
        return;
    }
    vt_entry_t e[MAX_EMUS + 48];
    int gl[64], n = gl_tab == GT_PLAT ? vt_entries(e, MAX_EMUS + 48) : gl_games(gl_tab, gl, 64);
    if (gl_tab == GT_PLAT && n && e[0].kind == VK_GAME) { memmove(e, e + 1, sizeof *e * (size_t)(n - 1)); n--; }
    int *sel = &gl_sel[gl_tab], old = *sel;
    if (in->pressed[BTN_LEFT] && *sel > 0) (*sel)--;
    if (in->pressed[BTN_RIGHT] && *sel < n - 1) (*sel)++;
    if (*sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); return; }
    if (!n) return;
    if (gl_tab != GT_PLAT) {
        int g = gl[*sel];
        if (in->pressed[BTN_A]) { home_play(g); return; }
        if (in->pressed[BTN_X]) { open_detail(g); return; }
        if (in->pressed[BTN_Y]) {
            games_toggle_fav(UI.root, &games[g]);
            ui_toast(games[g].fav ? T("home.fav_added") : T("home.fav_removed"));
            build_tabs();
            build_carousel();
            ui_sound(SND_OK);
        }
        return;
    }
    const vt_entry_t *s = &e[*sel];
    if (!in->pressed[BTN_A]) return;
    if (s->kind == VK_MENU) home_do(s->v);
    else if (is_plan(ptabs[s->v])) ui_sound(SND_ERROR);
    else if (s->v < ntabs) {
        tab_sel = s->v;
        lib_sel = 0;
        lib_scroll = lib_scroll_target = 0;
        build_view();
        go(SCR_LIBRARY);
        ui_sound(SND_OK);
    }
}

static void plat_input(void) {
    input_t *in = &UI.in;
    int old = plat_sel;
    if ((in->pressed[BTN_LEFT] || in->pressed[BTN_L]) && plat_sel > 0) plat_sel--;
    if ((in->pressed[BTN_RIGHT] || in->pressed[BTN_R]) && plat_sel < nptabs - 1) plat_sel++;
    if (plat_sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_A] && nptabs && is_plan(ptabs[plat_sel])) ui_sound(SND_ERROR);
    else if (in->pressed[BTN_A] && ntabs) {
        tab_sel = plat_sel;
        lib_sel = 0;
        lib_scroll = lib_scroll_target = 0;
        build_view();
        go(SCR_LIBRARY);
        ui_sound(SND_OK);
    } else if (in->pressed[BTN_B]) { go(SCR_HOME); ui_sound(SND_BACK); }
    else if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); }
}

static void library_draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    topbar_theme();
    int y = M(t->topbar_h) + M(0.016f);
    y += pills(y) + M(0.022f);
    char label[200];
    snprintf(label, sizeof label, T("lib.count"), nview);
    if (search[0]) { size_t l = strlen(label); snprintf(label + l, sizeof label - l, " · «%s»", search); }
    if (lib_style() == LS_CARDS) {   /* Tessera: il nome del gioco scelto, e a destra quanti sono */
        int cwid = ui_text_w(fs, label), lh = fnh;
        ui_text(fs, label, mx + W, y + (lh - fsh) / 2, t->text_dim, ALIGN_RIGHT);
        if (nview) {
            char fit[300];
            ui_text_fit(fn, games[view[lib_sel]].title, W - cwid - M(0.03f), fit, sizeof fit);
            ui_text(fn, fit, mx, y, t->accent, ALIGN_LEFT);
        }
        y += lh + M(0.018f);
    } else {
        small_caps(label, mx, y, t->text_dim);
        y += fsh + M(0.02f);
    }
    int bottom = UI.h - ui_keybar_h() - M(0.025f), area_h = bottom - y;
    int style = lib_style(), empty = lib_empty_kind();

    if (!nview) {
        if (empty == LE_REFRESH) empty_panel(mx, y, W, area_h, T("lib.empty_title"), T("lib.empty_text"), T("lib.empty_text2"), T("lib.refresh"));
        else if (empty == LE_OPEN_ALL) empty_panel(mx, y, W, area_h, T("lib.nofav_title"), T("lib.nofav_text"), T("lib.nofav_text2"), T("lib.open_all"));
        else empty_panel(mx, y, W, area_h, T("lib.none_title"), T("library.empty"), NULL, NULL);
    } else if (style == LS_CARDS) {
        card_grid(y, area_h);
    } else if (style == LS_LIST) {
        int lw = W * 47 / 100;
        title_list(mx, y, lw, area_h);
        game_panel(&games[view[lib_sel]], view[lib_sel], mx + lw + M(0.025f), y, W - lw - M(0.025f), area_h, false);
    } else {
        int lw = W * 52 / 100;   /* elenco verticale a sinistra, a destra il gioco scelto */
        onda_list(mx, y, lw, area_h);
        game_panel(&games[view[lib_sel]], view[lib_sel], mx + lw + M(0.03f), y, W - lw - M(0.03f), area_h, false);
    }

    if (nview) {
        bool resume = game_autosave(view[lib_sel], NULL);
        const char *h[] = { "A", T(resume ? "home.resume" : "common.play"), NULL, "B", T("common.back"), "",
                            "X", T("common.info"), NULL, "Y", T(games[view[lib_sel]].fav ? "common.unfavorite" : "common.favorite"), NULL,
                            "L/R", T("lib.category"), "", "SELECT", T("lib.search_filters"), T("lib.search_short"),
                            "START", T("common.settings"), NULL };
        ui_keybar(h, 21);
    } else {
        const char *h[] = { "A", T("lib.confirm"), NULL, "B", T("common.back"), "", "L/R", T("lib.category"), "",
                            "SELECT", T("lib.search_filters"), T("lib.search_short"), "START", T("common.settings"), NULL };
        ui_keybar(empty == LE_NONE ? h + 3 : h, empty == LE_NONE ? 12 : 15);
    }
}

static void library_input(void) {
    input_t *in = &UI.in;
    int empty = lib_empty_kind();
    if (in->pressed[BTN_A] && empty == LE_REFRESH) {
        reload_games();
        build_tabs();
        build_view();
        build_carousel();
        char m[64];
        snprintf(m, sizeof m, T("info.games"), ngames);
        ui_toast(m);
        ui_sound(SND_OK);
        return;
    }
    if (in->pressed[BTN_A] && empty == LE_OPEN_ALL) { tab_sel = 0; lib_sel = 0; build_view(); ui_sound(SND_OK); return; }
    lib_common_input(lib_style() == LS_CARDS ? GRID_COLS : 1);
}

/* ----------------------------------------------------- impostazioni */

enum { S_BRIGHT, S_VOLUME, S_MUSIC, S_LED, S_LEDCOLOR, S_LEDBRIGHT, S_THEME, S_VARIANT, S_LANG, S_GRIDCOLS, S_HOMESIZE, S_ASPECT, S_FILTER, S_BEZEL, S_DUP, S_MISSING, S_GENRES, S_COLLS, S_CPU, S_PADS, S_PADTEST, S_STICKCAL, S_KEYF1, S_KEYF2, S_KEYHOME, S_WIFI, S_WIFINETS, S_SAVER, S_SLEEP, S_SUSPEND, S_RESUME, S_AUTOSAVE, S_CLOCK, S_TZ, S_DEVICE, S_ROMCHECK, S_SCRAPE, S_CLEANMEDIA, S_UPDATE, S_CHANNEL, S_UPDNOTIFY, S_LOGLEVEL, S_INFO, S_SUSPENDNOW, S_RESTART, S_POWER, S_BOXCOVER, S_SPEAKER, S_FILESERVER, S_RESETSET, S_ERASE, S_STORE, S_SUPPORT, S_COUNT };

/* chiavi "settings.<voce>" e "settings_desc.<voce>" nei file di lingua */
static const char *set_key[S_COUNT] = {
    "brightness", "volume", "music", "led", "led_color", "led_brightness", "theme", "variant", "language", "grid_columns", "home_size", "aspect", "filter", "bezel", "duplicates", "missing", "genres",
    "collections", "cpu", "pads", "pad_test", "stick_cal", "key_f1", "key_f2", "key_home", "wifi", "wifi_nets", "screensaver", "sleep", "suspend", "resume", "autosave_exit", "clock", "timezone", "device", "romcheck", "scrape", "clean_media", "update", "channel", "update_notify", "log_level", "info", "suspend_now", "restart", "power", "box_covers", "speaker_rest", "file_server", "reset_settings", "erase_all", "store", "support",
};

/* Categorie delle impostazioni; l'ultima ("power": sospendi, riavvia, spegni) ha voci fisse */
static const char *cat_key[SC_COUNT + 1] = { "display", "look", "games", "controls", "system", "tools", "card", "updates", "power" };
static const int cat_items[SC_COUNT][12] = {
    { S_BRIGHT, S_VOLUME, S_SPEAKER, S_MUSIC, S_LED, S_LEDCOLOR, S_LEDBRIGHT, -1 },
    { S_THEME, S_VARIANT, S_LANG, S_GRIDCOLS, S_HOMESIZE, S_BOXCOVER, S_SAVER, -1 },
    { S_ASPECT, S_FILTER, S_BEZEL, S_DUP, S_MISSING, S_GENRES, S_COLLS, S_CPU, S_RESUME, S_AUTOSAVE, -1 },
    { S_PADS, S_PADTEST, S_STICKCAL, S_KEYF1, S_KEYF2, S_KEYHOME, -1 },
    { S_WIFI, S_WIFINETS, S_CLOCK, S_TZ, S_SUSPEND, S_SLEEP, S_DEVICE, -1 },
    { S_ROMCHECK, S_SCRAPE, S_CLEANMEDIA, -1 },          /* ROM e immagini */
    { S_STORE, S_FILESERVER, S_RESETSET, S_ERASE, -1 },  /* scheda e dati */
    { S_UPDATE, S_CHANNEL, S_UPDNOTIFY, S_LOGLEVEL, S_INFO, S_SUPPORT, -1 },
};
#define ROW_CAT 1000   /* righe della prima pagina: ROW_CAT + categoria */

/* HOME laterale, F1 e F2 ci sono solo sulla Brick Pro (sul Mac si mostrano per le prove) */
/* solo nel sistema su SD: partendo dal sistema TrimUI quella cartella è nella memoria interna */
static bool sc_available(void) {
    if (!plat_standalone()) return false;
#ifdef TRIMUI_DEVICE
    return true;
#else
    return getenv("TRIMUI_FAKE_STICKS") != NULL;
#endif
}
static void sc_open(void);
static void sc_restore_factory(void);

static bool has_extra_keys(void) {
    const char *m = plat_model();
    return !plat_is_device() || (m && !strcmp(m, "brickpro"));
}

/* righe della pagina corrente: voci S_* oppure ROW_CAT + categoria */
#define SC_LAST (SC_COUNT - 1)   /* ultima sezione; sospendi, riavvia e spegni sono nella schermata iniziale */
static int page_rows_of(int page, int *rows) {
    int n = 0;
    if (page < 0) {
        for (int c = 0; c < SC_COUNT; c++) rows[n++] = ROW_CAT + c;
        return n;
    }
    if (page >= SC_COUNT) {   /* sospendi solo nel sistema su SD; nell'app sul firmware TrimUI "riavvia" torna al suo menu */
        if (plat_standalone()) rows[n++] = S_SUSPENDNOW;
        if (plat_standalone() || plat_is_device()) rows[n++] = S_RESTART;
        rows[n++] = S_POWER;
        return n;
    }
    for (int k = 0; k < 12 && cat_items[page][k] >= 0; k++) {
        int it = cat_items[page][k];
        if (it == S_DEVICE && plat_is_device()) continue;   /* misura dello schermo: solo nel prototipo */
        if (it == S_WIFINETS && !wifi_supported()) continue;
        if (it == S_STICKCAL && !sc_available()) continue;
        if (it == S_GRIDCOLS) continue;   /* la libreria ora ha una misura fissa per tema */
        if ((it == S_KEYF1 || it == S_KEYF2 || it == S_KEYHOME) && !has_extra_keys()) continue;
        if ((it == S_UPDATE || it == S_CHANNEL || it == S_UPDNOTIFY) && !update_supported()) continue;
        if (it == S_FILESERVER && !wifi_supported()) continue;   /* serve il Wi-Fi del sistema su SD */   /* nell'app sul firmware TrimUI no */
        if ((it == S_RESETSET || it == S_ERASE) && plat_is_device() && !plat_standalone()) continue;   /* app sul firmware: la cartella è nella memoria interna */
        rows[n++] = it;
    }
    return n;
}
static int page_rows(int *rows) { return page_rows_of(set_page, rows); }

/* voci che in questo momento non possono fare nulla: in grigio, con la descrizione che resta leggibile */
static bool set_disabled(int i) {
    switch (i) {
    case S_SCRAPE: case S_UPDATE: case S_WIFINETS: case S_FILESERVER: return !UI.set.wifi;
    case S_MUSIC: return !music_count();
    case S_LEDCOLOR: case S_LEDBRIGHT: return !UI.set.led_mode;
    case S_VARIANT: return UI.th.nvariants < 2;
    case S_GRIDCOLS: case S_HOMESIZE: return is_list_layout() || is_bar_layout();
    default: return false;
    }
}

static void settings_open(int page, int row) {
    if (page < 0 && is_bar_layout()) { page = row >= 0 && row <= SC_LAST ? row : 0; row = 0; }   /* Onda: le sezioni sono pulsanti in alto */
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
    if (item == S_RESTART && !plat_standalone() && !strcmp(group, "settings")) return T("common.quit_menu");
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
    for (int i = 0; i < UI.th.nvariants; i++)   /* il nome che dà il tema stesso, se c'è */
        if (!strcmp(UI.th.variants[i], v) && UI.th.variant_labels[i][0]) return UI.th.variant_labels[i];
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
    case S_COLLS: snprintf(buf, n, "%d", NCOLLS); break;
    case S_PADTEST: case S_STICKCAL: buf[0] = 0; break;
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
    case S_AUTOSAVE: snprintf(buf, n, "%s", T(s->autosave_exit ? "common.yes" : "common.no")); break;
    case S_BOXCOVER: snprintf(buf, n, "%s", T(s->box_covers ? "common.yes" : "common.no")); break;
    case S_SPEAKER: { static const char *k[3] = { "speaker.always", "speaker.rest_a", "speaker.rest_b" }; snprintf(buf, n, "%s", T(k[s->speaker_rest % 3])); break; }
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
        /* si sceglie fra gli emulatori arcade: le console non hanno doppioni */
        do s->dup_mode = (s->dup_mode + dir + NEMUS + 1) % (NEMUS + 1);
        while (s->dup_mode > 0 && EMUS[s->dup_mode - 1].console);
        mark_duplicates();
        build_genres();
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
    case S_AUTOSAVE: s->autosave_exit = !s->autosave_exit; break;
    case S_BOXCOVER: s->box_covers = !s->box_covers; break;
    case S_SPEAKER: s->speaker_rest = (s->speaker_rest + dir + 3) % 3; plat_amp_config(s->speaker_rest); break;
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
        if (plat_standalone()) settings_save_tz_for_boot(s, UI.root);
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

/* ------------------------------------------------------------ Store */
/* Schermata dello Store (store.c): prima le quattro cartelle (Arcade, Console, App, Giochi Linux;
   le ultime due "Presto" finché non hanno pacchetti), poi i pacchetti della cartella, ognuno con
   l'immagine della sua console, poi la scheda del pacchetto con i pulsanti. Archivia toglie solo
   l'emulatore (giochi e salvataggi restano, come sulla Switch), Elimina anche giochi, salvataggi e
   immagini: tutte e due mostrano prima cosa si toglie e cosa resta, con le quantità; Elimina chiede
   poi di tenere premuto A per 3 secondi, come Cancella tutto. */
enum { SL_CATS, SL_PKGS };
enum { SA_NONE, SA_ARCHIVE, SA_DELETE, SA_DELETE2 };
static const char *const STO_CATS[] = { "arcade", "console", "apps", "themes", "linux" };
#define NSTO_CATS 5
static int sto_level, sto_cat, sto_sel, sto_cats[NSTO_CATS], nsto_cats;
static int sto_items[64], nsto_items;
static bool sto_detail, sto_job, sto_removing;
static int sto_dsel, sto_ask, sto_ask_sel;
static double sto_hold;          /* Elimina, ultima conferma: da quando A è premuto */
static store_usage_t sto_use;    /* anteprima: calcolata all'apertura della conferma */
static float sto_scroll;

static bool sto_online(void) { return wifi_supported() ? wifi_status(false)->state == WIFI_CONNECTED : true; }
static bool sto_was_offline;   /* aperto senza rete: appena il Wi-Fi si collega si scarica il catalogo */

/* categorie presenti nel catalogo, e pacchetti della categoria scelta */
static void sto_build(void) {
    nsto_cats = 0;
    for (int c = 0; store_count() && c < NSTO_CATS; c++) sto_cats[nsto_cats++] = c;
    if (sto_cat >= nsto_cats) sto_cat = nsto_cats ? nsto_cats - 1 : 0;
    nsto_items = 0;
    if (nsto_cats)
        for (int i = 0; i < store_count() && nsto_items < 64; i++)
            if (!strcmp(store_pkg(i)->category, STO_CATS[sto_cats[sto_cat]])) sto_items[nsto_items++] = i;
}

static bool sto_batch;     /* pacchetti di un backup da rimettere appena c'è il catalogo */
static int restpkg_n;      /* quanti, per la domanda */
static bool restpkg_asked;
static void sto_start(void) {
    sto_was_offline = !sto_online();
    store_lang(UI.set.lang);
    store_open(UI.root, UI.set.update_channel, !sto_was_offline);
    sto_level = SL_CATS;
    sto_cat = sto_sel = 0;
    sto_scroll = 0;
    sto_detail = sto_job = false;
    sto_ask = SA_NONE;
    go(SCR_STORE);
}

/* temi arrivati o tolti dallo Store: l'elenco delle impostazioni; se quello in uso non c'è più,
   si torna al predefinito */
static void sto_themes_refresh(void) {
    nthemes = theme_list(UI.root, theme_ids, theme_names, 16);
    char p[1200];
    snprintf(p, sizeof p, "%s/Themes/%s/theme.json", UI.root, UI.set.theme);
    if (access(p, F_OK) == 0) return;
    ui_log("tema %s tolto: torno al predefinito", UI.set.theme);
    snprintf(UI.set.theme, sizeof UI.set.theme, "%s", "tessera");
    UI.set.variant[0] = 0;
    settings_save(&UI.set, UI.root);
    ui_reload_theme();
}

static void sto_leave(void) {
    store_join();
    if (store_changed()) { reload_games(); sto_themes_refresh(); }
    go(back_screen == SCR_STORE ? SCR_HOME : back_screen);
}

static bool sto_is_app(const store_pkg_t *p) { return !strcmp(p->category, "apps"); }
static bool sto_is_theme(const store_pkg_t *p) { return !strcmp(p->category, "themes"); }
/* app e temi: niente giochi né salvataggi, quindi niente Archivia e una conferma sola */
static bool sto_no_data(const store_pkg_t *p) { return sto_is_app(p) || sto_is_theme(p); }

/* pulsanti della scheda secondo lo stato del pacchetto */
enum { SB_INSTALL, SB_UPDATE, SB_REDOWNLOAD, SB_ARCHIVE, SB_DELETE };
static int sto_buttons(const store_pkg_t *p, int *b) {
    int n = 0;
    if (sto_no_data(p)) {   /* niente da archiviare: app e temi non hanno giochi né salvataggi */
        if (p->state == PKG_ABSENT || p->state == PKG_ARCHIVED) b[n++] = SB_INSTALL;
        else { if (p->state == PKG_UPDATE || p->state == PKG_OLD) b[n++] = SB_UPDATE; b[n++] = SB_DELETE; }
        return n;
    }
    if (p->state == PKG_ABSENT) b[n++] = SB_INSTALL;
    else if (p->state == PKG_ARCHIVED) { b[n++] = SB_REDOWNLOAD; b[n++] = SB_DELETE; }
    else {
        if (p->state == PKG_UPDATE || p->state == PKG_OLD) b[n++] = SB_UPDATE;
        b[n++] = SB_ARCHIVE;
        b[n++] = SB_DELETE;
    }
    return n;
}

static void sto_input(void) {
    const store_status_t *s = store_status();
    input_t *in = &UI.in;
    last_input = UI.t;
    if (s->phase == ST_WORKING) {
        if (in->pressed[BTN_B]) { store_cancel(); ui_sound(SND_BACK); }
        return;
    }
    if (sto_batch && !sto_job && s->phase != ST_LOADING) {   /* catalogo pronto: i pacchetti del backup */
        sto_batch = false;
        if (s->phase == ST_READY && store_count()) {
            ui_log("store: rimetto i pacchetti del backup");
            store_install_restore();
            sto_job = true;
            sto_removing = false;
            return;
        }
    }
    if (sto_was_offline && sto_online() && s->phase != ST_LOADING && !sto_job && !sto_detail && !sto_ask) {
        sto_was_offline = false;   /* la rete è arrivata: il catalogo aggiornato */
        store_open(UI.root, UI.set.update_channel, true);
        return;
    }
    if (sto_job) {   /* lavoro finito: com'è andata */
        store_join();
        sto_job = false;
        static const char *errk[] = { "", "store.err_network", "store.err_signature", "store.err_catalog", "store.err_space",
                                      "store.err_verify", "store.err_write", "store.err_core", "store.err_cancel", "store.err_unsigned" };
        if (s->phase == ST_DONE) { ui_toast(T("store.done")); ui_sound(SND_OK); }
        else { ui_toast(T(errk[s->error >= 0 && s->error <= SE_UNSIGNED ? s->error : SE_WRITE])); ui_sound(SND_ERROR); }
        if (store_changed()) { reload_games(); sto_themes_refresh(); }
        return;
    }
    sto_build();
    int idx = sto_level == SL_PKGS && sto_sel < nsto_items ? sto_items[sto_sel] : -1;
    if (sto_ask) {   /* conferme di Archivia ed Elimina */
        if (sto_ask != SA_ARCHIVE) in->power = false;   /* qui il tasto di accensione non conferma */
        int a = sto_ask;
        if (a == SA_DELETE2) {   /* ultima: A tenuto premuto, per non passarla premendo A di fila */
            if (in->pressed[BTN_LEFT] && sto_ask_sel) { sto_ask_sel = 0; sto_hold = 0; ui_sound(SND_MOVE); }
            if (in->pressed[BTN_RIGHT] && !sto_ask_sel) { sto_ask_sel = 1; sto_hold = 0; ui_sound(SND_MOVE); }
            if (in->pressed[BTN_B] || (in->pressed[BTN_A] && !sto_ask_sel)) { sto_ask = SA_NONE; sto_hold = 0; ui_sound(SND_BACK); return; }
            if (sto_ask_sel && in->pressed[BTN_A]) sto_hold = UI.t;
            if (!sto_ask_sel || !in->down[BTN_A]) { sto_hold = 0; return; }
            if (sto_hold <= 0 || UI.t - sto_hold < CFM_HOLD_S) return;
            sto_hold = 0;
            ui_sound(SND_OK);
        } else {
            int r = ui_confirm_input(&sto_ask_sel);
            if (r < 0) return;
            if (r != 1 || idx < 0) { sto_ask = SA_NONE; return; }
            if (a == SA_DELETE && !sto_no_data(store_pkg(idx))) { sto_ask = SA_DELETE2; sto_ask_sel = 0; sto_hold = 0; return; }
            if (a == SA_DELETE) a = SA_DELETE2;   /* app: si toglie subito, si può riscaricare */
        }
        sto_ask = SA_NONE;
        if (idx < 0) return;
        ui_log("store: %s %s", a == SA_ARCHIVE ? "archivio" : "elimino", store_pkg(idx)->id);
        store_remove(idx, a == SA_DELETE2);
        sto_job = sto_removing = true;
        sto_detail = false;
        return;
    }
    if (sto_detail && idx >= 0) {
        const store_pkg_t *p = store_pkg(idx);
        int b[4], nb = sto_buttons(p, b), r = ui_popup_input(&sto_dsel, nb);
        if (r == -2) { sto_detail = false; ui_sound(SND_BACK); return; }
        if (r < 0) return;
        if (b[r] == SB_INSTALL || b[r] == SB_UPDATE || b[r] == SB_REDOWNLOAD) {
            if (!sto_online()) { ui_toast(T("store.err_network")); ui_sound(SND_ERROR); return; }
            ui_log("store: installo %s", p->id);
            store_install(idx);
            sto_job = true;
            sto_removing = false;
            sto_detail = false;
            ui_sound(SND_OK);
        } else {   /* prima l'anteprima: cosa si toglie e cosa resta */
            sto_ask = b[r] == SB_ARCHIVE ? SA_ARCHIVE : SA_DELETE;
            sto_ask_sel = 0;
            store_usage(idx, &sto_use);
            ui_sound(SND_OK);
        }
        return;
    }
    int n = sto_level == SL_CATS ? nsto_cats : nsto_items, cols = 4, old = sto_level == SL_CATS ? sto_cat : sto_sel;
    int *sel = sto_level == SL_CATS ? &sto_cat : &sto_sel;
    if (in->pressed[BTN_LEFT] && *sel > 0) (*sel)--;
    if (in->pressed[BTN_RIGHT] && *sel < n - 1) (*sel)++;
    if (in->pressed[BTN_UP] && *sel >= cols) *sel -= cols;
    if (in->pressed[BTN_DOWN] && *sel + cols < n) *sel += cols;
    /* niente proprio sotto ma una riga più in basso: all'ultima voce, come nella libreria */
    else if (in->pressed[BTN_DOWN] && n && *sel / cols < (n - 1) / cols) *sel = n - 1;
    if (*sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_B]) {
        ui_sound(SND_BACK);
        if (sto_level == SL_PKGS) { sto_level = SL_CATS; sto_scroll = 0; }
        else sto_leave();
        return;
    }
    if (in->pressed[BTN_A] && (n || sto_level == SL_PKGS)) {
        ui_sound(SND_OK);
        if (sto_level == SL_CATS) { sto_level = SL_PKGS; sto_sel = 0; sto_scroll = 0; }
        else if (nsto_items) { sto_detail = true; sto_dsel = 0; }
    }
}

static void sto_size(long long b, char *out, size_t n) {
    if (b >= 1 << 20) snprintf(out, n, "%.1f MB", (double)b / (1 << 20));
    else snprintf(out, n, "%lld KB", (b + 1023) / 1024);
}

/* l'immagine di una cartella: il cabinato, una console, il cassetto delle app, il portatile */
static void sto_cat_art(int c, color_t *col, emulator_t *em) {
    memset(em, 0, sizeof *em);
    static const char *const arts[] = { "", "", "@apps", "@themes", "@linux" };   /* Console: una console generica */
    static const unsigned cols[] = { 0x8E2A45, 0x2B4C9B, 0x4B3C86, 0x9A4E2A, 0x2F6B45 };
    snprintf(em->art, sizeof em->art, "%s", arts[c]);
    em->console = c == 1;
    *col = hexc(cols[c]);
}

static void sto_pkg_art(const store_pkg_t *p, color_t *col, emulator_t *em) {
    memset(em, 0, sizeof *em);
    if (sto_is_app(p)) {   /* app: il suo simbolo, disegnato qui (l'icona arriva solo installandola) */
        snprintf(em->art, sizeof em->art, "@app:%s", p->id);
        *col = !strcmp(p->id, "files") ? hexc(0x1F6E8C) : !strcmp(p->id, "viewer") ? hexc(0x8C4A1F)
             : !strcmp(p->id, "music") ? hexc(0x7A2E6E) : !strcmp(p->id, "sdtest") ? hexc(0x2E6B3E) : hexc(0x4B3C86);
        return;
    }
    if (sto_is_theme(p)) {   /* tema: una schermata nei colori delle sue varianti */
        snprintf(em->art, sizeof em->art, "@theme");
        unsigned c1 = 0;
        if (sscanf(p->colors, "%6x", &c1) != 1) c1 = 0x9A4E2A;
        snprintf(em->shortname, sizeof em->shortname, "%.15s", p->colors);   /* le prime due */
        *col = mix(hexc(c1), rgba(0, 0, 0, 255), 0.35f);
        return;
    }
    snprintf(em->id, sizeof em->id, "%s", p->emu);
    snprintf(em->art, sizeof em->art, "%s", p->art);
    snprintf(em->shortname, sizeof em->shortname, "%s", p->shortname);
    em->console = p->console;
    *col = emu_color(p->emu);
}

/* immagine di un riquadro dello Store: cartelle (@apps, @linux), app (@app:<id>) o console */
static void sto_art(const emulator_t *em, color_t c, const char *label, int ax, int ay, int aw, int ah) {
    int s = aw < ah ? aw : ah;
    color_t white = rgba(0xF2, 0xF2, 0xF2, 255);
    if (!strcmp(em->art, "@apps")) draw_icon_glyph(IC_APPS, ax + aw / 2, ay + ah / 2, s * 2 / 3, white, c);
    else if (!strcmp(em->art, "@linux")) {
        int lw = aw, lh = aw * 80 / 100;
        if (lh > ah * 80 / 100) { lh = ah * 80 / 100; lw = lh * 100 / 80; }
        draw_laptop(ax + (aw - lw) / 2, ay + (ah - lh) / 2, lw, lh);
    } else if (!strcmp(em->art, "@app:files")) {   /* cartella con i fogli, come l'icona dell'app */
        int fw = s * 80 / 100, fh = fw * 70 / 100, fx = ax + (aw - fw) / 2, fy = ay + (ah - fh) / 2, r = fw / 14;
        color_t light = mix(c, white, 0.55f);
        ui_rrect(fx, fy, fw * 45 / 100, fh / 4, r, light);
        ui_rrect(fx, fy + fh / 8, fw, fh * 7 / 8, r, light);
        ui_rrect(fx + fw / 16, fy + fh / 16, fw * 7 / 8, fh / 2, r / 2, white);
        ui_rrect(fx, fy + fh * 3 / 8, fw, fh * 5 / 8, r, mix(c, white, 0.2f));
    } else if (!strcmp(em->art, "@app:viewer")) {   /* cornice con monti e sole */
        int fw = s * 80 / 100, fh = fw * 75 / 100, fx = ax + (aw - fw) / 2, fy = ay + (ah - fh) / 2, b = fw / 14;
        ui_rrect(fx, fy, fw, fh, fw / 12, white);
        ui_rrect(fx + b, fy + b, fw - 2 * b, fh - 2 * b, fw / 20, mix(c, white, 0.35f));
        ui_triangle(fx + b, fy + fh - b, fx + fw * 2 / 5, fy + fh * 2 / 5, fx + fw * 3 / 4, fy + fh - b, mix(c, rgba(0, 0, 0, 255), 0.25f));
        ui_triangle(fx + fw / 2, fy + fh - b, fx + fw * 2 / 3, fy + fh * 3 / 5, fx + fw - b, fy + fh - b, mix(c, rgba(0, 0, 0, 255), 0.4f));
        ui_circle(fx + fw * 3 / 4, fy + fh / 3, fw / 12, rgba(255, 196, 60, 255));
    } else if (!strcmp(em->art, "@app:music")) {   /* due note unite */
        int u = s / 10, cx = ax + aw / 2, cy = ay + ah / 2;
        ui_circle(cx - 2 * u, cy + 2 * u, u * 3 / 2, white);
        ui_circle(cx + 2 * u, cy + u * 3 / 2, u * 3 / 2, white);
        ui_rect(cx - u / 2, cy - 3 * u, u * 2 / 3, 5 * u, white);
        ui_rect(cx + 3 * u, cy - 3 * u - u / 2, u * 2 / 3, 5 * u, white);
        ui_triangle(cx - u / 2, cy - 3 * u, cx + 3 * u + u * 2 / 3, cy - 4 * u, cx + 3 * u + u * 2 / 3, cy - 2 * u - u / 2, white);
        ui_triangle(cx - u / 2, cy - 3 * u, cx + 3 * u + u * 2 / 3, cy - 2 * u - u / 2, cx - u / 2, cy - u - u / 2, white);
    } else if (!strcmp(em->art, "@app:sdtest")) {   /* scheda SD con i contatti */
        int fw = s * 56 / 100, fh = fw * 13 / 10, fx = ax + (aw - fw) / 2, fy = ay + (ah - fh) / 2, cut = fw / 4;
        ui_rrect(fx, fy + cut, fw, fh - cut, fw / 12, white);
        ui_rrect(fx + cut, fy, fw - cut, fh / 2, fw / 12, white);
        ui_triangle(fx, fy + cut, fx + cut, fy, fx + cut, fy + cut, white);
        for (int k = 0; k < 4; k++) ui_rrect(fx + cut / 2 + k * fw * 2 / 11 + fw / 12, fy + fw / 10, fw / 10, fh / 6, fw / 40, mix(c, white, 0.3f));
    } else if (!strcmp(em->art, "@themes") || !strcmp(em->art, "@theme")) {   /* schermata: barra in alto e riquadri */
        unsigned v[2] = { 0, 0 };
        int nv = sscanf(em->shortname, "%6x %6x", &v[0], &v[1]);
        color_t a1 = nv >= 1 ? hexc(v[0]) : rgba(255, 196, 60, 255), a2 = nv >= 2 ? hexc(v[1]) : mix(c, white, 0.5f);
        int fw = s * 84 / 100, fh = fw * 62 / 100, fx = ax + (aw - fw) / 2, fy = ay + (ah - fh) / 2, b = fw / 18;
        ui_rrect(fx, fy, fw, fh, fw / 12, white);
        ui_rrect(fx + b, fy + b, fw - 2 * b, fh / 5, fw / 30, a1);
        int tw = (fw - 4 * b) / 3, ty = fy + b * 2 + fh / 5, th = fh - (ty - fy) - b;
        for (int k = 0; k < 3; k++) ui_rrect(fx + b + k * (tw + b), ty, tw, th, fw / 30, k == 1 ? a1 : k == 0 ? a2 : mix(c, white, 0.3f));
    } else if (!strncmp(em->art, "@app:", 5)) draw_icon_glyph(IC_APPS, ax + aw / 2, ay + ah / 2, s * 2 / 3, white, c);
    else draw_emu_art(em, c, label, ax, ay, aw, ah);
}

/* riquadro a righe come quelli delle Piattaforme, con l'immagine e il nome sotto */
static void sto_tile(int x, int y, int w, int h, color_t c, const emulator_t *em, const char *label, const char *name,
                     const char *sub, const char *badge, bool sel, bool gray) {
    theme_t *t = &UI.th;
    UI.gray = gray;   /* non installato: tutto in grigio, tranne etichetta e cornice della scelta */
    ui_shadow(x, y + M(0.006f), w, h, M(0.006f), M(0.016f), t->shadow);
    ui_rect(x, y, w, h, c);
    ui_clip_push(x, y, w, h);
    int sp = M(0.032f), lt = M(0.005f) + 1;
    for (int s = -h; s < w; s += sp) plat_line(x + s, y, x + s + h, y + h, lt, mix(c, rgba(0, 0, 0, 255), 0.32f));
    ui_clip_pop();
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fh = TTF_FontHeight(fn) + TTF_FontHeight(fs) + M(0.02f), pad = w * 9 / 100;
    int ax = x + pad, ay = y + pad, aw = w - 2 * pad, ah = h - fh - pad - pad / 2;
    sto_art(em, c, label, ax, ay, aw, ah);
    ui_rect(x, y + h - fh, w, fh, hexc(0x151822));
    char fit[120];
    TTF_Font *fname = ui_text_w(fn, name) > w - M(0.02f) ? fs : fn;   /* un nome lungo col carattere piccolo */
    ui_text_fit(fname, name, w - M(0.02f), fit, sizeof fit);
    int ty = y + h - fh + M(0.008f);
    ui_text(fname, fit, x + w / 2, ty + (TTF_FontHeight(fn) - TTF_FontHeight(fname)) / 2, rgba(0xF2, 0xF2, 0xF2, 255), ALIGN_CENTER);
    ui_text_fit(fs, sub, w - M(0.02f), fit, sizeof fit);
    ui_text(fs, fit, x + w / 2, ty + TTF_FontHeight(fn), rgba(0xC8, 0xCC, 0xD4, 255), ALIGN_CENTER);
    UI.gray = false;
    if (badge && badge[0]) {   /* stato in alto a destra */
        int bw = ui_text_w(fs, badge) + M(0.02f), bh = TTF_FontHeight(fs) + M(0.006f);
        ui_rrect(x + w - bw - M(0.008f), y + M(0.008f), bw, bh, bh / 2, t->accent);
        ui_text(fs, badge, x + w - bw / 2 - M(0.008f), y + M(0.011f), t->accent_text, ALIGN_CENTER);
    }
    int bt = M(0.006f) + 1, o = M(0.004f);
    if (sel) ui_rborder(x - o, y - o, w + 2 * o, h + 2 * o, bt + 2, bt, t->accent);
    else ui_rborder(x, y, w, h, 3, 2, with_alpha(rgba(255, 255, 255, 255), 70));
}

/* installato: a colori e senza etichetta; non installato: in grigio; da aggiornare: a colori con
   l'etichetta; archiviato: in grigio con l'etichetta (giochi e salvataggi sono ancora lì) */
static const char *sto_badge(const store_pkg_t *p) {
    return p->state == PKG_UPDATE || p->state == PKG_OLD ? T("store.update") : p->state == PKG_ARCHIVED ? T("store.archived") : "";
}

static bool sto_gray(const store_pkg_t *p) { return p->state == PKG_ABSENT || p->state == PKG_ARCHIVED; }

/* anteprima di Archivia ed Elimina: a sinistra cosa si toglie, a destra cosa resta, con quantità e
   misure; sotto una riga di avviso e i pulsanti (Elimina in rosso) */
static void sto_preview_draw(const store_pkg_t *pp, bool del) {
    theme_t *t = &UI.th;
    store_pkg_t p = *pp;
    TTF_Font *ft = UI.f_title, *fn = UI.f_normal, *fs = UI.f_small;
    int fth = TTF_FontHeight(ft), fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), r = M(t->panel_radius) * 2;
    /* righe: etichetta e valore, nella colonna di chi si toglie o di chi resta */
    struct { const char *k; char v[48]; bool gone; } row[5];
    int nr = 0;
    char a[32];
    bool app = sto_no_data(&p);
    if (app) {   /* un'app o un tema: si toglie solo lui, i file della scheda restano */
        row[nr].k = T(sto_is_theme(&p) ? "store.pv_theme" : "store.pv_app");
        sto_size(p.installed, row[nr].v, sizeof row[nr].v);
        row[nr++].gone = true;
        row[nr].k = T("store.pv_yourfiles");
        row[nr].v[0] = 0;
        row[nr++].gone = false;
    } else if (p.state != PKG_ARCHIVED) {
        row[nr].k = T("store.pv_emu");
        sto_size(p.installed, row[nr].v, sizeof row[nr].v);
        row[nr++].gone = true;
    }
    if (!app) {
    row[nr].k = T("store.pv_games");
    sto_size(sto_use.games_bytes, a, sizeof a);
    if (sto_use.games) snprintf(row[nr].v, sizeof row[nr].v, "%d · %s", sto_use.games, a);
    else snprintf(row[nr].v, sizeof row[nr].v, "0");
    row[nr++].gone = del;
    row[nr].k = T("store.pv_saves");
    snprintf(row[nr].v, sizeof row[nr].v, "%d", sto_use.saves);
    row[nr++].gone = del;
    }
    if (!app && p.console) {   /* le immagini degli arcade sono per nome del gioco, comuni agli altri emulatori: restano */
        row[nr].k = T("store.pv_media");
        snprintf(row[nr].v, sizeof row[nr].v, "%d", sto_use.media);
        row[nr++].gone = del;
    }
    if (!app) {
        row[nr].k = T("store.pv_bios");
        row[nr].v[0] = 0;
        row[nr++].gone = false;
    }
    int ngone = 0;
    for (int k = 0; k < nr; k++) ngone += row[k].gone;
    int most = ngone > nr - ngone ? ngone : nr - ngone, step = fnh + M(0.008f);
    /* altezza dal contenuto: titolo, console, intestazioni, righe, avviso, pulsanti */
    int head = M(0.04f) + fth + fsh + M(0.025f), cols_h = fsh + M(0.026f) + most * step;
    int pw = M(0.92f) < UI.w - 2 * M(t->margin) ? M(0.92f) : UI.w - 2 * M(t->margin);
    int ph = head + cols_h + M(0.015f) + fsh + ui_popup_buttons_h(), px = (UI.w - pw) / 2, py = (UI.h - ph) / 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(px, py, pw, ph, r, t->surface);
    char ttl[160], fit[200];
    snprintf(ttl, sizeof ttl, T(del ? "store.delete_title" : "store.archive_title"), p.name);
    ui_text_fit(ft, ttl, pw - M(0.08f), fit, sizeof fit);
    int y = py + M(0.04f);
    ui_text(ft, fit, px + pw / 2, y, t->text, ALIGN_CENTER);
    y += fth;
    ui_text_fit(fs, p.systems, pw - M(0.08f), fit, sizeof fit);
    ui_text(fs, fit, px + pw / 2, y, t->text_dim, ALIGN_CENTER);
    y += fsh + M(0.025f);
    int pad = M(0.045f), gap = M(0.03f), cw = (pw - 2 * pad - gap) / 2;
    color_t red = rgba(220, 70, 70, 255), green = rgba(70, 180, 110, 255);
    for (int c = 0; c < 2; c++) {   /* 0: si toglie, 1: resta */
        int cx = px + pad + c * (cw + gap), cy = y;
        color_t hc = c ? green : red;
        small_caps(T(c ? "store.pv_keep" : "store.pv_remove"), cx, cy, hc);
        cy += fsh + M(0.012f);
        ui_rect(cx, cy, cw, M(0.003f) + 1, with_alpha(hc, 120));
        cy += M(0.014f);
        for (int k = 0; k < nr; k++) {
            if (row[k].gone == (c == 1)) continue;
            ui_rrect(cx, cy + (fnh - M(0.016f)) / 2, M(0.016f), M(0.016f), M(0.008f), hc);
            ui_text(fn, row[k].k, cx + M(0.03f), cy, t->text, ALIGN_LEFT);
            ui_text(fn, row[k].v, cx + cw, cy, t->text_dim, ALIGN_RIGHT);
            cy += step;
        }
    }
    int ny = y + cols_h + M(0.012f);
    ui_text_fit(fs, T(app ? "store.app_note" : del ? "store.delete_note" : "store.archive_note"), pw - M(0.08f), fit, sizeof fit);
    ui_text(fs, fit, px + pw / 2, ny, del && !app ? red : t->text_dim, ALIGN_CENTER);
    const char *labels[2] = { T("common.cancel"), T(del ? "store.delete" : "store.archive") };
    ui_popup_buttons(px, py, pw, ph, labels, 2, sto_ask_sel, del ? 1 : -1);
}

static void sto_draw(void) {
    theme_t *t = &UI.th;
    const store_status_t *s = store_status();
    topbar_label(T("store.title"));
    sto_build();
    int mx = M(t->margin), W = UI.w - 2 * mx, y = M(t->topbar_h) + M(0.025f), bottom = UI.h - ui_keybar_h() - M(0.03f);
    TTF_Font *fs = UI.f_small;
    char title[160];
    if (sto_level == SL_PKGS && nsto_cats) { snprintf(title, sizeof title, "store.cat_%s", STO_CATS[sto_cats[sto_cat]]); home_title(T(title), y, t->text); }
    else home_title(T("store.title"), y, t->text);
    const char *status = s->phase == ST_LOADING ? T("store.loading") : s->online ? T("store.online") : store_count() ? T("store.offline") : T("store.none");
    ui_text(fs, status, mx + W, y + (TTF_FontHeight(UI.f_title) - TTF_FontHeight(fs)) / 2, s->online ? t->accent : t->text_dim, ALIGN_RIGHT);
    y += TTF_FontHeight(UI.f_title) + M(0.03f);
    int n = sto_level == SL_CATS ? nsto_cats : nsto_items;
    if (!n && sto_level == SL_PKGS) {
        empty_panel(mx, y, W, bottom - y, T("store.cat_soon"), T("store.soon_body"), NULL, NULL);
    } else if (!n) {
        empty_panel(mx, y, W, bottom - y, T(s->phase == ST_LOADING ? "store.loading" : "store.none"), s->phase == ST_LOADING ? "" : T("store.empty"), NULL, NULL);
    } else {
        int cols = 4, gap = M(0.022f), tw = (W - (cols - 1) * gap) / cols, th = tw * 112 / 100, area = bottom - y;
        int sel = sto_level == SL_CATS ? sto_cat : sto_sel, row = sel / cols;
        float target = (float)(row * (th + gap));
        if (target + th > sto_scroll + area) sto_scroll = ui_approach(sto_scroll, target + th - area);
        else if (target < sto_scroll) sto_scroll = ui_approach(sto_scroll, target);
        ui_clip_push(mx - M(0.02f), y - M(0.01f), W + M(0.04f), area + M(0.02f));
        for (int i = 0; i < n; i++) {
            int x = mx + (i % cols) * (tw + gap), yy = y + (i / cols) * (th + gap) - (int)sto_scroll;
            if (yy > bottom || yy + th < y - M(0.02f)) continue;
            emulator_t em;
            color_t c;
            char sub[96];
            if (sto_level == SL_CATS) {
                int ci = sto_cats[i], cnt = 0, upd = 0;
                for (int k = 0; k < store_count(); k++) {
                    const store_pkg_t *p = store_pkg(k);
                    if (strcmp(p->category, STO_CATS[ci])) continue;
                    cnt++;
                    upd += p->state == PKG_UPDATE || p->state == PKG_OLD;
                }
                sto_cat_art(ci, &c, &em);
                snprintf(title, sizeof title, "store.cat_%s", STO_CATS[ci]);
                bool apps_cat = !strcmp(STO_CATS[ci], "apps") || !strcmp(STO_CATS[ci], "linux"), theme_cat = !strcmp(STO_CATS[ci], "themes");
                const char *ck = theme_cat ? (cnt == 1 ? "store.count1_themes" : "store.count_themes")
                               : apps_cat ? (cnt == 1 ? "store.count1_apps" : "store.count_apps") : (cnt == 1 ? "store.count1" : "store.count");
                if (cnt) snprintf(sub, sizeof sub, T(ck), cnt);
                else snprintf(sub, sizeof sub, "%s", T("store.cat_soon"));
                char badge[48] = "";
                if (upd) snprintf(badge, sizeof badge, "%d", upd);
                sto_tile(x, yy, tw, th, c, &em, T(title), T(title), sub, badge, i == sto_cat, false);
            } else {
                store_pkg_t p = *store_pkg(sto_items[i]);
                sto_pkg_art(&p, &c, &em);
                /* in grande la console (dice di più a chi gioca), sotto l'emulatore e la misura */
                char size[32];
                sto_size(p.size, size, sizeof size);
                if (sto_no_data(&p)) snprintf(sub, sizeof sub, "%s", size);
                else snprintf(sub, sizeof sub, "%s · %s", p.name, size);
                sto_tile(x, yy, tw, th, c, &em, p.title, p.title, sub, sto_badge(&p), i == sto_sel, sto_gray(&p));
            }
        }
        ui_clip_pop();
    }
    const char *h[] = { "B", T("common.back"), NULL, "A", T(sto_level == SL_CATS ? "common.open" : "store.details"), NULL };
    ui_keybar(h, n ? 6 : 3);

    int idx = sto_level == SL_PKGS && sto_sel < nsto_items ? sto_items[sto_sel] : -1;
    if (sto_detail && idx >= 0) {   /* scheda del pacchetto */
        store_pkg_t p = *store_pkg(idx);
        TTF_Font *ft = UI.f_title, *fn = UI.f_normal;
        int fth = TTF_FontHeight(ft), fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), r = M(t->panel_radius);
        int pw = M(0.92f) < UI.w - 2 * mx ? M(0.92f) : UI.w - 2 * mx, ph = M(0.56f), px = (UI.w - pw) / 2, py = (UI.h - ph) / 2;
        ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
        ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
        ui_rrect(px, py, pw, ph, r, t->surface);
        int pad = M(0.04f), aw = pw * 30 / 100, ah = ph - ui_popup_buttons_h() - pad;
        emulator_t em;
        color_t c;
        sto_pkg_art(&p, &c, &em);
        UI.gray = sto_gray(&p);
        ui_rrect(px + pad, py + pad, aw, ah - pad, r, c);
        sto_art(&em, c, p.name, px + pad + aw / 10, py + pad + aw / 10, aw - aw / 5, ah - pad - aw / 5);
        UI.gray = false;
        int tx = px + pad + aw + M(0.03f), tw = px + pw - pad - tx, ty = py + pad;
        char fit[200], line[240], a[32], b2[32];
        ui_text_fit(ft, p.name, tw, fit, sizeof fit);
        ui_text(ft, fit, tx, ty, t->text, ALIGN_LEFT);
        ty += fth + M(0.008f);
        char lines[3][160];
        int nl = ui_wrap(fn, p.systems, tw, lines, 2);
        for (int k = 0; k < nl; k++, ty += fnh) ui_text(fn, lines[k], tx, ty, t->accent, ALIGN_LEFT);
        ty += M(0.015f);
        sto_size(p.size, a, sizeof a);
        sto_size(p.installed, b2, sizeof b2);
        snprintf(line, sizeof line, T("store.info"), p.label, a, b2);
        nl = ui_wrap(fs, line, tw, lines, 2);
        for (int k = 0; k < nl; k++, ty += fsh) ui_text(fs, lines[k], tx, ty, t->text_dim, ALIGN_LEFT);
        ty += M(0.012f);
        static const char *const stk[] = { "store.st_absent", "store.st_installed", "store.st_update", "store.st_old", "store.st_archived" };
        nl = ui_wrap(fs, T(stk[p.state >= 0 && p.state <= PKG_ARCHIVED ? p.state : 0]), tw, lines, 3);
        for (int k = 0; k < nl; k++, ty += fsh) ui_text(fs, lines[k], tx, ty, t->text, ALIGN_LEFT);
        int bl[4], nb = sto_buttons(&p, bl), danger = -1;
        static const char *const bk[] = { "store.install", "store.do_update", "store.redownload", "store.archive", "store.delete" };
        const char *labels[4];
        for (int k = 0; k < nb; k++) { labels[k] = T(bk[bl[k]]); if (bl[k] == SB_DELETE) danger = k; }
        if (sto_dsel >= nb) sto_dsel = nb - 1;
        ui_popup_buttons(px, py, pw, ph, labels, nb, sto_dsel, danger);
    }
    if (sto_ask == SA_DELETE2 && idx >= 0) {   /* ultima conferma: A tenuto premuto 3 secondi */
        double held = sto_hold > 0 ? UI.t - sto_hold : 0;
        ui_confirm_draw(T("store.delete2_title"), T("store.delete2_body"), T("common.cancel"), T("store.delete"), sto_ask_sel);
        int w = M(0.75f), h = M(0.36f), x = (UI.w - w) / 2, y2 = (UI.h - h) / 2, bh = M(0.012f);
        int bx = x + M(0.06f), bw = w - M(0.12f), by = y2 + h - ui_popup_buttons_h() - bh - M(0.012f);
        ui_rrect(bx, by, bw, bh, bh / 2, with_alpha(t->text_dim, 50));
        if (held > 0) ui_rrect(bx, by, (int)(bw * (held < CFM_HOLD_S ? held / CFM_HOLD_S : 1)), bh, bh / 2, rgba(220, 70, 70, 255));
    } else if (sto_ask && idx >= 0) sto_preview_draw(store_pkg(idx), sto_ask == SA_DELETE);
    if (s->phase == ST_WORKING) {   /* lavoro in corso */
        int pw = M(0.8f), ph = M(0.26f), px = (UI.w - pw) / 2, py = (UI.h - ph) / 2, r = M(t->panel_radius);
        ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
        ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
        ui_rrect(px, py, pw, ph, r, t->surface);
        char line[160];
        snprintf(line, sizeof line, T(sto_removing ? "store.removing" : "store.working"), s->working);
        if (s->batch_total > 1) {   /* pacchetti di un backup: quale su quanti */
            size_t l = strlen(line);
            snprintf(line + l, sizeof line - l, " (%d/%d)", s->batch_done + 1, s->batch_total);
        }
        ui_text_fit(UI.f_title, line, pw - M(0.08f), line, sizeof line);
        ui_text(UI.f_title, line, px + pw / 2, py + M(0.05f), t->text, ALIGN_CENTER);
        int bw = pw - M(0.12f), bh = M(0.012f), bx = px + M(0.06f), by = py + M(0.05f) + TTF_FontHeight(UI.f_title) + M(0.035f);
        long long tot = s->total, done = s->done;
        int pct = tot > 0 ? (int)(done * 100 / tot) : 0;
        ui_rrect(bx, by, bw, bh, bh / 2, with_alpha(t->text_dim, 60));
        ui_rrect(bx, by, (int)((long long)bw * pct / 100), bh, bh / 2, t->accent);
        char a[32], b2[32];
        sto_size(done, a, sizeof a);
        sto_size(tot, b2, sizeof b2);
        snprintf(line, sizeof line, "%s / %s", a, b2);
        ui_text(fs, tot > 0 ? line : "", px + pw / 2, by + bh + M(0.02f), t->text_dim, ALIGN_CENTER);
    }
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
static sdrow_t sdrows[MAX_EMUS + 8];
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

/* ------------------------------------------------- trasferimento file */
/* Impostazioni > Strumenti > Trasferisci file: l'indirizzo e il PIN da usare nel browser, cosa
   sta succedendo, le ultime operazioni. Il server (fileserver.c) resta acceso solo finché la
   finestra è aperta; alla chiusura, se qualcosa è cambiato, la libreria si rilegge. */
static bool fsrv_open;
static int fsrv_sel;
static char fsrv_ip[48];
static double fsrv_ip_t;

static void fsrv_start(void) {
    if (!UI.set.wifi) { ui_toast(T("scrape.no_wifi")); ui_sound(SND_ERROR); return; }
    if (!fs_start(UI.root)) { ui_toast(T("fileserver.no_port")); ui_sound(SND_ERROR); return; }
    ui_log("trasferimento file: aperto sulla porta %d", fs_port());
    fsrv_open = true;
    fsrv_sel = 0;
    fsrv_ip_t = -10;
    ui_sound(SND_OK);
}

static void fsrv_close(void) {
    fs_status_t s;
    fs_get_status(&s);
    fs_stop();
    fsrv_open = false;
    ui_log("trasferimento file: chiuso, %d file ricevuti", s.uploads);
    if (fs_changed()) {   /* ROM, immagini o giochi nuovi: si vedono subito */
        ui_image_forget_missing();
        reload_games();
    }
}

/* pulsanti della finestra: Chiudi e, se ce ne sono, Dimentica i browser */
/* scelta delle parti di un backup da ripristinare: le parti presenti nello zip, tutte spuntate;
   su e giù fra le righe e i pulsanti, A spunta o toglie, B annulla */
static const char *const PART_KEY[FS_NPARTS] = { "fileserver.part_saves", "fileserver.part_settings", "fileserver.part_bios",
                                                 "fileserver.part_wifi", "fileserver.part_media", "fileserver.part_games" };
typedef struct { int mask, sel, btn; } partpick_t;   /* sel == righe: si è sui pulsanti */

static int pp_rows(const fs_backup_t *bk, int *rows) {
    int n = 0;
    for (int p = 0; p < FS_NPARTS; p++) if (bk->count[p] > 0) rows[n++] = p;
    return n;
}

static void pp_reset(partpick_t *pp, const fs_backup_t *bk) {
    int rows[FS_NPARTS], n = pp_rows(bk, rows);
    pp->mask = 0;
    for (int i = 0; i < n; i++) pp->mask |= 1 << rows[i];
    pp->sel = n;   /* si parte dal pulsante che ripristina tutto */
    pp->btn = 1;
}

/* -1 ancora aperta, 0 annullata, altrimenti le parti scelte */
static int pp_input(partpick_t *pp, const fs_backup_t *bk) {
    input_t *in = &UI.in;
    int rows[FS_NPARTS], n = pp_rows(bk, rows);
    if (in->pressed[BTN_UP] && pp->sel > 0) { pp->sel--; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN] && pp->sel < n) { pp->sel++; ui_sound(SND_MOVE); }
    if (pp->sel == n && in->pressed[BTN_LEFT] && pp->btn) { pp->btn = 0; ui_sound(SND_MOVE); }
    if (pp->sel == n && in->pressed[BTN_RIGHT] && !pp->btn) { pp->btn = 1; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { ui_sound(SND_BACK); return 0; }
    if (!in->pressed[BTN_A]) return -1;
    if (pp->sel < n) { pp->mask ^= 1 << rows[pp->sel]; ui_sound(SND_MOVE); return -1; }
    if (!pp->btn) { ui_sound(SND_BACK); return 0; }
    if (!pp->mask) { ui_sound(SND_ERROR); return -1; }
    ui_sound(SND_OK);
    return pp->mask;
}

static void pp_draw(const partpick_t *pp, const fs_backup_t *bk, const char *title, const char *go) {
    theme_t *t = &UI.th;
    int rows[FS_NPARTS], n = pp_rows(bk, rows);
    TTF_Font *ft = UI.f_title, *fn = UI.f_normal, *fs = UI.f_small;
    int fth = TTF_FontHeight(ft), fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), r = M(t->panel_radius);
    int pw = M(0.8f), rh = fnh + M(0.026f);
    int ph = M(0.045f) + fth + M(0.008f) + fsh + M(0.025f) + n * rh + ui_popup_buttons_h();
    int px = (UI.w - pw) / 2, py = (UI.h - ph) / 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(px, py, pw, ph, r, t->surface);
    int y = py + M(0.045f);
    ui_text(ft, title, px + pw / 2, y, t->text, ALIGN_CENTER);
    y += fth + M(0.008f);
    char line[160];
    snprintf(line, sizeof line, T("fileserver.backup_of"), bk->date);
    ui_text(fs, line, px + pw / 2, y, t->text_dim, ALIGN_CENTER);
    y += fsh + M(0.025f);
    int lx = px + M(0.03f), lw = pw - M(0.06f), sh = fnh * 3 / 4;
    for (int i = 0; i < n; i++, y += rh) {
        bool on = pp->mask & (1 << rows[i]), sel = pp->sel == i;
        if (sel) ui_rrect(lx, y, lw, rh - M(0.006f), M(t->panel_radius), t->accent);
        int ty = y + (rh - M(0.006f) - fnh) / 2;
        ui_text(fn, T(PART_KEY[rows[i]]), lx + M(0.025f), ty, sel ? t->accent_text : t->text, ALIGN_LEFT);
        int sw = sh * 9 / 5, sx = lx + lw - M(0.025f) - sw;
        draw_switch(sx, y + (rh - M(0.006f) - sh) / 2, sh, on);
        snprintf(line, sizeof line, T("fileserver.part_files"), bk->count[rows[i]]);
        ui_text(fs, line, sx - M(0.02f), y + (rh - M(0.006f) - fsh) / 2, sel ? t->accent_text : t->text_dim, ALIGN_RIGHT);
    }
    const char *lb[] = { T("common.cancel"), go };
    ui_popup_buttons(px, py, pw, ph, lb, 2, pp->sel == n ? pp->btn : -1, -1);
}

static int fsrv_nbuttons(void) { return fs_browsers() > 0 ? 2 : 1; }

static partpick_t fsrv_pp;   /* scelta delle parti nel ripristino */
static bool fsrv_pp_on;

static void fsrv_input(void) {
    fs_status_t s;
    fs_get_status(&s);
    if (s.restore == FS_RS_PENDING) {   /* un backup arrivato dalla pagina: si ripristina? */
        if (!fsrv_pp_on) { pp_reset(&fsrv_pp, &s.restore_bk); fsrv_pp_on = true; }
        int r = pp_input(&fsrv_pp, &s.restore_bk);
        if (r >= 0) { fs_restore_answer(r); fsrv_pp_on = false; ui_log("trasferimento file: ripristino %s (parti %x)", r ? "confermato" : "annullato", (unsigned)r); }
        return;
    }
    if (s.restore == FS_RS_RUNNING) return;
    if (s.restore == FS_RS_DONE) {   /* impostazioni nuove: si riparte da capo */
        if (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B]) { ui_log("trasferimento file: backup ripristinato, riavvio"); fs_stop(); settings_load(&UI.set, UI.root); restart_system(); }
        return;
    }
    if (s.restore == FS_RS_FAILED) { ui_toast(T("fileserver.restore_failed")); fs_restore_clear(); ui_sound(SND_ERROR); }
    int n = fsrv_nbuttons(), r = ui_popup_input(&fsrv_sel, n);
    if (r == 1) {
        fs_forget_browsers();
        ui_log("trasferimento file: browser dimenticati");
        ui_toast(T("fileserver.forgotten"));
        fsrv_sel = 0;
        ui_sound(SND_OK);
    } else if (r != -1) { fsrv_close(); ui_sound(SND_BACK); }
}

/* il codice QR dell'indirizzo con il PIN: dal telefono si inquadra e si è già dentro */
static void fsrv_qr(int x, int y, int size, const char *url) {
    static unsigned char m[QR_MAX_SIDE * QR_MAX_SIDE];
    static char last[200];
    static int side;
    if (strcmp(last, url)) { snprintf(last, sizeof last, "%s", url); side = qr_encode(url, m); }
    if (!side) return;
    int quiet = 2, cell = size / (side + 2 * quiet), real = cell * (side + 2 * quiet), ox = x + (size - real) / 2, oy = y + (size - real) / 2;
    if (cell < 1) return;
    ui_rrect(ox, oy, real, real, cell, rgba(255, 255, 255, 255));
    for (int r = 0; r < side; r++)
        for (int c = 0; c < side; c++)
            if (m[r * side + c]) ui_rect(ox + (quiet + c) * cell, oy + (quiet + r) * cell, cell, cell, rgba(0, 0, 0, 255));
}

/* backup ripristinato: si riparte con le sue impostazioni; senza giochi nel backup, si ricorda
   di ricopiarli (salvataggi e liste restano invisibili finché le ROM non tornano) */
static void restore_done_draw(bool had_games) {
    theme_t *t = &UI.th;
    TTF_Font *ft = UI.f_title, *fs = UI.f_small;
    int fth = TTF_FontHeight(ft), fsh = TTF_FontHeight(fs), r = M(t->panel_radius);
    int pw = M(0.8f), ph = M(0.34f) + (had_games ? 0 : fsh + M(0.01f)), px = (UI.w - pw) / 2, py = (UI.h - ph) / 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(px, py, pw, ph, r, t->surface);
    int y = py + M(0.05f);
    ui_text(ft, T("fileserver.restore_done"), px + pw / 2, y, t->text, ALIGN_CENTER);
    y += fth + M(0.01f);
    ui_text(fs, T("fileserver.restore_done_body"), px + pw / 2, y, t->text_dim, ALIGN_CENTER);
    if (!had_games) ui_text(fs, T("fileserver.restore_no_games"), px + pw / 2, y + fsh + M(0.01f), t->text, ALIGN_CENTER);
    const char *lb[] = { T("common.restart") };
    ui_popup_buttons(px, py, pw, ph, lb, 1, 0, -1);
}

static void fsrv_draw(void) {
    theme_t *t = &UI.th;
    if (UI.t - fsrv_ip_t > 2) { fs_local_ip(fsrv_ip, sizeof fsrv_ip); fsrv_ip_t = UI.t; }
    fs_status_t s;
    fs_get_status(&s);
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    TTF_Font *ft = UI.f_title, *fn = UI.f_normal, *fs = UI.f_small;
    int fth = TTF_FontHeight(ft), fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int w = UI.w - 2 * M(0.04f), pad = M(0.04f);
    int h = pad + fth + M(0.025f) + fsh + fth + fsh + M(0.025f) + fsh + fth + M(0.03f) + fnh + M(0.03f) + 3 * fsh + ui_popup_buttons_h();
    if (h > UI.h - M(0.04f)) h = UI.h - M(0.04f);
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2, lx = x + pad;
    int foot = y + h - ui_popup_buttons_h();
    int qs = foot - (y + pad + fth + M(0.025f)) - M(0.01f);   /* lato del QR, a destra */
    if (qs > w * 40 / 100) qs = w * 40 / 100;
    int bw = w - 3 * pad - (fsrv_ip[0] ? qs : 0);
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    int cy = y + pad;
    ui_text(ft, T("fileserver.title"), lx, cy, t->text, ALIGN_LEFT);
    cy += fth + M(0.025f);
    char line[200], fit[200], url[200];
    const char *p = fs_pin();
    /* indirizzo fisso (mosaik.local), sotto quello numerico, poi il PIN */
    ui_text(fs, T("fileserver.open_on"), lx, cy, t->text_dim, ALIGN_LEFT);
    cy += fsh;
    if (!fsrv_ip[0]) {
        ui_text(fn, T("fileserver.no_network"), lx, cy + (fth - fnh) / 2, rgba(220, 70, 70, 255), ALIGN_LEFT);
        cy += fth + fsh;
    } else {
        char port[16] = "";
        if (fs_port() != 80) snprintf(port, sizeof port, ":%d", fs_port());
        snprintf(line, sizeof line, "http://%s%s", FS_HOSTNAME, port);
        TTF_Font *fu = ui_text_w(ft, line) <= bw ? ft : fn;
        ui_text(fu, line, lx, cy + (fth - TTF_FontHeight(fu)) / 2, t->accent, ALIGN_LEFT);
        cy += fth;
        snprintf(url, sizeof url, "http://%s%s", fsrv_ip, port);
        snprintf(line, sizeof line, T("fileserver.or_ip"), url);
        ui_text_fit(fs, line, bw, fit, sizeof fit);
        ui_text(fs, fit, lx, cy, t->text_dim, ALIGN_LEFT);
        cy += fsh;
        snprintf(line, sizeof line, "%s/#pin=%s", url, p);
        int qx = x + w - pad - qs, qy = y + pad + fth + M(0.025f);
        fsrv_qr(qx, qy, qs, line);
    }
    cy += M(0.025f);
    ui_text(fs, T("fileserver.pin"), lx, cy, t->text_dim, ALIGN_LEFT);
    cy += fsh;
    snprintf(line, sizeof line, "%.3s %.3s", p, strlen(p) > 3 ? p + 3 : "");
    ui_text(ft, line, lx, cy, t->text, ALIGN_LEFT);
    cy += fth + M(0.03f);
    /* cosa sta succedendo */
    if (s.restore == FS_RS_RUNNING) {
        int pct = s.total > 0 ? (int)(s.done * 100 / s.total) : 0;
        snprintf(line, sizeof line, T("fileserver.restoring"), pct);
        ui_text(fn, line, lx, cy, t->accent, ALIGN_LEFT);
    } else if (s.state == FS_IDLE) ui_text(fn, T("fileserver.waiting"), lx, cy, t->text_dim, ALIGN_LEFT);
    else {
        snprintf(line, sizeof line, T(s.state == FS_RECEIVING ? "fileserver.receiving" : "fileserver.sending"), s.current);
        int pw = s.total > 0 ? ui_text_w(fn, "100%") + M(0.02f) : 0;
        ui_text_fit(fn, line, bw - pw, fit, sizeof fit);
        ui_text(fn, fit, lx, cy, t->text, ALIGN_LEFT);
        if (s.total > 0) {
            int pct = (int)(s.done * 100 / s.total);
            snprintf(line, sizeof line, "%d%%", pct);
            ui_text(fn, line, lx + bw, cy, t->accent, ALIGN_RIGHT);
            int bh = M(0.012f), by = cy + fnh + M(0.006f);
            ui_rrect(lx, by, bw, bh, bh / 2, with_alpha(t->text_dim, 60));
            ui_rrect(lx, by, (int)((long long)bw * pct / 100), bh, bh / 2, t->accent);
        }
    }
    cy += fnh + M(0.03f);
    /* ultime operazioni */
    static const char *const ev_key[] = { "fileserver.ev_upload", "fileserver.ev_download", "fileserver.ev_zip", "fileserver.ev_delete",
                                          "fileserver.ev_rename", "fileserver.ev_mkdir", "fileserver.ev_login", "fileserver.ev_fail",
                                          "fileserver.ev_backup", "fileserver.ev_restore" };
    for (int i = 0; i < s.nevents && cy + fsh <= foot; i++, cy += fsh) {
        snprintf(line, sizeof line, T(ev_key[s.ev_kind[i] >= 0 && s.ev_kind[i] <= FS_EV_RESTORE ? s.ev_kind[i] : FS_EV_FAIL]), s.ev_name[i]);
        ui_text_fit(fs, line, bw, fit, sizeof fit);
        ui_text(fs, fit, lx, cy, i ? t->text_dim : t->text, ALIGN_LEFT);
    }
    const char *labels[] = { T("fileserver.close"), T("fileserver.forget") };
    int n = fsrv_nbuttons();
    if (fsrv_sel >= n) fsrv_sel = 0;
    ui_popup_buttons(x, y, w, h, labels, n, fsrv_sel, 1);
    /* ripristino: la domanda, e alla fine il riavvio */
    if (s.restore == FS_RS_PENDING) {
        if (!fsrv_pp_on) pp_reset(&fsrv_pp, &s.restore_bk);
        pp_draw(&fsrv_pp, &s.restore_bk, T("fileserver.restore_title"), T("fileserver.restore_go"));
    } else if (s.restore == FS_RS_DONE) restore_done_draw(s.restore_bk.count[FS_PART_GAMES] > 0);
}

/* ------------------------------------------------- backup copiato sulla scheda */
/* Un backup scaricato dalla pagina del trasferimento file (mosaik-backup-*.zip) e copiato nella
   cartella principale della scheda o in Arcade/: alla prima schermata (dopo la scelta della
   lingua, su una scheda nuova) si chiede se importarlo. Una volta sola per file: nome e
   dimensione finiscono in System/backup-visti.txt, a qualunque risposta. Lo zip resta dov'è. */
enum { IMP_NONE, IMP_ASK, IMP_RUN, IMP_DONE, IMP_FAILED };
#define IMP_MAX 8
static atomic_int imp_state;
static bool imp_checked;
static char imp_zip[1200];
static fs_backup_t imp_bk;
static partpick_t imp_pp;
static atomic_int imp_parts;
static int imp_ncand;
static char imp_cand[IMP_MAX][300];   /* "nome\tdimensione" dei backup trovati */

static void imp_seen_path(char *p, size_t n) { snprintf(p, n, "%s/System/backup-visti.txt", UI.root); }

static bool imp_seen(const char *key) {
    char p[1100], line[400];
    imp_seen_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        found = !strcmp(line, key);
    }
    fclose(f);
    return found;
}

static void imp_mark_seen(const char *key) {
    char p[1100];
    imp_seen_path(p, sizeof p);
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "%s\n", key);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void imp_mark_all(void) {
    for (int i = 0; i < imp_ncand; i++) imp_mark_seen(imp_cand[i]);
    imp_ncand = 0;
}

/* cerca i backup non ancora visti; tiene quello con il nome più recente (contiene la data) */
static bool imp_find(void) {
    char dirs[2][1100], best[300] = "";
    snprintf(dirs[0], sizeof dirs[0], "%s", UI.root);
    snprintf(dirs[1], sizeof dirs[1], "%s", UI.root);
    char *sl = strrchr(dirs[1], '/');
    if (sl && sl != dirs[1]) *sl = 0;
    else snprintf(dirs[1], sizeof dirs[1], "%s", sl ? "/" : ".");
    imp_ncand = 0;
    for (int d = 0; d < 2; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            if (strncasecmp(n, "mosaik-backup", 13) || l < 17 || strcasecmp(n + l - 4, ".zip") || l > 250) continue;
            char full[1400], key[300];
            struct stat sb;
            snprintf(full, sizeof full, "%s/%s", dirs[d], n);
            if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            snprintf(key, sizeof key, "%s\t%lld", n, (long long)sb.st_size);
            if (imp_seen(key)) continue;
            fs_backup_t bk;
            if (!fs_backup_info(full, &bk)) { ui_log("backup sulla scheda: %s non è un backup", n); imp_mark_seen(key); continue; }
            if (imp_ncand < IMP_MAX) snprintf(imp_cand[imp_ncand++], sizeof imp_cand[0], "%s", key);
            if (!best[0] || strcmp(n, best) > 0) {
                snprintf(best, sizeof best, "%s", n);
                snprintf(imp_zip, sizeof imp_zip, "%s", full);
                imp_bk = bk;
            }
        }
        closedir(dp);
    }
    if (best[0]) ui_log("backup sulla scheda: %s (%s, %d file)", imp_zip, imp_bk.date, imp_bk.files);
    return best[0] != 0;
}

static void *imp_thread(void *arg) {
    (void)arg;
    bool ok = fs_backup_import(UI.root, imp_zip, atomic_load(&imp_parts));
    atomic_store(&imp_state, ok ? IMP_DONE : IMP_FAILED);
    return NULL;
}

static void imp_input(void) {
    int st = atomic_load(&imp_state);
    if (st == IMP_ASK) {
        int r = pp_input(&imp_pp, &imp_bk);
        if (r < 0) return;
        imp_mark_all();
        if (!r) { ui_log("backup sulla scheda: non importato"); atomic_store(&imp_state, IMP_NONE); return; }
        ui_log("backup sulla scheda: importazione (parti %x)", (unsigned)r);
        atomic_store(&imp_parts, r);
        quiet_begin();
        atomic_store(&imp_state, IMP_RUN);
        pthread_t t;
        if (pthread_create(&t, NULL, imp_thread, NULL) != 0) atomic_store(&imp_state, IMP_FAILED);
        else pthread_detach(t);
    } else if (st == IMP_DONE) {
        if (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B]) {
            ui_log("backup sulla scheda: importato, riavvio");
            settings_load(&UI.set, UI.root);   /* il riavvio salva le impostazioni: quelle del backup */
            restart_system();
        }
    } else if (st == IMP_FAILED) {
        ui_log("backup sulla scheda: importazione non riuscita");
        ui_toast(T("fileserver.restore_failed"));
        ui_sound(SND_ERROR);
        reload_games();   /* una parte dei file può essere arrivata */
        atomic_store(&imp_state, IMP_NONE);
    }
}

static void imp_draw(void) {
    int st = atomic_load(&imp_state);
    if (st == IMP_ASK) {
        pp_draw(&imp_pp, &imp_bk, T("fileserver.import_title"), T("fileserver.import_go"));
        return;
    }
    if (st != IMP_RUN && st != IMP_DONE) return;
    theme_t *t = &UI.th;
    TTF_Font *ft = UI.f_title, *fs = UI.f_small;
    int pw = M(0.8f), ph = M(0.34f), px = (UI.w - pw) / 2, py = (UI.h - ph) / 2, r = M(t->panel_radius), fth = TTF_FontHeight(ft);
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(px, py, pw, ph, r, t->surface);
    if (st == IMP_DONE) { restore_done_draw(imp_bk.count[FS_PART_GAMES] > 0); return; }
    fs_status_t s;
    fs_get_status(&s);
    int pct = s.total > 0 ? (int)(s.done * 100 / s.total) : 0;
    char line[160];
    snprintf(line, sizeof line, T("fileserver.restoring"), pct);
    ui_text(ft, line, px + pw / 2, py + M(0.06f), t->text, ALIGN_CENTER);
    int bw = pw - M(0.12f), bh = M(0.012f), bx = px + M(0.06f), by = py + M(0.06f) + fth + M(0.04f);
    ui_rrect(bx, by, bw, bh, bh / 2, with_alpha(t->text_dim, 60));
    ui_rrect(bx, by, (int)((long long)bw * pct / 100), bh, bh / 2, t->accent);
    char fit[160];
    ui_text_fit(fs, s.current, bw, fit, sizeof fit);
    ui_text(fs, fit, px + pw / 2, by + bh + M(0.025f), t->text_dim, ALIGN_CENTER);
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
    const char *cores[MAX_EMUS + 1];
    int ncores = 0;
    for (int i = 0; i < NEMUS && ncores < MAX_EMUS; i++) cores[ncores++] = EMUS[i].id;
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

/* Sostieni Mosaik: la pagina delle donazioni (PayPal), con il codice QR da inquadrare col telefono.
   Si apre solo da qui: nessun avviso compare da solo. */
#define SUPPORT_URL "https://www.paypal.me/diapolonRS"
static bool support_open;

static void support_input(void) {
    int sel = 0;
    if (ui_popup_input(&sel, 1) != -1 || UI.in.pressed[BTN_B]) { support_open = false; ui_sound(SND_BACK); }
}

static void fsrv_qr(int x, int y, int size, const char *url);

static void support_draw(void) {
    theme_t *t = &UI.th;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    TTF_Font *ft = UI.f_title, *fn = UI.f_normal, *fs = UI.f_small;
    int fth = TTF_FontHeight(ft), fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int w = UI.w - 2 * M(0.04f), pad = M(0.04f), h = M(0.62f);
    if (h > UI.h - M(0.04f)) h = UI.h - M(0.04f);
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2, lx = x + pad;
    int foot = y + h - ui_popup_buttons_h();
    int qs = foot - (y + pad) - M(0.01f);
    if (qs > w * 40 / 100) qs = w * 40 / 100;
    int bw = w - 3 * pad - qs, cy = y + pad;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, with_alpha(t->bg, 255));   /* fondo pieno: in Onda la superficie è trasparente */
    ui_rrect(x, y, w, h, r, t->surface);
    ui_text(ft, T("support.title"), lx, cy, t->text, ALIGN_LEFT);
    cy += fth + M(0.02f);
    char lines[5][160];
    int n = ui_wrap(fn, T("support.body"), bw, lines, 5);
    for (int k = 0; k < n; k++, cy += fnh) ui_text(fn, lines[k], lx, cy, t->text, ALIGN_LEFT);
    cy += M(0.025f);
    ui_text(fs, T("support.scan"), lx, cy, t->text_dim, ALIGN_LEFT);
    cy += fsh;
    char fit[120];
    ui_text_fit(fn, "paypal.me/diapolonRS", bw, fit, sizeof fit);
    ui_text(fn, fit, lx, cy, t->accent, ALIGN_LEFT);
    fsrv_qr(x + w - pad - qs, y + pad, qs, SUPPORT_URL);
    const char *labels[1] = { T("support.close") };
    ui_popup_buttons(x, y, w, h, labels, 1, 0, -1);
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
    /* area di avvio della scheda più vecchia di quella delle immagini di questa versione (R07) */
    int bgen_latest, bgen = update_boot_gen(UI.root, &bgen_latest);
    bool bold = bgen < bgen_latest;
    char blines[2][160];
    int nbl = bold ? ui_wrap(fs, T("sdinfo.boot_old"), w - 2 * pad, blines, 2) : 0;
    int h = pad + TTF_FontHeight(UI.f_title) + M(0.02f) + M(0.03f) + M(0.015f) + sh + M(0.03f) + nsdrows * rh + M(0.02f) + sh + pad + ui_popup_buttons_h() - pad
          + nbl * sh;
    if (h > UI.h - M(0.04f)) h = UI.h - M(0.04f);
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
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
    int foot = y + h - ui_popup_buttons_h() - sh, bottom = foot - M(0.02f) - nbl * sh;   /* sopra il pulsante */
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
    size_t ll = strlen(line);
    snprintf(line + ll, sizeof line - ll, " · ");
    ll = strlen(line);
    snprintf(line + ll, sizeof line - ll, T("sdinfo.boot"), bgen);
    for (int i = 0; i < nbl; i++) ui_text(fs, blines[i], lx, foot - (nbl - i) * sh, rgba(230, 130, 20, 255), ALIGN_LEFT);
    ui_text(fs, line, lx, foot, t->text_dim, ALIGN_LEFT);
    const char *label[1] = { T("common.ok") };
    ui_popup_buttons(x, y, w, h, label, 1, 0, -1);
}

/* Pulisci immagini: le immagini in Media/<tipo>/ di giochi che non sono sulla scheda (ROM
   tolte) si contano, si mostrano con lo spazio occupato e, con conferma, si eliminano. */
static bool clean_open;
static int clean_n, clean_sel;
static long long clean_bytes;

static int cmp_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* conta (e con del toglie) le immagini senza gioco in un elenco di file; prefix: "" in
   Media/<tipo>/, "<console>/" nelle sottocartelle per console (issue #5) */
static int orphans_in(const char *dirp, const char *prefix, char **names, int nn, bool del, long long *bytes) {
    int count = 0;
    DIR *d2 = opendir(dirp);
    struct dirent *f;
    while (d2 && (f = readdir(d2))) {
        if (f->d_name[0] == '.') continue;
        char path[1700];
        snprintf(path, sizeof path, "%s/%s", dirp, f->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* Media/<tipo>/<console>/: solo un livello, e solo se è una console */
            if (!*prefix && core_console(f->d_name)) {
                char pre[64];
                snprintf(pre, sizeof pre, "%s/", f->d_name);
                count += orphans_in(path, pre, names, nn, del, bytes);
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        char stem[300];
        snprintf(stem, sizeof stem, "%s%s", prefix, f->d_name);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        const char *key = stem;
        if (bsearch(&key, names, (size_t)nn, sizeof *names, cmp_name)) continue;
        count++;
        *bytes += st.st_size;
        if (del) unlink(path);
    }
    if (d2) closedir(d2);
    return count;
}

static int media_orphans(bool del, long long *bytes) {
    /* nomi dei giochi presenti; per le console anche "<console>/<nome>" */
    char **names = malloc(sizeof *names * (size_t)(2 * ngames + 1));
    int nn = 0, count = 0;
    for (int i = 0; i < ngames; i++) {
        if (games[i].missing) continue;
        names[nn++] = strdup(games[i].name);
        if (core_console(games[i].core)) {
            char k[ROM_NAME + 20];
            snprintf(k, sizeof k, "%s/%s", games[i].core, games[i].name);
            names[nn++] = strdup(k);
        }
    }
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
        count += orphans_in(sub, "", names, nn, del, bytes);
    }
    if (d) closedir(d);
    for (int i = 0; i < nn; i++) free(names[i]);
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
    if (in->pressed[BTN_LEFT] && clean_sel) { clean_sel = 0; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_RIGHT] && !clean_sel) { clean_sel = 1; ui_sound(SND_MOVE); }
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
    const char *labels[2] = { T("common.cancel"), T("clean.delete") };
    ui_popup_buttons(x, y, w, h, labels, 2, clean_sel, 1);
}

/* Aggiorna sistema: cerca, chiede conferma, scarica, verifica, installa e riavvia */
static bool upd_open;
static bool cp_open;         /* gestione delle collezioni */
static int cp_sel;
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
                || romcheck_open || scrape_open || clean_open || sdinfo_open || wpanel_open || fsrv_open
                || (screen == SCR_STORE && store_status()->phase == ST_WORKING)
                || atomic_load(&imp_state) == IMP_RUN || atomic_load(&imp_state) == IMP_DONE || atomic_load(&rst_state);
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
    /* niente ripresa automatica dopo l'aggiornamento: il sistema nuovo deve arrivare al menu,
       che lo conferma (update_confirm_boot); con la ripresa il launcher uscirebbe subito e dopo
       tre avvii così l'initramfs tornerebbe al sistema vecchio (audit prelancio R02) */
    char rp[1100];
    snprintf(rp, sizeof rp, "%s/System/resume.txt", UI.root);
    unlink(rp);
    bool charger = on_charger();
    if (charger) ui_log("aggiornamento installato con il caricatore collegato: la console si spegne");
    ui_busy(charger ? T("common.restart_charger") : T("update.restarting"), charger ? 4 : 0.5);
    music_quit();
    ui_exit_now(charger ? EXIT_POWEROFF : EXIT_REBOOT);
}

/* Si installa solo con il caricatore o con abbastanza batteria: se la corrente manca mentre i
   file si sostituiscono, sulla SD restano file vecchi e nuovi mescolati (TODO.md). Scaricare
   invece si può sempre: i file arrivano con nome provvisorio e quelli in uso non si toccano.
   Livello sconosciuto: si lascia fare, un sensore che non risponde non deve bloccare. */
enum { UPD_MIN_BATTERY = 30 };
static bool upd_battery_ok(void) {
    bool charging;
    int pct = ui_battery(&charging);
    if (charging || pct < 0 || pct >= UPD_MIN_BATTERY) return true;
    char m[160];
    snprintf(m, sizeof m, T("update.battery_low"), pct);
    ui_toast(m);
    ui_log("aggiornamento: batteria al %d%% senza caricatore, fermo", pct);
    return false;
}

/* Aggiornamento scaricato e non installato (spegnimento, batteria scarica): all'avvio la console
   lo ricontrolla (update_resume_start) e chiede se installarlo. Più tardi lo tiene per l'avvio
   dopo, Elimina cancella i file scaricati. */
static bool rdy_open, rdy_asked;
/* SD che non risponde più dopo un risveglio (arcade-suspend scrive /run/sd-guasta): l'unica
   cosa da fare è riavviare. Nessuna scrittura sulla scheda: non tornerebbe più. */
static bool sdbad_open;
static void sdbad_input(void) {
    if (UI.in.pressed[BTN_A]) {
        ui_log("SD che non risponde: riavvio");
        _exit(21);   /* system-init vede /run/sd-guasta e riavvia subito dal kernel */
    }
}
static void sdbad_draw(void) {
    theme_t *t = &UI.th;
    char lines[3][160];
    int w = M(0.85f), pad = M(0.05f), sh = TTF_FontHeight(UI.f_small);
    int nl = ui_wrap(UI.f_small, T("sdbad.body"), w - 2 * pad, lines, 3);
    int h = pad + TTF_FontHeight(UI.f_title) + M(0.02f) + nl * sh + ui_popup_buttons_h();
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 160));
    ui_rrect(x, y, w, h, r, t->surface);
    ui_text(UI.f_title, T("sdbad.title"), x + pad, y + pad, rgba(230, 130, 20, 255), ALIGN_LEFT);
    int ty = y + pad + TTF_FontHeight(UI.f_title) + M(0.02f);
    for (int i = 0; i < nl; i++) ui_text(UI.f_small, lines[i], x + pad, ty + i * sh, t->text, ALIGN_LEFT);
    const char *label[1] = { T("common.restart") };
    ui_popup_buttons(x, y, w, h, label, 1, 0, -1);
}
static int rdy_sel = 2;

static bool upd_battery_ok(void);
static void rdy_input(void) {
    int r = ui_popup_input(&rdy_sel, 3);
    if (r == 2) {
        if (!upd_battery_ok()) { ui_sound(SND_ERROR); return; }
        rdy_open = false;
        update_apply();
        upd_open = true;   /* la finestra dell'aggiornamento mostra l'installazione e il riavvio */
        ui_sound(SND_OK);
    } else if (r == 0) {
        rdy_open = false;
        update_discard();
        ui_toast(T("update.deleted"));
        ui_sound(SND_BACK);
    } else if (r == 1 || r == -2) {
        rdy_open = false;
        ui_sound(SND_BACK);
    }
}

static void rdy_draw(void) {
    theme_t *t = &UI.th;
    const update_status_t *s = update_status();
    char body[400], lines[3][160];
    snprintf(body, sizeof body, T("update.ready_hint"), s->version);
    if (s->reinstall) { size_t l = strlen(body); snprintf(body + l, sizeof body - l, " %s", T("update.reinstall")); }
    int w = M(0.85f), pad = M(0.05f);
    int nl = ui_wrap(UI.f_small, body, w - 2 * pad, lines, 3), sh = TTF_FontHeight(UI.f_small);
    int h = pad + TTF_FontHeight(UI.f_title) + M(0.02f) + nl * sh + ui_popup_buttons_h();
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    if (t->surface.a < 200) ui_rrect(x, y, w, h, r, with_alpha(t->bg2, 230));
    int ty = y + pad;
    ui_text(UI.f_title, T("update.resume_title"), x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.02f);
    for (int i = 0; i < nl; i++) ui_text(UI.f_small, lines[i], x + pad, ty + i * sh, t->text_dim, ALIGN_LEFT);
    const char *labels[3] = { T("update.delete"), T("update.later"), T("update.install") };
    ui_popup_buttons(x, y, w, h, labels, 3, rdy_sel, 0);
}

/* pulsanti della finestra secondo la fase; con due, il secondo è l'azione (scelto all'inizio) */
static int upd_sel = 1, upd_sel_phase = -1;
static int upd_buttons(const update_status_t *s, const char **labels) {
    int p = s->phase;
    if (p == UPD_AVAILABLE) { labels[0] = T("common.cancel"); labels[1] = T("update.download"); return 2; }
    if (p == UPD_READY) { labels[0] = T("common.cancel"); labels[1] = T("update.install"); return 2; }
    if (p == UPD_DONE) { labels[0] = T("update.restart"); return 1; }
    if (p == UPD_INSTALL) return 0;
    if (upd_busy()) { labels[0] = T("common.cancel"); return 1; }
    labels[0] = T("common.ok");
    return 1;
}

static void upd_input(void) {
    const update_status_t *s = update_status();
    last_input = UI.t;   /* niente salvaschermo o spegnimento automatico durante l'aggiornamento */
    const char *labels[2];
    int n = upd_buttons(s, labels);
    if (upd_sel_phase != s->phase) { upd_sel_phase = s->phase; upd_sel = n - 1; }
    if (!n) return;
    int r = ui_popup_input(&upd_sel, n);
    if (r == -1) return;
    bool action = n == 2 ? r == 1 : r == 0;   /* B (-2) annulla sempre */
    if (upd_busy()) { if (s->phase != UPD_INSTALL) { update_cancel(); ui_sound(SND_BACK); } return; }
    if (s->phase == UPD_AVAILABLE) {
        if (action) { update_install(); ui_sound(SND_OK); }
        else { update_join(); upd_open = false; ui_sound(SND_BACK); }
        return;
    }
    if (s->phase == UPD_READY) {
        if (action) {
            if (!upd_battery_ok()) { ui_sound(SND_ERROR); return; }
            update_apply(); ui_sound(SND_OK);
        }
        else { update_discard(); ui_sound(SND_BACK); }
        return;
    }
    if (s->phase == UPD_DONE) { if (r == 0) { update_join(); upd_reboot(); } return; }   /* solo A riavvia */
    update_join(); upd_open = false; ui_sound(SND_BACK);
}

static void upd_draw(void) {
    theme_t *t = &UI.th;
    const update_status_t *s = update_status();
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    char line[200], key[32], sub[400] = "";
    switch (s->phase) {
    case UPD_SEARCH: snprintf(line, sizeof line, "%s", T("update.searching")); break;
    case UPD_CHECK: snprintf(line, sizeof line, "%s", T("update.checking")); break;
    case UPD_NO_SERVER: snprintf(line, sizeof line, "%s", T("update.none")); snprintf(sub, sizeof sub, "%s", T("update.none_hint")); break;
    case UPD_CURRENT: snprintf(line, sizeof line, "%s", T("update.current")); snprintf(sub, sizeof sub, "%s", update_current()); break;
    case UPD_AVAILABLE:
        snprintf(line, sizeof line, T("update.available"), s->version);
        if (s->reinstall) snprintf(sub, sizeof sub, "%s", T("update.reinstall"));
        break;
    case UPD_DOWNLOAD: case UPD_VERIFY: snprintf(line, sizeof line, "%s", T("update.downloading")); break;
    case UPD_READY:
        snprintf(line, sizeof line, "%s", T("update.ready"));
        snprintf(sub, sizeof sub, T("update.ready_hint"), s->version);
        if (s->reinstall) {   /* il Wi-Fi porta tutto tranne l'area di avvio: lo si dice prima */
            size_t l = strlen(sub);
            snprintf(sub + l, sizeof sub - l, " %s", T("update.reinstall"));
        }
        break;
    case UPD_INSTALL: snprintf(line, sizeof line, "%s", T("update.installing")); break;
    case UPD_DONE: snprintf(line, sizeof line, "%s", T("update.done")); snprintf(sub, sizeof sub, "%s", s->version); break;
    default:
        snprintf(key, sizeof key, "update.err%d", s->error);
        snprintf(line, sizeof line, "%s", T(key));
        snprintf(sub, sizeof sub, "%s", T("update.err_safe"));
    }
    bool bar = s->phase == UPD_CHECK || s->phase == UPD_DOWNLOAD || s->phase == UPD_VERIFY || s->phase == UPD_INSTALL;
    if (bar && s->total) {
        char a[32], b[32];
        fmt_bytes(s->done, a, sizeof a);
        fmt_bytes(s->total, b, sizeof b);
        snprintf(sub, sizeof sub, "%s / %s", a, b);
    }
    /* finestra alta quanto il contenuto, pulsanti dentro come in sospensione e spegnimento */
    const char *blabels[2];
    int nb = upd_buttons(s, blabels);
    int w = M(0.95f), pad = M(0.05f), bw = w - 2 * pad, bh = M(0.02f), sh = TTF_FontHeight(UI.f_small);
    char lines[3][160];
    int nl = sub[0] ? ui_wrap(UI.f_small, sub, bw, lines, 3) : 0;
    int h = pad + TTF_FontHeight(UI.f_title) + M(0.03f) + TTF_FontHeight(UI.f_normal) + M(0.03f)
          + (bar ? bh + M(0.03f) : 0) + nl * sh + (nb ? ui_popup_buttons_h() : pad);
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    if (t->surface.a < 200) ui_rrect(x, y, w, h, r, with_alpha(t->bg2, 230));
    int ty = y + pad;
    char upd_title[120];
    snprintf(upd_title, sizeof upd_title, "%s%s%s", T("update.title"), UI.set.update_channel ? " · " : "", UI.set.update_channel ? T("update.channel_dev") : "");
    ui_text(UI.f_title, upd_title, x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.03f);
    ui_text(UI.f_normal, line, x + pad, ty, s->phase == UPD_FAILED ? rgba(220, 70, 70, 255) : t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_normal) + M(0.03f);
    if (bar) {
        float frac = s->total ? (float)s->done / (float)s->total : 0.f;
        if (s->phase == UPD_INSTALL) frac = 1.f;
        if (frac > 1) frac = 1;
        ui_rrect(x + pad, ty, bw, bh, bh / 2, with_alpha(t->text_dim, 70));
        if (frac > 0) ui_rrect(x + pad, ty, (int)(bw * frac), bh, bh / 2, t->accent);
        ty += bh + M(0.03f);
    }
    for (int i = 0; i < nl; i++) ui_text(UI.f_small, lines[i], x + pad, ty + i * sh, t->text_dim, ALIGN_LEFT);
    ui_popup_buttons(x, y, w, h, blabels, nb, upd_sel_phase == s->phase ? upd_sel : nb - 1, -1);
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
    char core[16] = "", name[ROM_NAME] = "";
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
    char rom[ROM_NAME], l[48], m[120];
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
    if (set_page < 0 && is_bar_layout()) settings_open(-1, 0);   /* tema cambiato mentre si era qui */
    int rows[16], n = page_rows(rows);
    bool grid = set_page < 0 && !is_list_layout();   /* Tessera: sezioni su due colonne */
    if (grid) {
        int old = set_row;
        if (in->pressed[BTN_UP]) set_row = set_row >= 2 ? set_row - 2 : set_row + ((n - 1 - set_row) / 2) * 2;
        if (in->pressed[BTN_DOWN])   /* niente sotto ma una riga più in basso: l'ultima voce */
            set_row = set_row + 2 < n ? set_row + 2 : set_row / 2 < (n - 1) / 2 ? n - 1 : set_row % 2;
        if (in->pressed[BTN_LEFT] && set_row % 2) set_row--;
        if (in->pressed[BTN_RIGHT] && set_row % 2 == 0 && set_row + 1 < n) set_row++;
        if (set_row != old) ui_sound(SND_MOVE);
    } else {
        if (in->pressed[BTN_UP]) { set_row = (set_row + n - 1) % n; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_DOWN]) { set_row = (set_row + 1) % n; ui_sound(SND_MOVE); }
    }
    if (set_page >= 0 && is_bar_layout() && (in->pressed[BTN_L] || in->pressed[BTN_R])) {   /* Onda: sezione */
        settings_open((set_page + (in->pressed[BTN_L] ? SC_LAST : 1)) % SC_COUNT, 0);
        ui_sound(SND_MOVE);
        return;
    }
    if (set_row >= n) set_row = n - 1;
    set_sel = rows[set_row];
    /* Sostieni Mosaik: Y dalla pagina iniziale delle impostazioni (in Onda da ogni sezione) */
    if ((set_page < 0 || is_bar_layout()) && in->pressed[BTN_Y]) { support_open = true; ui_sound(SND_OK); return; }
    if (grid) {
        if (in->pressed[BTN_A]) { settings_open(set_sel - ROW_CAT, 0); ui_sound(SND_OK); }
        else if (in->pressed[BTN_B] || in->pressed[BTN_START]) { go(back_screen == SCR_SETTINGS ? SCR_HOME : back_screen); ui_sound(SND_BACK); }
        return;
    }
    if (in->pressed[BTN_B] || in->pressed[BTN_START]) {
        if (set_page >= 0 && in->pressed[BTN_B] && !is_bar_layout()) { settings_open(-1, set_page); ui_sound(SND_BACK); return; }
        go(back_screen == SCR_SETTINGS ? SCR_HOME : back_screen);
        ui_sound(SND_BACK);
        return;
    }
    if (set_sel >= ROW_CAT) {   /* categoria: A o destra la aprono */
        if (in->pressed[BTN_A] || in->pressed[BTN_RIGHT]) { settings_open(set_sel - ROW_CAT, 0); ui_sound(SND_OK); }
        return;
    }
    if (set_disabled(set_sel)) {
        if (in->pressed[BTN_A] || in->pressed[BTN_LEFT] || in->pressed[BTN_RIGHT]) {
            if (set_sel == S_SCRAPE || set_sel == S_UPDATE || set_sel == S_FILESERVER) ui_toast(T("scrape.no_wifi"));
            ui_sound(SND_ERROR);
        }
        return;
    }
    if (in->pressed[BTN_LEFT]) set_change(set_sel, -1);
    if (in->pressed[BTN_RIGHT]) set_change(set_sel, +1);
    if (in->pressed[BTN_A]) {
        if (set_sel == S_POWER) { dlg_open = true; dlg_suspend = false; dlg_sel = 0; ui_sound(SND_OK); }
        else if (set_sel == S_SUSPENDNOW) { ui_sound(SND_OK); ui_suspend(); last_input = UI.t; }
        else if (set_sel == S_RESTART) {
            if (plat_standalone()) { cfm_kind = CFM_RESTART; cfm_sel = 0; ui_sound(SND_OK); }
            else quit_to_menu();
        }
        else if (set_sel == S_CLOCK) { clock_start(); ui_sound(SND_OK); }
        else if (set_sel == S_CLEANMEDIA) clean_start();
        else if (set_sel == S_UPDATE) upd_start();
        else if (set_sel == S_GENRES) gp_show(GP_MANAGE);
        else if (set_sel == S_COLLS) {
            if (NCOLLS) { cp_open = true; cp_sel = 0; ui_sound(SND_OK); }
            else { ui_toast(T("coll.none")); ui_sound(SND_ERROR); }
        }
        else if (set_sel == S_PADTEST) { ptest_open = true; ptest_hold = 0; ui_sound(SND_OK); }
        else if (set_sel == S_STICKCAL) { sc_open(); ui_sound(SND_OK); }
        else if (set_sel == S_WIFI) { set_change(S_WIFI, +1); ui_sound(SND_OK); }
        else if (set_sel == S_WIFINETS) {
            if (UI.set.wifi) { wpanel_start(); ui_sound(SND_OK); }
            else ui_sound(SND_ERROR);
        }
        else if (set_sel == S_ROMCHECK) {
            quiet_begin();
            rcq_open = true; rcq_sel = 0; ui_sound(SND_OK);
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
        else if (set_sel == S_SUPPORT) { support_open = true; ui_sound(SND_OK); }
        else if (set_sel == S_FILESERVER) fsrv_start();
        else if (set_sel == S_STORE) { sto_start(); ui_sound(SND_OK); }
        else if (set_sel == S_RESETSET || set_sel == S_ERASE) { cfm_kind = set_sel == S_ERASE ? CFM_ERASE : CFM_RESETSET; cfm_sel = 0; ui_sound(SND_OK); }
        else set_change(set_sel, +1);
    }
}

static void draw_switch(int x, int y, int h, bool on) {
    theme_t *t = &UI.th;
    int w = h * 9 / 5;
    ui_rrect(x, y, w, h, h / 2, on ? t->accent : with_alpha(t->text_dim, 110));
    ui_circle(on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - h / 8, rgba(255, 255, 255, 255));
}

/* --- impostazioni: sezioni (schede in Tessera, menu numerato in Cabinato, pulsanti in Onda) e voci in riquadri --- */
static const char *set_cat_title(int row) {
    static char k[64];
    snprintf(k, sizeof k, "settings_cat.%s", cat_key[row - ROW_CAT]);
    return T(k);
}
static const char *set_cat_sub(int row) {
    static char k[64];
    snprintf(k, sizeof k, "settings_cat_sum.%s", cat_key[row - ROW_CAT]);
    return T(k);
}
static bool set_has_value(int i) { return i != S_FILESERVER && i != S_RESETSET && i != S_ERASE && i != S_STORE && i != S_POWER && i != S_SUSPENDNOW && i != S_RESTART && i != S_SCRAPE && i != S_ROMCHECK && i != S_CLEANMEDIA; }
static bool set_slider(int i) { return i == S_BRIGHT || i == S_VOLUME || i == S_LEDBRIGHT; }
static bool set_switch(int i) { return i == S_WIFI || i == S_RESUME || i == S_BEZEL || i == S_AUTOSAVE || i == S_BOXCOVER; }

/* riquadro di una voce: bordo del colore del tema se scelto */
static void set_box(int x, int y, int w, int h, int r, bool sel) {
    theme_t *t = &UI.th;
    ui_rrect(x, y, w, h, r, t->surface);
    int b = M(0.004f) < 3 ? 3 : M(0.004f);
    if (sel) ui_rborder(x, y, w, h, r, b, t->accent);
    else ui_rborder(x, y, w, h, r, 2, t->line);
}

/* valore di una voce: cursore, interruttore o testo (con le frecce se si può cambiare).
   right: x è il bordo destro; altrimenti il bordo sinistro. maxw: spazio disponibile. */
static void set_draw_value(int i, TTF_Font *f, int x, int cy, int maxw, bool sel, bool right) {
    theme_t *t = &UI.th;
    int fh = TTF_FontHeight(f);
    char val[160], fit[160];
    set_value(i, val, sizeof val);
    if (set_disabled(i)) {
        if (i == S_SCRAPE) return;
        ui_text_fit(f, val, maxw, fit, sizeof fit);
        ui_text(f, fit, x, cy - fh / 2, with_alpha(t->text_dim, 120), right ? ALIGN_RIGHT : ALIGN_LEFT);
        return;
    }
    if (!set_has_value(i)) return;
    if (set_slider(i)) {
        int nw = ui_text_w(f, "20"), bh = M(0.012f), kr = bh * 3 / 2, bw = M(0.26f);
        if (bw > maxw - nw - M(0.03f) - 2 * kr) bw = maxw - nw - M(0.03f) - 2 * kr;
        int tot = kr + bw + M(0.03f) + nw, x0 = (right ? x - tot : x) + kr;
        int mn = i == S_VOLUME ? 0 : 1, mxv = i == S_VOLUME ? 20 : 10;
        int v = i == S_BRIGHT ? UI.set.brightness : i == S_VOLUME ? UI.set.volume : UI.set.led_bright;
        ui_rrect(x0, cy - bh / 2, bw, bh, bh / 2, with_alpha(t->text_dim, 80));
        int fw = bw * (v - mn) / (mxv - mn);
        ui_rrect(x0, cy - bh / 2, fw, bh, bh / 2, t->accent);
        ui_circle(x0 + fw, cy, kr, sel ? t->accent : t->surface);
        ui_ring(x0 + fw, cy, kr, 2, t->accent);
        ui_text(f, val, x0 + bw + M(0.03f) + nw, cy - fh / 2, t->text, ALIGN_RIGHT);
        return;
    }
    if (set_switch(i)) {
        bool on = i == S_WIFI ? UI.set.wifi : i == S_BEZEL ? UI.set.bezel : i == S_AUTOSAVE ? UI.set.autosave_exit : i == S_BOXCOVER ? UI.set.box_covers : UI.set.resume_last;
        int sh = f == UI.f_small ? M(0.032f) : M(0.04f), sw = sh * 9 / 5;
        draw_switch(right ? x - sw : x, cy - sh / 2, sh, on);
        return;
    }
    color_t vc = sel ? t->accent : t->text_dim;
    bool arrows = sel && i != S_INFO && i != S_SUPPORT && i != S_CLOCK && i != S_UPDATE && i != S_GENRES && i != S_COLLS && i != S_PADTEST && i != S_STICKCAL && i != S_WIFINETS;
    int aw = arrows ? ui_text_w(f, "‹") + M(0.015f) : 0, rr = fh / 3, dw = i == S_LEDCOLOR ? 2 * rr + M(0.015f) : 0;
    ui_text_fit(f, val, maxw - 2 * aw - dw, fit, sizeof fit);
    int tw = ui_text_w(f, fit), x0 = right ? x - (2 * aw + dw + tw) : x;
    if (arrows) ui_text(f, "‹", x0, cy - fh / 2, vc, ALIGN_LEFT);
    int cx = x0 + aw;
    if (i == S_LEDCOLOR) {   /* pallino del colore (per "tema": il colore principale del tema) */
        unsigned c = UI.set.led_color ? LED_COLORS[UI.set.led_color % NLED_COLORS].rgb
                                      : (unsigned)t->accent.r << 16 | t->accent.g << 8 | t->accent.b;
        ui_circle(cx + rr, cy, rr, rgba(c >> 16 & 255, c >> 8 & 255, c & 255, 255));
        ui_ring(cx + rr, cy, rr, 1, with_alpha(t->text_dim, 120));
        cx += dw;
    }
    ui_text(f, fit, cx, cy - fh / 2, vc, ALIGN_LEFT);
    if (arrows) ui_text(f, "›", cx + tw + M(0.015f), cy - fh / 2, vc, ALIGN_LEFT);
}

static color_t set_label_color(int i, bool sel) {
    theme_t *t = &UI.th;
    if (set_disabled(i)) return with_alpha(t->text_dim, 120);
    if (i == S_POWER && sel) return rgba(220, 70, 70, 255);
    return t->text;
}

/* Tessera: sezioni in schede su due colonne, con nome e cosa contengono */
static void settings_draw_grid(int y, int bottom) {
    theme_t *t = &UI.th;
    int rows[16], n = page_rows(rows);
    int mx = M(t->margin), W = UI.w - 2 * mx, gap = M(0.02f), r = M(t->panel_radius) * 2, pad = M(0.022f);
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int nr = (n + 1) / 2, cw = (W - gap) / 2, ch = (bottom - y - (nr - 1) * gap) / nr;
    if (ch > fnh + 2 * fsh + 3 * pad) ch = fnh + 2 * fsh + 3 * pad;
    for (int k = 0; k < n; k++) {
        int x = mx + (k % 2) * (cw + gap), yy = y + (k / 2) * (ch + gap);
        bool sel = k == set_row;
        set_box(x, yy, cw, ch, r, sel);
        char fit[160], lines[2][160];
        ui_text_fit(fn, set_cat_title(rows[k]), cw - 2 * pad, fit, sizeof fit);
        int nl = ui_wrap(fs, set_cat_sub(rows[k]), cw - 2 * pad, lines, 2);
        int ty = yy + (ch - fnh - nl * fsh - M(0.008f)) / 2;
        ui_text(fn, fit, x + pad, ty, t->text, ALIGN_LEFT);
        for (int l = 0; l < nl; l++) ui_text(fs, lines[l], x + pad, ty + fnh + M(0.008f) + l * fsh, t->text_dim, ALIGN_LEFT);
    }
}

/* Cabinato: menu numerato a sinistra, a destra le voci della sezione con i valori */
static void settings_draw_menu(int y, int bottom) {
    theme_t *t = &UI.th;
    int rows[16], n = page_rows(rows);
    int mx = M(t->margin), W = UI.w - 2 * mx, lw = W * 40 / 100, r = M(t->panel_radius), gap = M(0.012f);
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int rh = (bottom - y - (n - 1) * gap) / n;
    if (rh > fnh + M(0.045f)) rh = fnh + M(0.045f);
    for (int k = 0; k < n; k++) {
        int yy = y + k * (rh + gap);
        bool sel = k == set_row;
        set_box(mx, yy, lw, rh, r, sel);
        char num[8], fit[120];
        snprintf(num, sizeof num, "%02d", k + 1);
        ui_text(fn, num, mx + M(0.016f), yy + (rh - fnh) / 2, sel ? t->accent : t->text, ALIGN_LEFT);
        int lx = mx + M(0.016f) + ui_text_w(fn, "00") + M(0.022f);
        ui_text_fit(fn, set_cat_title(rows[k]), mx + lw - lx - M(0.012f), fit, sizeof fit);
        ui_text(fn, fit, lx, yy + (rh - fnh) / 2, t->text, ALIGN_LEFT);
    }
    int px = mx + lw + M(0.025f), pw = mx + W - px, ph = n * (rh + gap) - gap, pad = M(0.022f);
    set_box(px, y, pw, ph, r, false);
    int sel = rows[set_row], ty = y + pad, iw = pw - 2 * pad;
    small_caps(set_cat_title(sel), px + pad, ty, t->accent);
    ty += fsh + M(0.02f);
    int items[16], ni = page_rows_of(sel - ROW_CAT, items);
    /* sempre la stessa misura: se le voci non ci stanno tutte, l'ultima riga dice che ce ne sono altre */
    TTF_Font *f = fn;
    int lh = fnh + M(0.014f), fh = fnh, fit_rows = (y + ph - pad - ty - fh) / lh + 1;
    for (int k = 0; k < ni && k < fit_rows; k++, ty += lh) {
        int i = items[k];
        if (k == fit_rows - 1 && ni > fit_rows) { ui_text(f, "…", px + pad, ty, t->text_dim, ALIGN_LEFT); break; }
        char fit[160], val[160];
        ui_text_fit(f, set_text("settings", i), iw * 58 / 100, fit, sizeof fit);
        int lw2 = ui_text(f, fit, px + pad, ty, set_disabled(i) ? with_alpha(t->text_dim, 120) : t->text, ALIGN_LEFT);
        if (!set_has_value(i) || (set_disabled(i) && i == S_SCRAPE)) continue;
        set_value(i, val, sizeof val);
        ui_text_fit(f, val, iw - lw2 - M(0.03f), fit, sizeof fit);
        ui_text(f, fit, px + pw - pad, ty, t->text_dim, ALIGN_RIGHT);
    }
}

/* Onda: le sezioni sono pulsanti in alto, L e R le cambiano */
static int settings_draw_tabs(int y) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), avail = UI.w - 2 * mx, gap = M(0.014f);
    static float off, off_target;
    const char *lab[SC_COUNT];
    for (int c = 0; c <= SC_LAST; c++) lab[c] = set_cat_title(ROW_CAT + c);
    TTF_Font *f = UI.f_normal;
    int tot = 0;
    for (int c = 0; c <= SC_LAST; c++) tot += ui_text_w(f, lab[c]) + 2 * M(0.022f) + gap;
    if (tot - gap > avail) f = UI.f_small;
    int xs[SC_COUNT], ws[SC_COUNT];
    tot = 0;
    for (int c = 0; c <= SC_LAST; c++) {
        ws[c] = ui_text_w(f, lab[c]) + 2 * M(f == UI.f_small ? 0.018f : 0.022f);
        xs[c] = tot;
        tot += ws[c] + gap;
    }
    int cur = set_page < 0 ? 0 : set_page;
    if (xs[cur] + ws[cur] - off_target > avail)   /* si parte sempre dall'inizio di un pulsante */
        for (int c = 0; c <= cur; c++)
            if (xs[cur] + ws[cur] - xs[c] <= avail) { off_target = (float)xs[c]; break; }
    if (xs[cur] < off_target) off_target = (float)xs[cur];
    off = ui_approach(off, off_target);
    int h = TTF_FontHeight(f) + M(0.022f);
    ui_clip_push(mx - 2, y - 2, avail + 4, h + 4);
    for (int c = 0; c <= SC_LAST; c++) {   /* solo pulsanti interi: quelli tagliati compaiono scorrendo */
        int x = mx + xs[c] - (int)off;
        if (x >= mx - M(0.004f) && x + ws[c] <= mx + avail + M(0.004f)) pill_f(f, lab[c], x, y, c == cur);
    }
    ui_clip_pop();
    return h;
}

static void settings_draw(void) {
    theme_t *t = &UI.th;
    topbar_theme();
    int mx = M(t->margin), W = UI.w - 2 * mx, y = M(t->topbar_h) + M(0.025f);
    int bottom = UI.h - ui_keybar_h() - M(0.03f);
    bool onda = is_bar_layout();
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    home_title(set_page < 0 || onda ? T("settings.title") : set_cat_title(ROW_CAT + set_page), y, t->text);
    if (set_page < 0 || onda) {   /* Sostieni Mosaik, ben visibile: a destra del titolo, si apre con Y */
        const char *lab = T("support.title");
        int fnh = TTF_FontHeight(fn), ph = fnh + M(0.018f), d = ph - M(0.014f), pw = M(0.016f) + d + M(0.014f) + ui_text_w(fn, lab) + M(0.024f);
        int px = UI.w - mx - pw, py = y + (TTF_FontHeight(UI.f_title) - ph) / 2;
        ui_rrect(px, py, pw, ph, ph / 2, with_alpha(t->accent, 40));
        ui_rborder(px, py, pw, ph, ph / 2, M(0.003f) + 1, t->accent);
        int cx = px + M(0.016f);
        ui_rrect(cx, py + (ph - d) / 2, d, d, d / 2, t->accent);
        ui_text(fs, "Y", cx + d / 2, py + (ph - TTF_FontHeight(fs)) / 2, t->accent_text, ALIGN_CENTER);
        ui_text(fn, lab, cx + d + M(0.014f), py + (ph - fnh) / 2, t->accent, ALIGN_LEFT);
    }
    y += TTF_FontHeight(UI.f_title) + M(0.03f);
    if (set_page < 0) {
        if (is_list_layout()) settings_draw_menu(y, bottom); else settings_draw_grid(y, bottom);
    } else {
        if (onda) y += settings_draw_tabs(y) + M(0.03f);
        int rows[16], nrows = page_rows(rows);
        int r = is_list_layout() ? M(t->panel_radius) : M(t->panel_radius) * 2, pad = M(0.022f), gap = M(0.014f);
        int rh = onda ? fnh + fsh + M(0.03f) : fnh + M(0.032f), step = rh + gap;
        int desc_h = 2 * fsh + M(0.012f);
        int area_h = bottom - desc_h - y;
        area_h -= (area_h + gap) % step;   /* solo riquadri interi: niente voci tagliate a metà */
        float ry = (float)set_row * step, target = set_scroll;
        if (ry - target > area_h - rh) target = ry - (area_h - rh);
        if (ry < target) target = ry;
        set_scroll = ui_approach(set_scroll, target);
        ui_clip_push(0, y - M(0.008f), UI.w, area_h + M(0.016f));
        for (int k = 0; k < nrows; k++) {
            int i = rows[k], yy = y + (int)(k * step - set_scroll);
            if (yy + rh < y - step || yy > y + area_h) continue;
            bool sel = k == set_row;
            set_box(mx, yy, W, rh, r, sel);
            char fit[160];
            if (onda) {   /* nome in alto, valore sotto (cursori e interruttori a destra) */
                ui_text_fit(fn, set_text("settings", i), W - 2 * pad, fit, sizeof fit);
                bool one = !set_has_value(i) && !set_disabled(i);   /* senza valore: nome al centro */
                int ty = one ? yy + (rh - fnh) / 2 : yy + (rh - fnh - fsh) / 2;
                ui_text(fn, fit, mx + pad, ty, set_label_color(i, sel), ALIGN_LEFT);
                if (set_slider(i) || set_switch(i)) set_draw_value(i, fs, mx + pad, ty + fnh + fsh / 2 + M(0.006f), W / 2, sel, false);
                else set_draw_value(i, fs, mx + pad, ty + fnh + fsh / 2, W - 2 * pad, sel, false);
            } else {
                int cy = yy + rh / 2;
                ui_text_fit(fn, set_text("settings", i), W * 55 / 100, fit, sizeof fit);
                int lw = ui_text(fn, fit, mx + pad, cy - fnh / 2, set_label_color(i, sel), ALIGN_LEFT);
                set_draw_value(i, fn, mx + W - pad, cy, W - 2 * pad - lw - M(0.03f), sel, true);
            }
        }
        ui_clip_pop();
        /* a cosa serve la voce scelta */
        char lines[2][160];
        int n = ui_wrap(fs, set_text("settings_desc", set_sel), W, lines, 2);
        for (int k = 0; k < n; k++) ui_text(fs, lines[k], mx, bottom - desc_h + M(0.012f) + k * fsh, t->text_dim, ALIGN_LEFT);
    }
    /* con una finestra aperta, la barra dei tasti è quella della finestra */
    if (clock_open || sdinfo_open || support_open || wpanel_open || clean_open || fsrv_open) return;
    int end;
    if (set_page < 0) {
        const char *h[] = { "A", T("common.open"), NULL, "B", T("common.back"), NULL };
        end = ui_keybar(h, 6);
    } else {
        bool act = set_sel == S_POWER || set_sel == S_SUSPENDNOW || set_sel == S_RESTART || set_sel == S_CLOCK || set_sel == S_UPDATE || set_sel == S_GENRES || set_sel == S_COLLS ||
                   set_sel == S_PADTEST || set_sel == S_STICKCAL || set_sel == S_WIFINETS || set_sel == S_ROMCHECK ||
                   set_sel == S_SCRAPE || set_sel == S_CLEANMEDIA || set_sel == S_INFO || set_sel == S_FILESERVER ||
                   set_sel == S_RESETSET || set_sel == S_ERASE || set_sel == S_STORE || set_sel == S_SUPPORT;
        const char *h[] = { "A", T(act ? "common.open" : "common.change"), NULL, "B", T("common.back"), NULL,
                            "L/R", T("common.section"), "" };
        end = ui_keybar(h, onda ? 9 : 6);
    }
    if ((set_page < 0 || onda) && update_supported()) {   /* versione: utile quando si segnala un problema */
        char ver[96];
        snprintf(ver, sizeof ver, "Mosaik %s", update_current());
        int vw = ui_text_w(fs, ver);
        if (mx + vw + M(0.03f) < end)
            ui_text(fs, ver, mx, UI.h - ui_keybar_h() / 2 - fsh / 2, t->text_dim, ALIGN_LEFT);
    }
}

/* --------------------------------------------------------- dialogo */

/* Sulla console, finché il sistema gira come app del firmware originale, c'è anche
   "Esci": torna al menu TrimUI. Pulsanti: annulla, (esci), spegni. */
/* Annulla e Spegni; in mezzo Riavvia nel sistema su SD, Esci se si è partiti dal sistema TrimUI */
static int dlg_buttons(void) { return plat_standalone() || plat_is_device() ? 3 : 2; }

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
        if (n == 3 && dlg_sel == 1) { if (plat_standalone()) restart_system(); else quit_to_menu(); }
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
    ui_text(UI.f_small, T(dlg_buttons() == 3 && !plat_standalone() ? "dialog.power_body_app" : "dialog.power_body"), x + w / 2, y + M(0.06f) + TTF_FontHeight(UI.f_title) + M(0.01f), t->text_dim, ALIGN_CENTER);
    int n = dlg_buttons(), bw = (w - M(0.03f) * (n + 1)) / n, bh = M(0.085f), by = y + h - bh - M(0.04f);
    const char *labels[3] = { T("common.cancel"), n == 3 ? T(plat_standalone() ? "common.restart" : "common.quit_menu") : T("common.power_off"), T("common.power_off") };
    for (int i = 0; i < n; i++) {
        int bx = x + M(0.03f) + i * (bw + M(0.03f));
        bool sel = dlg_sel == i;
        ui_rrect(bx, by, bw, bh, bh / 2, sel ? t->accent : with_alpha(t->text_dim, 40));
        TTF_Font *lf = ui_text_w(UI.f_normal, labels[i]) > bw - M(0.02f) ? UI.f_small : UI.f_normal;   /* tre pulsanti: le parole lunghe devono starci */
        ui_text(lf, labels[i], bx + bw / 2, by + (bh - TTF_FontHeight(lf)) / 2, sel ? t->accent_text : t->text, ALIGN_CENTER);
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

static int setup_sel = 1;
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
        int r = ui_popup_input(&setup_sel, 2);   /* "Più tardi" o "Collega" */
        if (r == 1) { ui_sound(SND_OK); setup_finish(true); }
        else if (r == 0 || r == -2) { ui_sound(SND_BACK); setup_finish(false); }
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
        int h = pad + ft + M(0.03f) + fn * n + ui_popup_buttons_h();
        panel_box(w, h, &x, &y);
        int ty = y + pad;
        ui_text(UI.f_title, T("setup.wifi_title"), x + pad, ty, t->text, ALIGN_LEFT);
        ty += ft + M(0.03f);
        for (int i = 0; i < n; i++, ty += fn) ui_text(UI.f_normal, lines[i], x + pad, ty, t->text, ALIGN_LEFT);
        const char *labels[2] = { T("setup.later"), T("setup.connect") };
        ui_popup_buttons(x, y, w, h, labels, 2, setup_sel, -1);
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

/* ------------------------------------------------ calibrazione levette */
/* Centro a levette ferme, poi i limiti ruotandole fino in fondo. Sulla SD si scrive solo alla
   fine: annullando, o spegnendo a metà, resta tutto com'era. */

enum { SCS_CLOSED, SCS_INTRO, SCS_PREP, SCS_CENTER, SCS_RANGE, SCS_APPLY, SCS_CANCEL, SCS_DONE, SCS_FAIL };
static int sc_state;
static double sc_t;
static stickcal_t sc_c;
static long long sc_sum[SC_AXES];
static int sc_n;
static const char *sc_ud(void) {
#ifdef TRIMUI_DEVICE
    return "/mnt/UDISK";
#else
    static char p[1100];   /* sul Mac: una cartella qualunque al posto di quella in memoria */
    snprintf(p, sizeof p, "%s/System/udisk-prova", UI.root);
    mkdir(p, 0755);
    return p;
#endif
}

static bool sc_brick(void) { const char *m = plat_model(); return m && !strcmp(m, "brickpro"); }
static void sc_sysdir(char *out, size_t n) { snprintf(out, n, "%s/System", UI.root); }

/* il programma dei tasti rilegge i valori: la Brick Pro lo fa a comando, la Smart Pro riavviandolo */
static void sc_reload(void) {
#ifdef TRIMUI_DEVICE
    int r = system(sc_brick() ? "touch /tmp/trimui_inputd/cal_update"
                              : "killall trimui_inputd; sleep 0.5; trimui_inputd > /dev/null 2>&1 &");
    (void)r;
#endif
}

static int sc_axis(int a) {
#ifndef TRIMUI_DEVICE
    if (getenv("TRIMUI_FAKE_STICKS")) {   /* solo per le prove sul Mac */
        static const int centre[SC_AXES] = { 2106, 2191, 1975, 2140 };
        double swing = sc_state == SCS_RANGE ? sin(UI.t * 3 + a) * (UI.t - sc_t > 2 ? 1000 : 300) : 0;
        return -((centre[a] + (int)swing) - 300) * 32760 / 3795;
    }
#endif
    return UI.in.axis[a];
}

/* la Brick Pro ha una corsa molto più corta della Smart Pro: valori di fabbrica 1450-2750 contro
   1050-2900, e una levetta col centro spostato ha un lato ancora più corto */
static int sc_expected_span(void) { return sc_brick() ? 650 : 1150; }

static void sc_open(void) {
    stickcal_set_min_span(sc_brick() ? 250 : 450);
    sc_state = SCS_INTRO;
}

static void sc_restore_factory(void) {
    char sys[1100];
    sc_sysdir(sys, sizeof sys);
    stickcal_reset(sys, sc_ud(), sc_brick());
    sc_reload();
    ui_log("calibrazione levette: tolta, valori di fabbrica");
    ui_toast(T("stickcal.restored"));
}

static void sc_go(int state) { sc_state = state; sc_t = UI.t; }

static void sc_log_range(const char *why) {
    for (int a = 0; a < SC_AXES; a++)
        ui_log("calibrazione levette (%s): asse %d min %d centro %d max %d", why, a, sc_c.min[a], sc_c.zero[a], sc_c.max[a]);
}

static void sc_abort(void) {
    if (sc_state == SCS_RANGE) sc_log_range("annullata");
    stickcal_cancel(sc_ud(), sc_brick());
    sc_reload();
    ui_sound(SND_BACK);
    sc_go(SCS_CANCEL);
}

static void sc_input(void) {
    input_t *in = &UI.in;
    double el = UI.t - sc_t;
    switch (sc_state) {
    case SCS_INTRO: {
        char sys[1100];
        sc_sysdir(sys, sizeof sys);
        if (in->pressed[BTN_B]) { sc_state = SCS_CLOSED; ui_sound(SND_BACK); }
        else if (in->pressed[BTN_X] && stickcal_has_custom(sys)) { cfm_kind = CFM_STICKS; cfm_sel = 0; sc_state = SCS_CLOSED; ui_sound(SND_OK); }
        else if (in->pressed[BTN_A]) {
            ui_sound(SND_OK);
            if (stickcal_begin(sc_ud(), sc_brick())) { sc_reload(); sc_go(SCS_PREP); }
            else { stickcal_cancel(sc_ud(), sc_brick()); sc_go(SCS_FAIL); }
        }
        break;
    }
    case SCS_PREP:
        if (in->pressed[BTN_B]) sc_abort();
        else if (el > 2.5) { memset(sc_sum, 0, sizeof sc_sum); sc_n = 0; sc_go(SCS_CENTER); }
        break;
    case SCS_CENTER:
        if (in->pressed[BTN_B]) { sc_abort(); break; }
        if (el > 0.5) {
            for (int a = 0; a < SC_AXES; a++) sc_sum[a] += stickcal_raw(sc_axis(a));
            sc_n++;
        }
        if (el > 2.5) {
            bool signal = sc_n > 0;
            for (int a = 0; a < SC_AXES && signal; a++) {
                int z = (int)(sc_sum[a] / sc_n);
                sc_c.zero[a] = sc_c.min[a] = sc_c.max[a] = z;
                signal = z >= 900;   /* con questi valori una levetta ferma non può stare così in basso */
            }
            ui_log("calibrazione levette: centro %d %d %d %d", sc_c.zero[0], sc_c.zero[1], sc_c.zero[2], sc_c.zero[3]);
            if (signal) sc_go(SCS_RANGE);
            else { stickcal_cancel(sc_ud(), sc_brick()); sc_reload(); sc_go(SCS_FAIL); ui_sound(SND_ERROR); }
        }
        break;
    case SCS_RANGE:
        for (int a = 0; a < SC_AXES; a++) {
            int r = stickcal_raw(sc_axis(a));
            if (r < 0) continue;
            if (r < sc_c.min[a]) sc_c.min[a] = r;
            if (r > sc_c.max[a]) sc_c.max[a] = r;
        }
        if (in->pressed[BTN_B]) sc_abort();
        else if (in->pressed[BTN_A] && stickcal_ok(&sc_c)) {
            char sys[1100];
            sc_sysdir(sys, sizeof sys);
            bool ok = stickcal_save(sys, sc_ud(), &sc_c, sc_brick());
            sc_log_range("salvata");
            sc_reload();
            ui_sound(ok ? SND_OK : SND_ERROR);
            sc_go(ok ? SCS_APPLY : SCS_FAIL);
        }
        break;
    case SCS_APPLY:
        if (el > 2.5) sc_go(SCS_DONE);
        break;
    case SCS_CANCEL:
        if (el > 2.5) sc_state = SCS_CLOSED;
        break;
    case SCS_DONE:
    case SCS_FAIL:
        if (in->pressed[BTN_A] || in->pressed[BTN_B]) { sc_state = SCS_CLOSED; ui_sound(SND_BACK); }
        break;
    }
}

static void sc_dial(int cx, int cy, int r, int xaxis, int yaxis, const char *label) {
    theme_t *t = &UI.th;
    int dot = r / 5;
    bool ok = sc_state == SCS_RANGE && stickcal_axis_ok(&sc_c, xaxis) && stickcal_axis_ok(&sc_c, yaxis);
    ui_circle(cx, cy, r, ok ? with_alpha(t->accent, 90) : with_alpha(t->text, 26));
    ui_rect(cx - r, cy, 2 * r, 1, t->line);
    ui_rect(cx, cy - r, 1, 2 * r, t->line);
    int dx = 0, dy = 0;
    if (sc_state == SCS_RANGE) {
        int rx = stickcal_raw(sc_axis(xaxis)), ry = stickcal_raw(sc_axis(yaxis));
        int span = sc_expected_span();
        if (rx >= 0) dx = -(rx - sc_c.zero[xaxis]) * (r - dot) / span;
        if (ry >= 0) dy = -(ry - sc_c.zero[yaxis]) * (r - dot) / span;
        if (dx > r - dot) dx = r - dot;
        if (dx < dot - r) dx = dot - r;
        if (dy > r - dot) dy = r - dot;
        if (dy < dot - r) dy = dot - r;
    }
    ui_circle(cx + dx, cy + dy, dot, t->accent);
    ui_text(UI.f_small, label, cx, cy - r - TTF_FontHeight(UI.f_small) - M(0.008f), t->text_dim, ALIGN_CENTER);
}

static void sc_draw(void) {
    theme_t *t = &UI.th;
    int pad = M(0.04f), w = UI.w - 2 * M(t->margin), h = UI.h - M(t->hintbar_h) - 2 * M(0.03f), x, y;
    panel_box(w, h, &x, &y);
    ui_text(UI.f_title, T("stickcal.title"), x + pad, y + pad, t->text, ALIGN_LEFT);
    const char *msg = sc_state == SCS_INTRO ? T("stickcal.intro") : sc_state == SCS_PREP ? T("stickcal.prep") :
                      sc_state == SCS_CENTER ? T("stickcal.center") : sc_state == SCS_RANGE ? T("stickcal.range") :
                      sc_state == SCS_APPLY || sc_state == SCS_CANCEL ? T("stickcal.wait") :
                      sc_state == SCS_DONE ? T("stickcal.done") : T("stickcal.fail");
    char lines[4][160];
    int top = y + pad + TTF_FontHeight(UI.f_title) + M(0.03f), lh = TTF_FontHeight(UI.f_normal) + M(0.006f);
    int n = ui_wrap(UI.f_normal, msg, w - 2 * pad, lines, 4);
    for (int i = 0; i < n; i++) ui_text(UI.f_normal, lines[i], x + pad, top + i * lh, t->text, ALIGN_LEFT);
    int bottom = y + h - pad, ctop = top + n * lh + M(0.02f) + TTF_FontHeight(UI.f_small);
    int r = (bottom - ctop) / 2, cy = (ctop + bottom) / 2;
    if (r > M(0.15f)) r = M(0.15f);
    if (sc_state == SCS_CENTER || sc_state == SCS_RANGE) {
        sc_dial(x + w / 4, cy, r, SC_LX, SC_LY, T("ptest.left"));
        sc_dial(x + 3 * w / 4, cy, r, SC_RX, SC_RY, T("ptest.right"));
    }
    if (sc_state == SCS_INTRO) {
        char sys[1100];
        sc_sysdir(sys, sizeof sys);
        const char *h1[] = { "A", T("stickcal.start"), "X", T("confirm.restore"), "B", T("common.back") };
        const char *h2[] = { "A", T("stickcal.start"), "B", T("common.back") };
        if (stickcal_has_custom(sys)) ui_hints(h1, 6); else ui_hints(h2, 4);
    } else if (sc_state == SCS_RANGE) {
        const char *h1[] = { "A", T("stickcal.save"), "B", T("common.cancel") };
        const char *h2[] = { "B", T("common.cancel") };
        if (stickcal_ok(&sc_c)) ui_hints(h1, 4); else ui_hints(h2, 2);
    } else if (sc_state == SCS_PREP || sc_state == SCS_CENTER) {
        const char *h[] = { "B", T("common.cancel") };
        ui_hints(h, 2);
    } else if (sc_state == SCS_DONE || sc_state == SCS_FAIL) {
        const char *h[] = { "A", T("common.ok") };
        ui_hints(h, 2);
    }
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
    if (emu && emu->launch[0]) snprintf(core, sizeof core, "%s/%s", UI.root, emu->launch);   /* emulatore a sé: il suo script */
    else snprintf(core, sizeof core, "%s/Cores/%s.%s", UI.root, emu ? emu->core : g->core, CORE_EXT);
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
    h += ui_popup_buttons_h() - pad;
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
    const char *label[1] = { T("common.ok") };
    ui_popup_buttons(x, y, w, h, label, 1, 0, -1);
}

/* ------------------------------------------------- gestione delle collezioni */

static void lists_changed(void);

static void cp_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_UP]) { cp_sel = (cp_sel + NCOLLS - 1) % NCOLLS; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { cp_sel = (cp_sel + 1) % NCOLLS; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { cp_open = false; ui_sound(SND_BACK); return; }
    if (in->pressed[BTN_A]) {
        snprintf(coll_name, sizeof coll_name, "%s", COLLS[cp_sel]);
        kb_mode = KBM_COLL_RENAME;
        kb_open = true;
        kb_row = 1;
        kb_col = 0;
        ui_sound(SND_OK);
    } else if (in->pressed[BTN_X]) {
        cfm_kind = CFM_COLL;
        cfm_sel = 0;
        ui_sound(SND_OK);
    }
}

static void cp_rename_done(void) {
    if (games_rename_coll(UI.root, cp_sel, coll_name, games, ngames)) {
        coll_sel = COLL_ALL;
        ui_toast(T("coll.renamed"));
        lists_changed();
        ui_sound(SND_OK);
    } else {
        ui_toast(T("coll.failed"));
        ui_sound(SND_ERROR);
    }
}

static void cp_draw(void) {
    theme_t *t = &UI.th;
    int rh = M(t->list_row_h), pad = M(0.04f), ft = TTF_FontHeight(UI.f_title), fh = TTF_FontHeight(UI.f_normal);
    int maxrows = (UI.h - M(t->hintbar_h) - M(0.2f) - ft) / rh, vis = NCOLLS < maxrows ? NCOLLS : maxrows;
    int w = M(0.95f) < UI.w - 2 * M(t->margin) ? M(0.95f) : UI.w - 2 * M(t->margin), x, y;
    panel_box(w, pad * 2 + ft + M(0.02f) + rh * vis, &x, &y);
    ui_text(UI.f_title, T("settings.collections"), x + pad, y + pad, t->text, ALIGN_LEFT);
    int ry = y + pad + ft + M(0.02f), first = cp_sel - vis + 1 > 0 ? cp_sel - vis + 1 : 0;
    for (int i = first; i < first + vis && i < NCOLLS; i++, ry += rh) {
        bool sel = i == cp_sel;
        if (sel) ui_rrect(x + M(0.02f), ry, w - M(0.04f), rh - M(0.008f), M(t->panel_radius), t->accent);
        ui_text(UI.f_normal, COLLS[i], x + M(0.05f), ry + (rh - M(0.008f) - fh) / 2, sel ? t->accent_text : t->text, ALIGN_LEFT);
    }
    const char *hint[] = { "B", T("common.back"), "X", T("coll.delete"), "A", T("coll.rename") };
    ui_hints(hint, 6);
}

/* ------------------------------------------------- reimposta e cancella tutto */
/* Strumenti → Reimposta impostazioni: impostazioni, liste, collezioni, reti Wi-Fi e browser
   ricordati tornano come nuovi; giochi, BIOS, salvataggi e tempo di gioco restano.
   Strumenti → Cancella tutto: anche ROM, BIOS, salvataggi, immagini, musica, giochi Linux, dati
   dello scraper, app e tempo di gioco, come una scheda appena scritta. In tutti e due i casi restano
   sistema, core, temi, lingue, filtri, cornici, indici arcade, registri, file degli aggiornamenti
   e calibrazione delle levette (dell'hardware); poi si riparte con la prima accensione. */
static atomic_int rst_count;
static bool rst_all;

static const char *const RST_SYSTEM[] = { "settings.json", "favorites.txt", "recent.txt", "hidden.txt", "core-options.txt",
                                          "fuso-orario.txt", "genre-custom.tsv", "genre-groups.tsv", "wifi.conf",
                                          "fileserver-browsers.txt", "resume.txt", "next.txt", "next.env", "backup-visti.txt",
                                          "menu-rotazione", NULL };
static const char *const RST_SYSTEM_ALL[] = { "stats.tsv", "romcheck.tsv", NULL };
/* cartelle svuotate da "Cancella tutto"; in Roms restano le cartelle degli emulatori e LEGGIMI.txt */
static const char *const RST_DIRS[] = { "Bios", "Saves", "Media", "Music", "Linux", "Apps", NULL };

/* toglie il contenuto di dir (e dir stessa se self); falso se qualcosa non si è potuto togliere */
static bool rst_tree(const char *dir, bool self) {
    struct stat ds;
    if (lstat(dir, &ds) != 0) return errno == ENOENT;
    if (!S_ISDIR(ds.st_mode)) return true;   /* collegamento (raccolte sul Mac): non si segue */
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT;
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (lstat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) ok = rst_tree(p, true) && ok;
        else if (unlink(p) != 0) ok = false;
        else atomic_fetch_add(&rst_count, 1);
    }
    closedir(d);
    if (self && rmdir(dir) != 0) ok = false;
    return ok;
}

static bool rst_file(const char *rel) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", UI.root, rel);
    if (unlink(p) == 0) { atomic_fetch_add(&rst_count, 1); return true; }
    return errno == ENOENT;
}

static void *rst_thread(void *arg) {
    (void)arg;
    bool ok = true;
    char p[1200];
    for (int i = 0; RST_SYSTEM[i]; i++) { snprintf(p, sizeof p, "System/%s", RST_SYSTEM[i]); ok = rst_file(p) && ok; }
    snprintf(p, sizeof p, "%s/System/collections", UI.root);
    ok = rst_tree(p, true) && ok;
    if (rst_all) {
        for (int i = 0; RST_SYSTEM_ALL[i]; i++) { snprintf(p, sizeof p, "System/%s", RST_SYSTEM_ALL[i]); ok = rst_file(p) && ok; }
        for (int i = 0; RST_DIRS[i]; i++) {
            snprintf(p, sizeof p, "%s/%s", UI.root, RST_DIRS[i]);
            if (strcmp(RST_DIRS[i], "Apps")) { ok = rst_tree(p, false) && ok; continue; }
            /* Apps: restano le app installate dallo Store (System/pacchetti/<nome>.txt), come gli emulatori */
            DIR *ad = opendir(p);
            struct dirent *ae;
            while (ad && (ae = readdir(ad))) {
                if (!strcmp(ae->d_name, ".") || !strcmp(ae->d_name, "..")) continue;
                char q[1600], rec[1600];
                snprintf(q, sizeof q, "%s/%s", p, ae->d_name);
                snprintf(rec, sizeof rec, "%s/System/pacchetti/%s.txt", UI.root, ae->d_name);
                if (access(rec, F_OK) == 0) continue;
                struct stat sb;
                if (lstat(q, &sb) != 0) continue;
                if (S_ISDIR(sb.st_mode)) ok = rst_tree(q, true) && ok;
                else if (unlink(q) == 0) atomic_fetch_add(&rst_count, 1);
                else ok = false;
            }
            if (ad) closedir(ad);
        }
        /* Roms: il contenuto delle cartelle, non le cartelle né LEGGIMI.txt */
        snprintf(p, sizeof p, "%s/Roms", UI.root);
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char q[1600];
            struct stat sb;
            snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            if (lstat(q, &sb) == 0 && S_ISDIR(sb.st_mode)) ok = rst_tree(q, false) && ok;
        }
        if (d) closedir(d);
        /* Dats: restano solo gli indici arcade (<id>.tsv con accanto <id>.roms) */
        snprintf(p, sizeof p, "%s/Dats", UI.root);
        d = opendir(p);
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char q[1600], r[1600];
            struct stat sb;
            snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            if (lstat(q, &sb) != 0) continue;
            if (S_ISDIR(sb.st_mode)) { ok = rst_tree(q, true) && ok; continue; }
            size_t l = strlen(e->d_name);
            bool arcade = false;
            if (l > 4 && !strcmp(e->d_name + l - 4, ".tsv")) { snprintf(r, sizeof r, "%s/%.*s.roms", p, (int)(l - 4), e->d_name); arcade = access(r, F_OK) == 0; }
            else if (l > 5 && !strcmp(e->d_name + l - 5, ".roms")) arcade = true;
            if (!arcade) { if (unlink(q) == 0) atomic_fetch_add(&rst_count, 1); else ok = false; }
        }
        if (d) closedir(d);
    }
    sync();
    atomic_store(&rst_state, ok ? RST_DONE : RST_FAILED);
    return NULL;
}

static void rst_start(bool all) {
    rst_all = all;
    atomic_store(&rst_count, 0);
    ui_log("%s", all ? "cancella tutto: inizio" : "reimposta impostazioni: inizio");
    UI.set.music_vol = 0;   /* niente brano successivo da Music, che si svuota */
    music_stop();
    if (fsrv_open) fsrv_close();
    quiet_begin();
    atomic_store(&rst_state, RST_RUN);
    pthread_t t;
    if (pthread_create(&t, NULL, rst_thread, NULL) != 0) atomic_store(&rst_state, RST_FAILED);
    else pthread_detach(t);
}

/* alla fine si riparte sempre: le impostazioni in memoria tornano quelle predefinite, così il
   riavvio non riscrive le vecchie, e la prima accensione chiede lingua e Wi-Fi */
static void rst_input(void) {
    int st = atomic_load(&rst_state);
    if (st == RST_RUN) return;
    if (st == RST_FAILED) {
        ui_log("%s: qualche file non si è potuto togliere (%d tolti)", rst_all ? "cancella tutto" : "reimposta impostazioni", atomic_load(&rst_count));
        ui_toast(T("reset_all.partial"));
        atomic_store(&rst_state, RST_DONE);
    }
    static double t0;
    if (!t0) { t0 = UI.t; ui_log("%s: fatto, %d file tolti", rst_all ? "cancella tutto" : "reimposta impostazioni", atomic_load(&rst_count)); }
    if (UI.t - t0 > 1.5) {
        settings_load(&UI.set, UI.root);   /* senza file: i valori predefiniti, prima accensione */
        restart_system();
    }
}

static void rst_draw(void) {
    theme_t *t = &UI.th;
    int st = atomic_load(&rst_state);
    TTF_Font *ft = UI.f_title, *fs = UI.f_small;
    int pw = M(0.8f), ph = M(0.26f), px = (UI.w - pw) / 2, py = (UI.h - ph) / 2, r = M(t->panel_radius), fth = TTF_FontHeight(ft);
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 160));
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(px, py, pw, ph, r, t->surface);
    ui_text(ft, T(st == RST_RUN ? "reset_all.working" : "reset_all.done"), px + pw / 2, py + M(0.07f), t->text, ALIGN_CENTER);
    char line[96];
    snprintf(line, sizeof line, T("reset_all.count"), atomic_load(&rst_count));
    ui_text(fs, line, px + pw / 2, py + M(0.07f) + fth + M(0.02f), t->text_dim, ALIGN_CENTER);
}

/* ------------------------------------------------- conferme */

static void cfm_input(void) {
    bool erase = cfm_kind == CFM_RESETSET || cfm_kind == CFM_RESETSET2 || cfm_kind == CFM_ERASE || cfm_kind == CFM_ERASE2 || cfm_kind == CFM_ERASE3;
    if (erase) UI.in.power = false;   /* qui il tasto di accensione non conferma */
    if (cfm_kind == CFM_ERASE3) {   /* ultima: A tenuto premuto, per non passarla premendo A di fila */
        input_t *in = &UI.in;
        if (in->pressed[BTN_LEFT] && cfm_sel) { cfm_sel = 0; cfm_hold = 0; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_RIGHT] && !cfm_sel) { cfm_sel = 1; cfm_hold = 0; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_B] || (in->pressed[BTN_A] && !cfm_sel)) { cfm_kind = CFM_NONE; cfm_hold = 0; ui_sound(SND_BACK); return; }
        if (cfm_sel && in->pressed[BTN_A]) cfm_hold = UI.t;
        if (!cfm_sel || !in->down[BTN_A]) { cfm_hold = 0; return; }
        if (cfm_hold > 0 && UI.t - cfm_hold >= CFM_HOLD_S) { cfm_kind = CFM_NONE; cfm_hold = 0; ui_sound(SND_OK); rst_start(true); }
        return;
    }
    int r = ui_confirm_input(&cfm_sel);
    if (r < 0) return;
    int k = cfm_kind;
    cfm_kind = CFM_NONE;
    if (r != 1) return;
    if (k == CFM_COLL) {
        if (games_delete_coll(UI.root, cp_sel, games, ngames)) {
            coll_sel = COLL_ALL;
            ui_toast(T("coll.deleted"));
            lists_changed();
        } else ui_toast(T("coll.failed"));
        if (cp_sel >= NCOLLS) cp_sel = NCOLLS ? NCOLLS - 1 : 0;
        if (!NCOLLS) cp_open = false;
    } else if (k == CFM_STICKS) {
        sc_restore_factory();
    } else if (k == CFM_RESTART) {
        restart_system();
    } else if (k == CFM_RESETSET || k == CFM_ERASE || k == CFM_ERASE2) {   /* conferma successiva, di nuovo su Annulla */
        cfm_kind = k == CFM_RESETSET ? CFM_RESETSET2 : k == CFM_ERASE ? CFM_ERASE2 : CFM_ERASE3;
        cfm_sel = 0;
        cfm_hold = 0;
    } else if (k == CFM_STORE) {
        sto_start();
    } else if (k == CFM_RESTPKG) {   /* dopo un ripristino: emulatori e app del backup */
        sto_start();
        sto_batch = true;
    } else if (k == CFM_PKG) {   /* pacchetto copiato sulla scheda */
        char msg[160];
        snprintf(msg, sizeof msg, T("store.working"), pkg_name);
        ui_busy(msg, 0);
        int r = store_offline_install(UI.root, pkg_path);
        ui_toast(T(r == SE_NONE ? "store.done" : r == SE_UNSIGNED ? "store.err_unsigned" : "store.err_verify"));
        reload_games();
        pkg_checked = false;   /* ce ne può essere un altro */
    } else if (k == CFM_RESETSET2) {
        rst_start(false);
    } else if (k == CFM_FORGET) {
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
    if (cfm_kind == CFM_RESTART) ui_confirm_draw(T("dialog.restart_title"), T("dialog.power_body"), T("common.cancel"), T("common.restart"), cfm_sel);
    else if (cfm_kind == CFM_RESETSET) ui_confirm_draw(T("reset_all.settings_title"), T("reset_all.settings_body"), T("common.cancel"), T("reset_all.settings_go"), cfm_sel);
    else if (cfm_kind == CFM_ERASE) ui_confirm_draw(T("reset_all.erase_title"), T("reset_all.erase_body"), T("common.cancel"), T("reset_all.erase_go"), cfm_sel);
    else if (cfm_kind == CFM_STORE) ui_confirm_draw(T("store.ask_title"), T("store.ask_body"), T("store.later"), T("store.open"), cfm_sel);
    else if (cfm_kind == CFM_RESTPKG) {
        char body[200];
        snprintf(body, sizeof body, T("store.restore_body"), restpkg_n);
        ui_confirm_draw(T("store.restore_title"), body, T("store.later"), T("store.restore_go"), cfm_sel);
    }
    else if (cfm_kind == CFM_PKG) {
        char title[160], body[200];
        snprintf(title, sizeof title, T("store.pkg_title"), pkg_name);
        snprintf(body, sizeof body, T("store.pkg_body"), pkg_label);
        ui_confirm_draw(title, body, T("store.later"), T("store.install"), cfm_sel);
    }
    else if (cfm_kind == CFM_RESETSET2) ui_confirm_draw(T("reset_all.sure_title"), T("reset_all.settings2_body"), T("common.cancel"), T("reset_all.settings_go"), cfm_sel);
    else if (cfm_kind == CFM_ERASE2) ui_confirm_draw(T("reset_all.sure_title"), T("reset_all.erase2_body"), T("common.cancel"), T("reset_all.erase_go"), cfm_sel);
    else if (cfm_kind == CFM_ERASE3) {
        double held = cfm_hold > 0 ? UI.t - cfm_hold : 0;
        ui_confirm_draw(T("reset_all.erase3_title"), T("reset_all.erase3_body"), T("common.cancel"), T("reset_all.erase_go"), cfm_sel);
        /* avanzamento della pressione, sopra i pulsanti */
        int w = M(0.75f), h = M(0.36f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, bh = M(0.012f);
        int bx = x + M(0.06f), bw = w - M(0.12f), by = y + h - ui_popup_buttons_h() - bh - M(0.012f);
        ui_rrect(bx, by, bw, bh, bh / 2, with_alpha(UI.th.text_dim, 50));
        if (held > 0) ui_rrect(bx, by, (int)(bw * (held < CFM_HOLD_S ? held / CFM_HOLD_S : 1)), bh, bh / 2, rgba(220, 70, 70, 255));
    }
    else if (cfm_kind == CFM_STICKS) ui_confirm_draw(T("confirm.sticks_title"), T("confirm.sticks_body"), T("common.cancel"), T("confirm.restore"), cfm_sel);
    else if (cfm_kind == CFM_COLL) ui_confirm_draw(T("confirm.coll_title"), T("confirm.coll_body"), T("common.cancel"), T("coll.delete"), cfm_sel);
    else if (cfm_kind == CFM_FORGET) ui_confirm_draw(T("confirm.forget_title"), T("confirm.forget_body"), T("common.cancel"), T("wifi.forget"), cfm_sel);
    else ui_confirm_draw(T("confirm.genres_title"), T("confirm.genres_body"), T("common.cancel"), T("confirm.restore"), cfm_sel);
}

/* ------------------------------------------------- reimposta il gioco */
/* Riporta un gioco com'era la prima volta: impostazioni proprie, DIP switch, punteggi e dati
   interni. La ROM non si tocca; gli stati salvati solo se lo si chiede a parte. */

static int reset_step, reset_sel;   /* 0 chiuso, 1 conferma, 2 anche gli stati? */

static int reset_failed;   /* file che esistono ma non si sono potuti cancellare */

static void reset_remove(const char *dir, const char *rom, const char *ext) {
    char p[1600];
    snprintf(p, sizeof p, "%s/%s.%s", dir, rom, ext);
    if (remove(p) == 0) ui_log("reimposta: tolto %s", p);
    else if (errno != ENOENT) { reset_failed++; ui_log("reimposta: impossibile togliere %s: %s", p, strerror(errno)); }
}

static int detail_slots_for = -2;   /* scheda del gioco: slot già cercati per questo gioco */

static void game_reset(const game_t *g, bool states) {
    detail_slots_for = -2;
    char core[80], base[1200], sub[1300];
    save_base(g, base, sizeof base, core, sizeof core);
    reset_remove(base, g->key, "cfg");
    reset_remove(base, g->key, "opt");
    if (states) {
        /* con gli stati anche il salvataggio a batteria delle console (.srm e .rtc, .flash del
           Neo Geo Pocket fino alla 0.9.17): è la partita */
        static const char *const ext[] = { "state", "2.state", "3.state", "auto.state", "auto.state.prev", "auto.state.rot",
                                           "state.png", "2.state.png", "3.state.png", "auto.state.png", "auto.state.prev.png", "srm", "rtc", "flash" };
        for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) reset_remove(base, g->key, ext[i]);
    }
    /* FBNeo: memoria interna e punteggi */
    snprintf(sub, sizeof sub, "%s/fbneo", base);
    reset_remove(sub, g->key, "fs");
    reset_remove(sub, g->key, "nv");
    reset_remove(sub, g->key, "hi");
    reset_remove(sub, g->key, "diff");
    snprintf(sub, sizeof sub, "%s/Bios/fbneo", UI.root);
    reset_remove(sub, g->key, "hi");
    /* MAME 2003-Plus */
    snprintf(sub, sizeof sub, "%s/mame2003-plus/nvram", base);
    reset_remove(sub, g->key, "nv");
    snprintf(sub, sizeof sub, "%s/mame2003-plus/hi", base);
    reset_remove(sub, g->key, "hi");
    snprintf(sub, sizeof sub, "%s/mame2003-plus/cfg", base);
    reset_remove(sub, g->key, "cfg");
    /* MAME 2010 */
    snprintf(sub, sizeof sub, "%s/mame2010/nvram", base);
    reset_remove(sub, g->key, "nv");
    snprintf(sub, sizeof sub, "%s/mame2010/hi", base);
    reset_remove(sub, g->key, "hi");
    snprintf(sub, sizeof sub, "%s/mame2010/cfg", base);
    reset_remove(sub, g->key, "cfg");
    ui_toast(T(reset_failed ? "reset.partial" : "reset.done"));
    reset_failed = 0;
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
    topbar_theme();
    int mx = M(t->margin), W = UI.w - 2 * mx, y = M(t->topbar_h) + M(0.025f);
    home_title(T("detail.title"), y, t->text);
    y += TTF_FontHeight(UI.f_title) + M(0.03f);
    int bottom = UI.h - ui_keybar_h() - M(0.03f), r = M(t->panel_radius) * 2;
    TTF_Font *ft = UI.f_title, *fn = UI.f_normal, *fs = UI.f_small;
    int fth = TTF_FontHeight(ft), fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    SDL_Texture *shot = media_image(g, t->preview_types, t->npreview);
    if (!shot) shot = media_image(g, t->cover_types, t->ncover);
    /* Le schermate dei giochi sono 4:3 o verticali: in una fascia panoramica, come nella tavola di
       Onda, resterebbero minuscole o tagliate. Onda usa l'impaginazione con l'immagine a sinistra. */
    bool pano = false, arcade = is_list_layout() || is_bar_layout();

    /* immagine: panoramica in alto (Onda) o a sinistra (Tessera larga, Cabinato stretta) */
    int ix = mx, iy = y, iw, ih, tx, ty, tw, tbot;
    if (pano) { iw = W; ih = (bottom - y) * 30 / 100; }
    else { iw = W * (arcade ? 38 : 52) / 100; ih = bottom - y; }
    if (shot) {
        ui_rrect(ix, iy, iw, ih, r / 2, rgba(14, 14, 18, 255));
        ui_clip_push(ix, iy, iw, ih);
        /* immagine intera, con ai lati la stessa sfocata: anche i giochi verticali si riconoscono */
        ui_image_fill(ui_image_blur(shot), ix, iy, iw, ih, r / 2);
        ui_rect(ix, iy, iw, ih, rgba(0, 0, 0, 110));
        ui_image_fit(shot, ix, iy, iw, ih);
        ui_clip_pop();
    } else cover_box(g, ix, iy, iw, ih, r / 2);
    if (pano) {
        tx = mx; ty = iy + ih + M(0.025f); tw = W; tbot = bottom;
    } else {
        int px = mx + iw + M(0.025f), pw = mx + W - px, pad = M(0.022f);
        ui_rrect(px, y, pw, bottom - y, r, t->surface);
        ui_rborder(px, y, pw, bottom - y, r, 2, t->line);
        tx = px + pad; ty = y + pad; tw = pw - 2 * pad; tbot = bottom - pad;
    }

    /* testi: emulatore, titolo intero, giocatori, salvataggio, partite, ROM */
    char buf[300], fit[300], lines[4][160];
    if (!pano) {
        if (is_list_layout()) snprintf(buf, sizeof buf, "%s / %s", !strcmp(g->core, "linux") ? "Linux" : core_console(g->core) ? T("plat.console") : "Arcade", games_core_label(g->core));
        else snprintf(buf, sizeof buf, "%s", games_core_label(g->core));
        if (is_list_layout()) small_caps(buf, tx, ty, t->accent);
        else { ui_text_fit(fs, buf, tw, fit, sizeof fit); ui_text(fs, fit, tx, ty, t->accent, ALIGN_LEFT); }
        ty += fsh + M(0.018f);
    }
    int n = ui_wrap(ft, g->title, tw, lines, pano ? 2 : 4);
    for (int k = 0; k < n; k++, ty += fth) ui_text(ft, lines[k], tx, ty, t->text, ALIGN_LEFT);
    ty += M(0.02f);
    char players[48] = "", genre[48];
    if (g->players > 1) snprintf(players, sizeof players, T("info.players"), g->players);
    genre_label(g->genre, genre, sizeof genre);
    const char *parts[5] = { pano ? games_core_label(g->core) : "", g->year, g->maker, genre, players };
    buf[0] = 0;
    for (int k = 0; k < 5; k++) {
        if (!parts[k][0]) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%s%s", l ? " · " : "", parts[k]);
    }
    n = ui_wrap(fs, buf, tw, lines, 2);
    for (int k = 0; k < n; k++, ty += fsh) ui_text(fs, lines[k], tx, ty, t->text_dim, ALIGN_LEFT);
    ty += M(0.012f);
    time_t when;
    if (game_autosave(detail_idx, &when)) {
        char w[48];
        when_text(when, w, sizeof w);
        snprintf(buf, sizeof buf, "%s · %s", T("lib.autosave"), w);
        ui_text_fit(fs, buf, tw, fit, sizeof fit);
        ui_text(fs, fit, tx, ty, t->accent, ALIGN_LEFT);
        ty += fsh;
    }
    /* stati salvati a mano negli slot del menu in gioco (come li chiama il runner):
       si cercano una volta per gioco, non a ogni fotogramma */
    static char slots[16];
    if (detail_slots_for != detail_idx) {
        detail_slots_for = detail_idx;
        slots[0] = 0;
        if (strcmp(g->core, "linux") && !g->missing) {
            static const char *const sl[] = { "state", "2.state", "3.state" };
            char base[1200], core[80], st[1400];
            struct stat sb;
            save_base(g, base, sizeof base, core, sizeof core);
            for (int k = 0; k < 3; k++) {
                snprintf(st, sizeof st, "%s/%s.%s", base, g->key, sl[k]);
                if (stat(st, &sb) == 0 && sb.st_size > 0) {
                    size_t len = strlen(slots);
                    snprintf(slots + len, sizeof slots - len, "%s%d", len ? ", " : "", k + 1);
                }
            }
        }
    }
    if (slots[0]) {
        snprintf(buf, sizeof buf, T("detail.states"), slots);
        ui_text_fit(fs, buf, tw, fit, sizeof fit);
        ui_text(fs, fit, tx, ty, t->accent, ALIGN_LEFT);
        ty += fsh;
    }
    if (g->missing) {
        const emulator_t *emu = emulator_find(g->core);
        snprintf(buf, sizeof buf, T("missing.toast"), g->name, emu ? emu->roms : "Roms");
    } else if (g->plays) {
        char dur[64], day[64];
        format_duration(g->seconds, dur, sizeof dur);
        time_t lp = (time_t)g->last_played;
        struct tm tmv;
        localtime_r(&lp, &tmv);
        strftime(day, sizeof day, "%d/%m/%Y", &tmv);
        snprintf(buf, sizeof buf, T(g->plays == 1 ? "detail.stats1" : "detail.stats"), g->plays, dur, day);
    } else snprintf(buf, sizeof buf, "%s", T("detail.never"));
    n = ui_wrap(fs, buf, tw, lines, 2);
    for (int k = 0; k < n; k++, ty += fsh) ui_text(fs, lines[k], tx, ty, t->text_dim, ALIGN_LEFT);
    if (g->romstatus != RC_NONE) {
        char key[32], detail[300];
        snprintf(key, sizeof key, "rom.status%d", g->romstatus);
        romcheck_detail(UI.root, g, detail, sizeof detail);
        snprintf(buf, sizeof buf, "%s%s%s", T(key), detail[0] ? ": " : "", detail);
        ui_text_fit(fs, buf, tw, fit, sizeof fit);
        ui_text(fs, fit, tx, ty, rom_problem(g) ? rgba(230, 130, 20, 255) : t->text_dim, ALIGN_LEFT);
        ty += fsh;
    }

    /* storia del gioco nello spazio che resta, scorrevole */
    ty += M(0.018f);
    if (tbot - ty > fsh * 2) {
        ui_rect(tx, ty, tw, 1, t->line);
        ty += M(0.014f);
        wrap_history(g, tw - M(0.01f));
        detail_scroll = ui_approach(detail_scroll, detail_scroll_target);
        ui_clip_push(tx, ty, tw, tbot - ty);
        /* la descrizione viene da ArcadeDB: per le console non c'è da suggerire nulla */
        if (!ndlines && !core_console(g->core)) ui_text(fs, T("detail.no_history"), tx, ty, t->text_dim, ALIGN_LEFT);
        for (int k = 0; k < ndlines; k++) {
            int ly = ty + (int)((k - detail_scroll) * fsh);
            if (ly + fsh < ty || ly > tbot) continue;
            ui_text(fs, dlines[k], tx, ly, t->text, ALIGN_LEFT);
        }
        ui_clip_pop();
    }
    (void)fnh;
    const char *h[] = { "A", T("common.play"), NULL, "B", T("common.back"), NULL, "X", T("coll.hint"), NULL,
                        "Y", T(g->fav ? "common.unfavorite" : "common.favorite"), NULL, "^v", T("detail.scroll"), "" };
    ui_keybar(h, ndlines ? 15 : 12);
}

/* ------------------------------------------------------------ app */

static void apps_launch(const app_t *a) {
    game_t g;
    memset(&g, 0, sizeof g);
    snprintf(g.name, sizeof g.name, "%s", a->name);
    snprintf(g.title, sizeof g.title, "%s", a->title);
    snprintf(g.core, sizeof g.core, "app");
    g.path = (char *)a->path;
    launch(&g);
}

static void apps_input(void) {
    input_t *in = &UI.in;
    int old = app_sel;
    if (in->pressed[BTN_UP] && app_sel > 0) app_sel--;
    if (in->pressed[BTN_DOWN] && app_sel < napps - 1) app_sel++;
    if (app_sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_A] && napps) { apps_launch(&apps[app_sel]); return; }
    if (in->pressed[BTN_B]) { go(SCR_HOME); ui_sound(SND_BACK); return; }
    if (in->pressed[BTN_START]) { settings_open(-1, 0); go(SCR_SETTINGS); ui_sound(SND_OK); }
}

static void apps_draw(void) {
    theme_t *t = &UI.th;
    topbar_theme();
    int mx = M(t->margin), W = UI.w - 2 * mx, y = M(t->topbar_h) + M(0.025f), bottom = UI.h - ui_keybar_h() - M(0.03f);
    home_title(T("apps.title"), y, t->text);
    y += TTF_FontHeight(UI.f_title) + M(0.03f);
    if (!napps) {
        empty_panel(mx, y, W, bottom - y, T("apps.empty_title"), T("apps.empty_text"), NULL, NULL);
    } else {
        TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
        int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), pad = M(0.016f), gap = M(0.014f);
        int rh = fnh + fsh + 2 * pad + M(0.01f), is = rh - 2 * pad, r = M(t->tile_radius);
        int vis = (bottom - y + gap) / (rh + gap);
        if (vis < 1) vis = 1;
        float target = app_sel < vis ? 0 : (float)(app_sel - vis + 1);
        app_scroll = ui_approach(app_scroll, target);
        ui_clip_push(mx - M(0.01f), y - M(0.01f), W + M(0.02f), bottom - y + M(0.02f));
        for (int i = 0; i < napps; i++) {
            int ry = y + (int)((i - app_scroll) * (rh + gap));
            if (ry + rh < y || ry > bottom) continue;
            const app_t *a = &apps[i];
            ui_rrect(mx, ry, W, rh, r, t->surface);
            if (i == app_sel) selection_ring(mx, ry, W, rh, r);
            int ix = mx + pad, iy = ry + pad;
            SDL_Texture *ic = a->icon[0] ? ui_image(a->icon) : NULL;
            if (ic) ui_image_fill(ic, ix, iy, is, is, r / 2);
            else {
                ui_rrect(ix, iy, is, is, r / 2, t->tile);
                draw_icon_glyph(IC_APPS, ix + is / 2, iy + is / 2, is * 2 / 3, t->accent, t->tile);
            }
            int tx = ix + is + M(0.022f), tw = mx + W - pad - tx;
            char fit[300], sub[400];
            ui_text_fit(fn, a->title, tw, fit, sizeof fit);
            ui_text(fn, fit, tx, ry + pad, t->text, ALIGN_LEFT);
            snprintf(sub, sizeof sub, "%s%s%s", a->author, a->author[0] && a->desc[0] ? " · " : "", a->desc);
            ui_text_fit(fs, sub, tw, fit, sizeof fit);
            ui_text(fs, fit, tx, ry + pad + fnh + M(0.006f), t->text_dim, ALIGN_LEFT);
        }
        ui_clip_pop();
    }
    const char *h[] = { "A", T("common.open"), NULL, "B", T("common.back"), NULL };
    ui_keybar(h, napps ? 6 : 3);
}

/* ------------------------------------------------------ verifica ROM */

/* prima di cominciare: rapida (l'indice degli zip) o completa (decomprime tutto, più lenta) */
static void rcq_input(void) {
    int r = ui_popup_input(&rcq_sel, 2);
    if (r == -1) return;
    rcq_open = false;
    if (r < 0) { ui_sound(SND_BACK); return; }
    ui_log("verifica ROM %s", r ? "completa" : "rapida");
    if (romcheck_start(UI.root, games, ngames, r == 1)) { romcheck_open = true; ui_sound(SND_OK); }
}

static void rcq_draw(void) { ui_confirm_draw(T("rom.choose_title"), T("rom.choose_body"), T("rom.quick"), T("rom.full"), rcq_sel); }

static void romcheck_input(void) {
    last_input = UI.t;   /* niente spegnimento automatico mentre lavora */
    const romcheck_status_t *s = romcheck_status();
    bool press = UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B];
    if (!s->finished && press) { romcheck_cancel(); ui_sound(SND_BACK); }
    if (s->finished && press) {
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
    int h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.03f) + TTF_FontHeight(UI.f_normal) * 2 + M(0.06f) + TTF_FontHeight(UI.f_small) * 3 + ui_popup_buttons_h() - pad, x, y;
    panel_box(w, h, &x, &y);
    int ty = y + pad;
    ui_text(UI.f_title, T(s->full ? "rom.title_full" : "rom.title"), x + pad, ty, t->text, ALIGN_LEFT);
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
    if (s->full) {   /* i rovinati in testa: è il motivo della verifica completa */
        size_t l = strlen(line);
        snprintf(line + l, sizeof line - l, T("rom.summary3"), s->count[RC_DAMAGED]);
    }
    ui_text(UI.f_small, line, x + pad, ty, t->text_dim, ALIGN_LEFT);
    const char *label[1] = { T(s->finished ? "common.ok" : "scrape.stop") };
    ui_popup_buttons(x, y, w, h, label, 1, 0, -1);
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
    bool coll = kb_mode == KBM_COLL || kb_mode == KBM_COLL_RENAME;
    if (cap) *cap = coll ? sizeof coll_name : sizeof search;
    return coll ? coll_name : search;
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

static void cp_rename_done(void);

/* fine scrittura: per la ricerca chiude anche i filtri; per la collezione la crea */
static void wifi_join(const char *ssid, const char *psk);

static void genre_kb_done(void);

static void kb_done(void) {
    if (kb_mode == KBM_COLL_RENAME) { kb_open = false; cp_rename_done(); return; }
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
    /* un pulsante solo nella finestra: "Interrompi" mentre lavora, "OK" alla fine */
    bool press = UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B];
    if (!finished && press && !s->cancel) { scraper_cancel(); ui_sound(SND_BACK); }
    if (finished && press) {
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
    int pad = M(0.05f), bh = M(0.02f);
    int w = M(0.95f), h = pad + TTF_FontHeight(UI.f_title) + M(0.03f) + TTF_FontHeight(UI.f_normal) + M(0.03f) + bh + M(0.03f)
                        + TTF_FontHeight(UI.f_small) + ui_popup_buttons_h();
    int x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    if (t->surface.a < 200) ui_rrect(x, y, w, h, r, with_alpha(t->bg2, 230));   /* temi con superfici trasparenti */
    int ty = y + pad;
    ui_text(UI.f_title, T("scrape.title"), x + pad, ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_title) + M(0.03f);
    char line[200];
    const char *phase = s->phase == SCRAPE_INFO ? T("scrape.info") : s->phase == SCRAPE_IMAGES ? T("scrape.images")
                      : s->phase == SCRAPE_DONE ? T(s->cancel ? "scrape.stopped" : "scrape.done") : T("scrape.failed");
    ui_text(UI.f_normal, phase, x + pad, ty, t->text_dim, ALIGN_LEFT);
    ty += TTF_FontHeight(UI.f_normal) + M(0.03f);
    /* barra di avanzamento */
    int bw = w - 2 * pad;
    float frac = s->total ? (float)s->done / s->total : (s->phase == SCRAPE_DONE ? 1.f : 0.f);
    ui_rrect(x + pad, ty, bw, bh, bh / 2, with_alpha(t->text_dim, 70));
    if (frac > 0) ui_rrect(x + pad, ty, (int)(bw * frac), bh, bh / 2, t->accent);
    ty += bh + M(0.03f);
    snprintf(line, sizeof line, T("scrape.progress"), s->done, s->total, s->saved, s->missing);
    ui_text(UI.f_small, line, x + pad, ty, t->text_dim, ALIGN_LEFT);
    bool finished = s->phase == SCRAPE_DONE || s->phase == SCRAPE_FAILED;
    const char *label[1] = { T(finished ? "common.ok" : "scrape.stop") };
    ui_popup_buttons(x, y, w, h, label, 1, 0, -1);
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
        if (!charging && pct >= 0 && pct <= 15 && !warned_low) {
            char m[96];
            snprintf(m, sizeof m, T("battery.low"), pct);
            ui_toast(m);
            warned_low = true;
        }
    }
    if (!charging && pct >= 0 && pct <= 5 && !saver_on) {
        char m[128];
        snprintf(m, sizeof m, T("battery.critical"), pct);
        int w = ui_text_w(UI.f_small, m) + M(0.04f), h = TTF_FontHeight(UI.f_small) + M(0.015f);
        int x = (UI.w - w) / 2, y = UI.h - M(UI.th.hintbar_h) - h - M(0.015f);
        ui_rrect(x, y, w, h, h / 2, rgba(200, 40, 40, 235));
        ui_text(UI.f_small, m, UI.w / 2, y + M(0.0075f), rgba(255, 255, 255, 255), ALIGN_CENTER);
    }
}

/* ----------------------------------------------------------- main */

/* Ripresa della partita all'accensione: resume.txt diventa next.txt. Quello scritto da un
   runner della 0.9.17 non ha la console (sesto campo): si ricava dalla cartella della ROM, e i
   salvataggi di quel gioco si portano subito in Saves/<console> (audit prelancio R02). */
static bool consume_resume(void) {
    char a[1100], b[1100], line[2400];
    snprintf(a, sizeof a, "%s/System/resume.txt", UI.root);
    snprintf(b, sizeof b, "%s/System/next.txt", UI.root);
    struct stat st;
    if (!UI.set.resume_last || stat(a, &st) != 0) return false;
    FILE *f = fopen(a, "r");
    bool legacy = false;
    char *fld[6] = { 0 };
    int nf = 0;
    if (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        for (char *p = line; p && nf < 6; nf++) { fld[nf] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        legacy = nf == 5 && !strcmp(fld[0], "libretro");
    }
    if (f) fclose(f);
    if (legacy) {
        emulators_load(UI.root);
        const emulator_t *emu = NULL;
        /* percorsi reali: la cartella del launcher può essere relativa, la ROM assoluta */
        char rom_real[PATH_MAX];
        if (!realpath(fld[2], rom_real)) snprintf(rom_real, sizeof rom_real, "%s", fld[2]);
        for (int e = 0; e < NEMUS && !emu; e++) {
            char dir[1100], dir_real[PATH_MAX];
            if (EMUS[e].roms[0] == '/') snprintf(dir, sizeof dir, "%s", EMUS[e].roms);
            else snprintf(dir, sizeof dir, "%s/%s", UI.root, EMUS[e].roms);
            if (!realpath(dir, dir_real)) continue;
            size_t l = strlen(dir_real);
            if (!strncmp(rom_real, dir_real, l) && rom_real[l] == '/') emu = &EMUS[e];
        }
        if (emu && emu->console) {
            game_t g;
            memset(&g, 0, sizeof g);
            snprintf(g.core, sizeof g.core, "%s", emu->id);
            const char *base = strrchr(fld[2], '/');
            snprintf(g.name, sizeof g.name, "%s", base ? base + 1 : fld[2]);
            char *dot = strrchr(g.name, '.');
            if (dot) *dot = 0;
            saves_split(UI.root, &g, 1);
            char tmp[1200];
            snprintf(tmp, sizeof tmp, "%s.tmp", b);
            FILE *o = fopen(tmp, "w");
            bool ok = o && fprintf(o, "%s\t%s\t%s\t%s\t%s\t%s\n", fld[0], fld[1], fld[2], fld[3], fld[4], emu->id) > 0;
            if (o && fclose(o) != 0) ok = false;
            if (ok && rename(tmp, b) == 0) { unlink(a); ui_log("ripresa: console %s ricavata dalla cartella della ROM", emu->id); return true; }
            unlink(tmp);
        }
    }
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
#ifndef TRIMUI_DEVICE
    if (!UI.set.wifi) wifi_enable(false);   /* Mac: il Wi-Fi simulato segue l'impostazione (sulla console lo fa system-init) */
#endif
    ui_busy(T("common.loading"), 0);   /* subito qualcosa a schermo mentre si caricano i giochi */
    if (!auto_script && consume_resume()) {
        ui_log("riprendo l'ultima partita");
        /* il sistema è arrivato fin qui: vale come prima schermata per l'aggiornamento */
        update_confirm_boot(UI.root);
        plat_audio_release();
        ui_shutdown_code(EXIT_LAUNCH);
        return EXIT_LAUNCH;
    }
    ui_mark("schermata di caricamento");
    nthemes = theme_list(UI.root, theme_ids, theme_names, 16);
    nlangs = lang_list(UI.root, lang_codes, lang_names, 16);
    nfilters = filters_list(UI.root, UI.set.lang, filters, 24);
    store_recover(UI.root);   /* un'installazione dello Store interrotta: prima di leggere i giochi */
    reload_games();
    ui_mark("giochi pronti");
    music_init(UI.root);
    plat_amp_config(UI.set.speaker_rest);   /* fruscio della Brick Pro (issue #4) */
    ui_mark("musica");
    screen = SCR_HOME;
#ifndef TRIMUI_DEVICE
    if (getenv("TRIMUI_START_LIBRARY")) screen = SCR_LIBRARY;   /* prove sul Mac: si parte dalla libreria */
#endif
    screen_t0 = -1;
    diag_check_failure();
    if (!UI.set.setup_done) setup_step = 1;
    bool first_frame = true, idle_logged = false;
    unsigned idle_skip = 0;

    while (!UI.in.quit) {
        ui_poll();
        music_update();
        plat_amp_idle(music_title()[0] || audio_quiet);   /* amplificatore spento dopo 4 s senza suoni */
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
        if (UI.in.power && !upd_busy() && atomic_load(&imp_state) != IMP_RUN && !atomic_load(&rst_state) && !(dlg_open && dlg_suspend)) {
            dlg_open = dlg_suspend = true;
            dlg_sel = 1;
            saver_on = false;
            last_input = UI.t;
            UI.in.power = false;   /* questa pressione ha solo aperto la finestra */
            ui_sound(SND_OK);
        }
        if (UI.in.power_long && (!dlg_open || dlg_suspend) && !upd_open && atomic_load(&imp_state) != IMP_RUN && !atomic_load(&rst_state)) { dlg_open = true; dlg_suspend = false; dlg_sel = dlg_buttons() - 1; ui_sound(SND_OK); }
        /* HOME (tasto laterale della Brick Pro): chiude finestre e torna alla schermata iniziale */
        if (UI.in.home && !romcheck_open && !scrape_open && !upd_open && !setup_step && !ptest_open && !sc_state && !atomic_load(&imp_state) && !atomic_load(&rst_state) &&
            !(screen == SCR_STORE && store_status()->phase == ST_WORKING)) {
            dlg_suspend = false;
            reset_step = 0;
            cfm_kind = CFM_NONE;
            cp_open = false;
            diag_open = kb_open = fpanel_open = dlg_open = gopt_open = clock_open = sdinfo_open = support_open = wpanel_open = clean_open = gp_open = rcq_open = false;
            if (screen == SCR_STORE) { store_join(); if (store_changed()) reload_games(); sto_detail = false; sto_ask = SA_NONE; }
            if (fsrv_open) fsrv_close();
            go(SCR_HOME);
            ui_sound(SND_BACK);
        }
        /* il layout può cambiare col tema: la home a carosello non esiste nel layout a elenco */

        /* controllo degli aggiornamenti: una volta per avvio, appena il Wi-Fi è collegato */
        static bool notify_started;
        if (!notify_started && UI.set.update_notify && update_supported() && !upd_open && wifi_status(false)->state == WIFI_CONNECTED) {
            notify_started = true;
            update_notify_start(UI.root, UI.set.update_channel);
        }
        /* SD bloccata dopo un risveglio: prima di tutto il resto */
        static double sdbad_t;
        if (!sdbad_open && plat_standalone() && UI.t - sdbad_t > 1.0) {
            sdbad_t = UI.t;
            sdbad_open = access("/run/sd-guasta", F_OK) == 0 || getenv("ARCADE_TEST_SDBAD");
        }
        if (sdbad_open) sdbad_input();
        else if (dlg_open && dlg_suspend) {
            int r = ui_confirm_input(&dlg_sel);
            if (r >= 0) dlg_open = dlg_suspend = false;
            if (r == 1) { ui_suspend(); last_input = UI.t; }
        }
        else if (rdy_open) rdy_input();
        else if (atomic_load(&rst_state)) rst_input();
        else if (atomic_load(&imp_state)) imp_input();
        else if (cfm_kind) cfm_input();
        else if (setup_step) setup_input();
        else if (sc_state) sc_input();
        else if (ptest_open) ptest_input();
        else if (reset_step) reset_input();
        else if (diag_open) diag_input();
        else if (rcq_open) rcq_input();
        else if (romcheck_open) romcheck_input();
        else if (scrape_open) scrape_input();
        else if (upd_open) upd_input();
        else if (kb_open) keyboard_input();
        else if (gp_open) gp_input();
        else if (cp_open) cp_input();
        else if (gopt_open && screen == SCR_DETAIL) gopt_input();
        else if (fpanel_open) fpanel_input();
        else if (dlg_open) dialog_input();
        else if (clock_open) clock_input();
        else if (wpanel_open) wpanel_input();
        else if (clean_open) clean_input();
        else if (sdinfo_open) sdinfo_input();
        else if (support_open) support_input();
        else if (fsrv_open) fsrv_input();
        else if (screen == SCR_HOME) home_input();
        else if (screen == SCR_LIBRARY) library_input();
        else if (screen == SCR_DETAIL) detail_input();
        else if (screen == SCR_PLATFORMS) plat_input();
        else if (screen == SCR_APPS) apps_input();
        else if (screen == SCR_STORE) sto_input();
        else settings_input();

        /* Fermo sullo schermo da qualche secondo: CPU bassa e meno disegni, la console scalda
           meno. Basta un tasto per tornare subito come prima; i lavori lunghi non rallentano. */
        bool held = false;
        for (int b = 0; b < BTN_COUNT; b++) held = held || UI.in.down[b];
        if (held) last_input = UI.t;
        upd_audio();
        /* l'uscita audio è aperta e assestata: ora l'amplificatore si può accendere */
        if (UI.t > 0.4) plat_audio_release();
        bool working = romcheck_open || scrape_open || upd_open || clean_open || wpanel_open || kb_open || sdbad_open || fsrv_open || screen == SCR_STORE;
        /* nei primi 30 s il Wi-Fi carica il driver e si collega: niente cambi di frequenza della CPU */
        bool idle = !working && !UI.automation && UI.t > 30 && UI.t - last_input > IDLE_AFTER_S;
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
        if (screen == SCR_HOME) home_draw();
        else if (screen == SCR_LIBRARY) {
            library_draw();
        } else if (screen == SCR_DETAIL) detail_draw();
        else if (screen == SCR_PLATFORMS) plat_draw();
        else if (screen == SCR_APPS) apps_draw();
        else if (screen == SCR_STORE) sto_draw();
        else settings_draw();
        if (dlg_open && !dlg_suspend) dialog_draw();
        if (clock_open) clock_draw();
        if (wpanel_open) wpanel_draw();
        if (clean_open) clean_draw();
        if (sdinfo_open) sdinfo_draw();
        if (support_open) support_draw();
        if (fsrv_open) fsrv_draw();
        if (fpanel_open) fpanel_draw();
        if (gopt_open && screen == SCR_DETAIL) gopt_draw();
        if (gp_open) gp_draw();
        if (cp_open) cp_draw();
        if (kb_open) keyboard_draw();
        if (scrape_open) scrape_draw();
        if (upd_open) upd_draw();
        if (romcheck_open) romcheck_draw();
        if (rcq_open) rcq_draw();
        if (diag_open) diag_draw();
        if (reset_step) reset_draw();
        if (sc_state) sc_draw();
        if (cfm_kind) cfm_draw();
        if (ptest_open) ptest_draw();
        if (setup_step && !atomic_load(&imp_state)) setup_draw();
        if (atomic_load(&imp_state)) imp_draw();
        if (atomic_load(&rst_state)) rst_draw();
        if (rdy_open) rdy_draw();
        if (sdbad_open) sdbad_draw();
        if (dlg_open && dlg_suspend) ui_confirm_draw(T("dialog.suspend_title"), T("dialog.suspend_body"), T("common.cancel"), T("dialog.suspend"), dlg_sel);
        battery_check();
        if (first_frame) {
            first_frame = false;
            ui_mark("prima schermata");
            update_confirm_boot(UI.root);
            if (update_boot_failed()) ui_toast(T("update.boot_failed"));
            else if (update_supported()) update_resume_start(UI.root);   /* scaricato e non installato? */
        }
        /* la domanda arriva quando il controllo in sottofondo è finito, solo se nulla è aperto */
        /* un backup copiato sulla scheda: dopo la lingua, prima del Wi-Fi della prima accensione */
        if (!imp_checked && setup_step != 1 && !upd_open && !dlg_open && !cfm_kind && !rdy_open && !sdbad_open) {
            imp_checked = true;
            if (imp_find()) { atomic_store(&imp_state, IMP_ASK); pp_reset(&imp_pp, &imp_bk); }
        }
        /* poi i pacchetti dello Store copiati sulla scheda, uno alla volta */
        if (imp_checked && !atomic_load(&imp_state) && !pkg_checked && !setup_step && !upd_open && !dlg_open && !cfm_kind && !rdy_open && !sdbad_open) {
            pkg_checked = true;
            if (store_offline_find(UI.root, pkg_path, sizeof pkg_path, pkg_name, sizeof pkg_name, pkg_label, sizeof pkg_label)) {
                ui_log("store: pacchetto sulla scheda: %s (%s)", pkg_name, pkg_path);
                cfm_kind = CFM_PKG;
                cfm_sel = 1;
            }
        }
        /* dopo un ripristino: una volta per avvio, a Wi-Fi collegato, rimettere i pacchetti del backup
           (prima dell'invito della scheda senza emulatori, che allora non serve) */
        if (pkg_checked && !restpkg_asked && !atomic_load(&imp_state) && !setup_step && !upd_open && !dlg_open && !cfm_kind &&
            !rdy_open && !sdbad_open && screen != SCR_STORE && !fsrv_open && !wpanel_open && !kb_open && sto_online()) {
            restpkg_asked = true;
            if ((restpkg_n = store_restore_pending(UI.root)) > 0) {
                cfm_kind = CFM_RESTPKG;
                cfm_sel = 1;
                store_asked = true;
            }
        }
        /* nessun emulatore (scheda nuova): una volta per avvio, l'invito ad aprire lo Store, solo
           a Wi-Fi collegato (senza rete lo Store non ha il catalogo) e non mentre si sceglie la rete */
        if (pkg_checked && !store_asked && NEMUS == 0 && !atomic_load(&imp_state) && !setup_step && !upd_open && !dlg_open && !cfm_kind &&
            !rdy_open && !sdbad_open && screen != SCR_STORE && !fsrv_open && !wpanel_open && !kb_open && sto_online()) {
            store_asked = true;
            cfm_kind = CFM_STORE;
            cfm_sel = 1;
        }
        if (!rdy_asked && update_resume_ready() && !upd_open && !dlg_open && !cfm_kind && !setup_step && !atomic_load(&imp_state)) {
            rdy_asked = rdy_open = true;
            rdy_sel = 2;
        }
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
