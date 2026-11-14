/* gamemenu: menu sopra un gioco Linux, con la grafica del menu degli emulatori.

   Lo avvia gamewatch con il gioco fermo (SIGSTOP). Il gioco ha lo schermo e la GPU, quindi
   il menu si disegna fuori schermo (ui_init_offscreen: renderer software, tema e caratteri
   del launcher) e si mostra in un livello del display sopra al gioco (driver Allwinner
   disp2): l'immagine ARGB sta nella parte finale della memoria di fb0, che con la GPU attiva
   non è mostrata. Lo schermo della Smart Pro è montato in verticale: il menu si ruota
   (System/menu-rotazione: 90 o 270, predefinito 270).

   Uscita: 0 riprendi, 10 esci dal gioco, 20 spegni, 3 menu non disponibile.
   Sul Mac: gamemenu --root sdcard --shot file.png disegna una schermata di prova. */
#include "../common/ui.h"
#include "../common/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GM_RESUME, GM_VOLUME, GM_BRIGHT, GM_QUIT, GM_COUNT };
enum { RC_RESUME = 0, RC_UNAVAILABLE = 3, RC_QUIT = 10, RC_POWER = 20, RC_SLEEP = 30 };
static bool asking;       /* finestra "sospendere?" aperta */
static int ask_sel = 1;

static int sel;
/* geometria del pannello e delle righe (in pixel dell'immagine del menu) */
static int g_px, g_py, g_pw, g_ph, g_rows_y, g_rh;

static void row_text(int i, char *label, size_t ln, char *value, size_t vn) {
    static const char *keys[GM_COUNT] = { "game.back_to_game", "settings.volume", "settings.brightness", "game.exit" };
    snprintf(label, ln, "%s", T(keys[i]));
    value[0] = 0;
    if (i == GM_VOLUME) snprintf(value, vn, "%d", UI.set.volume);
    if (i == GM_BRIGHT) snprintf(value, vn, "%d", UI.set.brightness);
}

/* fondo del menu: velo scuro, ombra e pannello. Si usa anche per ripulire la zona delle voci:
   con una superficie semitrasparente (tema Copertina) coprire le righe con il suo colore lasciava
   vedere l'evidenziazione di prima, e cambiando voce si accendevano tutte */
static void draw_base(int px, int py, int pw, int ph) {
    theme_t *t = &UI.th;
    int r = M(t->panel_radius) * 2;
    /* tutto lo schermo coperto, senza trasparenza: i pulsanti del gioco visti sotto il velo si
       confondevano con le voci del menu (Smart Pro, 14 novembre) */
    ui_rect(0, 0, UI.w, UI.h, with_alpha(t->bg, 255));
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 110));
    /* fondo pieno: sotto il gioco Linux continua a muoversi e con una superficie semitrasparente
       (tema Copertina) il menu si leggeva male, come per le finestre delle app (app_panel) */
    ui_rrect(px, py, pw, ph, r, with_alpha(t->bg, 255));
    ui_rrect(px, py, pw, ph, r, t->surface);
    /* finestra con un bordo sottile, piena (14 novembre: la forma di finestra, senza trasparenze) */
    ui_rborder(px, py, pw, ph, r, M(0.003f) < 1 ? 1 : M(0.003f), t->line);
}

/* parti fisse, come menu_draw del runner (pagina principale): si disegnano una volta */
static void draw_static(const char *title) {
    theme_t *t = &UI.th;
    int hb = M(t->hintbar_h), my = M(0.035f);
    int pw = M(0.80f);
    if (pw > UI.w - 2 * M(t->margin)) pw = UI.w - 2 * M(t->margin);
    int ph = UI.h - hb - 2 * my;
    int px = M(t->margin), py = my;
    draw_base(px, py, pw, ph);
    char buf[300];
    int pad = M(0.04f), y = py + pad;
    ui_text_fit(UI.f_title, title, pw - 2 * pad, buf, sizeof buf);
    ui_text(UI.f_title, buf, px + pad, y, t->text, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_title);
    ui_text(UI.f_small, T("game.linux_game"), px + pad, y, t->accent, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_small) + M(0.02f);
    int note_h = TTF_FontHeight(UI.f_small) + M(0.02f);
    ui_text_fit(UI.f_small, T("game.linux_note"), pw - 2 * pad, buf, sizeof buf);
    ui_text(UI.f_small, buf, px + pad, py + ph - pad / 2 - note_h + M(0.01f), t->text_dim, ALIGN_LEFT);
    ui_rect(0, UI.h - hb, UI.w, hb, with_alpha(t->bg, 255));
    const char *h[] = { "B", T("game.back_to_game"), "<>", T("common.change"), "A", T("common.ok") };
    ui_hints(h, 6);
    g_px = px; g_py = py; g_pw = pw; g_ph = ph; g_rows_y = y; g_rh = M(t->list_row_h);
}

/* righe delle voci: a ogni tasto si ridisegna solo questa zona */
static void draw_rows(void) {
    theme_t *t = &UI.th;
    char buf[300], label[200], value[120];
    /* la zona delle righe torna com'era senza righe: svuotata e ridisegnata con il fondo */
    SDL_Rect z = { g_px, g_rows_y, g_pw, GM_COUNT * g_rh };
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, 0);
    SDL_RenderFillRect(UI.ren, &z);
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
    ui_clip_push(z.x, z.y, z.w, z.h);
    draw_base(g_px, g_py, g_pw, g_ph);
    ui_clip_pop();
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    ui_rrect(g_px + M(0.02f), g_rows_y + sel * g_rh, g_pw - M(0.04f), g_rh, M(t->panel_radius), t->accent);
    for (int i = 0; i < GM_COUNT; i++) {
        int ry = g_rows_y + i * g_rh;
        bool is_sel = i == sel;
        row_text(i, label, sizeof label, value, sizeof value);
        color_t c = is_sel ? t->accent_text : t->text;
        int ty = ry + (g_rh - fh) / 2, right = g_px + g_pw - M(0.05f), vw = 0;
        if (value[0]) {
            snprintf(buf, sizeof buf, is_sel ? "‹ %s ›" : "%s", value);
            vw = ui_text(f, buf, right, ty, is_sel ? c : t->text_dim, ALIGN_RIGHT) + M(0.03f);
        }
        ui_text_fit(f, label, right - vw - (g_px + M(0.05f)), buf, sizeof buf);
        ui_text(f, buf, g_px + M(0.05f), ty, c, ALIGN_LEFT);
    }
}

/* restituisce RC_* oppure -1 se il menu resta aperto */
static int input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_UP]) { sel = (sel + GM_COUNT - 1) % GM_COUNT; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { sel = (sel + 1) % GM_COUNT; ui_sound(SND_MOVE); }
    int d = in->pressed[BTN_RIGHT] ? 1 : in->pressed[BTN_LEFT] ? -1 : 0;
    if (d && sel == GM_VOLUME) { UI.set.volume = UI.set.volume + d < 0 ? 0 : UI.set.volume + d > 20 ? 20 : UI.set.volume + d; settings_save(&UI.set, UI.root); }
    if (d && sel == GM_BRIGHT) { UI.set.brightness = UI.set.brightness + d < 1 ? 1 : UI.set.brightness + d > 10 ? 10 : UI.set.brightness + d; settings_save(&UI.set, UI.root); }
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU] || UI.in.home) return RC_RESUME;
    if (in->pressed[BTN_A]) {
        if (sel == GM_RESUME) return RC_RESUME;
        if (sel == GM_QUIT) return RC_QUIT;
    }
    if (UI.in.power) { asking = true; ask_sel = 1; return -1; }
    if (UI.in.power_long) return RC_POWER;
    return -1;
}

static void draw_ask(void) {
    ui_confirm_draw(T("dialog.suspend_title"), T("dialog.suspend_body"), T("common.cancel"), T("dialog.suspend"), ask_sel);
}

#ifdef TRIMUI_DEVICE
/* ------------------------------------------------------------ livello del display */
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* include/video/sunxi_display2.h del kernel 4.9 (Allwinner disp2) */
struct disp_rect { int x, y; unsigned int width, height; };
struct disp_rectsz { unsigned int width, height; };
struct disp_rect64 { long long x, y, width, height; };
struct disp_fb_info {
    unsigned long long addr[3];
    struct disp_rectsz size[3];
    unsigned int align[3];
    int format;              /* 0 = ARGB 8888 */
    int color_space;
    unsigned int trd_right_addr[3];
    bool pre_multiply;
    struct disp_rect64 crop; /* virgola fissa 32.32 */
    int flags;
    int scan;
};
struct disp_layer_info {
    int mode;                /* 0 = immagine */
    unsigned char zorder, alpha_mode, alpha_value;
    struct disp_rect screen_win;
    bool b_trd_out;
    int out_trd_mode;
    union { unsigned int color; struct disp_fb_info fb; };
    unsigned int id;
};
struct disp_layer_config { struct disp_layer_info info; bool enable; unsigned int channel, layer_id; };
#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

static int fbfd = -1, dispfd = -1;
static uint8_t *fbmem;
static size_t fblen;
static uint32_t *ov;
static int ov_w, ov_h, ov_rot;
static struct disp_layer_config mine;

static int disp_layer(int cmd, struct disp_layer_config *c) {
    unsigned long arg[4] = { 0, (unsigned long)c, 1, 0 };
    return ioctl(dispfd, cmd, arg);
}

static bool layer_open(void) {
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo fx;
    fbfd = open("/dev/fb0", O_RDWR);
    dispfd = open("/dev/disp", O_RDWR);
    if (fbfd < 0 || dispfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fx)) {
        ui_log("menu: fb0 o disp non disponibili");
        return false;
    }
    /* livello del gioco: canale 1, livello 0; se i valori non sono plausibili non si tocca nulla */
    struct disp_layer_config game;
    memset(&game, 0, sizeof game);
    game.channel = 1;
    int r = disp_layer(DISP_LAYER_GET_CONFIG, &game);
    ui_log("menu: livello del gioco (esito %d): acceso %d, z %d, %ux%u, indirizzo 0x%llx", r, game.enable, game.info.zorder,
           game.info.screen_win.width, game.info.screen_win.height, game.info.fb.addr[0]);
    if (r < 0 || !game.enable || game.info.mode != 0 || !game.info.fb.addr[0] || game.info.screen_win.width < 320 ||
        game.info.screen_win.height < 240 || game.info.screen_win.width > 4096 || game.info.screen_win.height > 4096)
        return false;
    ov_w = (int)game.info.screen_win.width;
    ov_h = (int)game.info.screen_win.height;
    size_t need = (size_t)ov_w * ov_h * 4;
    if (need + 4096 > fx.smem_len) return false;
    /* Memoria per il menu: in fondo a fb0, se il gioco mostra le prime righe (runner); se le sue
       immagini stanno più avanti (Deadcast le tiene dalla riga 12288 in poi, fino quasi in fondo)
       si usa l'inizio, che resta libero. Prima il menu finiva su righe del gioco e non si vedeva. */
    long long gy = game.info.fb.crop.y >> 32, line = fx.line_length ? fx.line_length : (long long)v.xres_virtual * 4;
    size_t game_off = (size_t)(game.info.fb.addr[0] > fx.smem_start ? game.info.fb.addr[0] - fx.smem_start : 0) + (size_t)(gy * line);
    size_t off = (fx.smem_len - need) & ~(size_t)4095;
    if (game_off >= need) off = 0;
    ui_log("menu: fb0 %u byte, %ux%u virtuali, riga %u; gioco da %zu (riga %lld), menu da %zu", fx.smem_len, v.xres_virtual,
           v.yres_virtual, fx.line_length, game_off, gy, off);
    fblen = fx.smem_len;
    fbmem = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) { fbmem = NULL; return false; }
    ov = (uint32_t *)(fbmem + off);
    memset(ov, 0, need);
    /* schermo verticale e menu orizzontale: si ruota */
    ov_rot = 0;
    if ((ov_h > ov_w) != (UI.h > UI.w)) {
        ov_rot = 270;   /* provato sulla Smart Pro (con 90 il menu era capovolto) */
        char p[1100];
        snprintf(p, sizeof p, "%s/System/menu-rotazione", UI.root);
        FILE *f = fopen(p, "r");
        if (f) { int v2 = 0; if (fscanf(f, "%d", &v2) == 1 && (v2 == 90 || v2 == 270)) ov_rot = v2; fclose(f); }
    }
    mine = game;
    mine.channel = 2;
    mine.layer_id = 0;
    mine.enable = 1;
    mine.info.zorder = (unsigned char)(game.info.zorder + 1);
    mine.info.alpha_mode = 0;   /* alfa per pixel */
    mine.info.alpha_value = 255;
    mine.info.fb.addr[0] = fx.smem_start + off;
    mine.info.fb.addr[1] = mine.info.fb.addr[2] = 0;
    mine.info.fb.format = 0;
    mine.info.fb.pre_multiply = 0;
    for (int k = 0; k < 3; k++) { mine.info.fb.size[k].width = (unsigned)ov_w; mine.info.fb.size[k].height = (unsigned)ov_h; }
    mine.info.fb.crop = (struct disp_rect64){ 0, 0, (long long)ov_w << 32, (long long)ov_h << 32 };
    mine.info.screen_win = (struct disp_rect){ 0, 0, (unsigned)ov_w, (unsigned)ov_h };
    r = disp_layer(DISP_LAYER_SET_CONFIG, &mine);
    if (r < 0) { mine.channel = 0; r = disp_layer(DISP_LAYER_SET_CONFIG, &mine); }
    ui_log("menu: livello sul canale %u (esito %d), rotazione %d", mine.channel, r, ov_rot);
    return r >= 0;
}

/* zona (x, y, w, h) dell'immagine del menu (UI.w x UI.h) nel livello, ruotata se serve.
   Rotazione a quadratini di 32x32: si leggono in ordine dall'immagine (memoria normale), si
   girano in una piccola tabella e si scrivono nel livello a pezzi contigui (la memoria
   video è lenta se scritta a salti). */
static void layer_show(SDL_Surface *s, int x0, int y0, int w, int h) {
    const uint32_t *src = s->pixels;
    int sp = s->pitch / 4;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > s->w) w = s->w - x0;
    if (y0 + h > s->h) h = s->h - y0;
    if (w <= 0 || h <= 0) return;
    if (ov_rot == 0) {
        for (int y = y0; y < y0 + h && y < ov_h; y++) memcpy(ov + (size_t)y * ov_w + x0, src + (size_t)y * sp + x0, (size_t)w * 4);
        return;
    }
    enum { T = 32 };
    uint32_t tile[T][T];   /* tile[colonna][riga] dell'immagine */
    for (int ty = y0; ty < y0 + h; ty += T) {
        int bh = y0 + h - ty < T ? y0 + h - ty : T;
        for (int tx = x0; tx < x0 + w; tx += T) {
            int bw = x0 + w - tx < T ? x0 + w - tx : T;
            for (int j = 0; j < bh; j++) {
                const uint32_t *row = src + (size_t)(ty + j) * sp + tx;
                for (int i = 0; i < bw; i++) tile[i][ov_rot == 270 ? j : bh - 1 - j] = row[i];
            }
            for (int i = 0; i < bw; i++) {
                int x = tx + i;
                if (ov_rot == 270) {   /* (x, y) -> (X = y, Y = ov_h - 1 - x) */
                    int Y = ov_h - 1 - x;
                    if (Y >= 0 && ty + bh <= ov_w) memcpy(ov + (size_t)Y * ov_w + ty, tile[i], (size_t)bh * 4);
                } else {               /* 90: (x, y) -> (X = ov_w - 1 - y, Y = x) */
                    int X0 = ov_w - (ty + bh);
                    if (x < ov_h && X0 >= 0) memcpy(ov + (size_t)x * ov_w + X0, tile[i], (size_t)bh * 4);
                }
            }
        }
    }
}

static void layer_close(void) {
    if (dispfd >= 0 && mine.enable) { mine.enable = 0; disp_layer(DISP_LAYER_SET_CONFIG, &mine); }
    if (fbmem) munmap(fbmem, fblen);
    if (fbfd >= 0) close(fbfd);
    if (dispfd >= 0) close(dispfd);
}
#else
static bool layer_open(void) { return true; }
static void layer_show(SDL_Surface *s, int x0, int y0, int w, int h) { (void)s; (void)x0; (void)y0; (void)w; (void)h; }
static void layer_close(void) {}
#endif

/* Barra del volume sopra un gioco Linux, che intanto continua: la lancia gamewatch alla prima
   pressione dei tasti del volume; poi legge da sola i tasti seguenti e si chiude quando
   per 1,4 secondi non ne arrivano altri. */
static int volume_osd(int delta) {
    if (!layer_open()) { layer_close(); return RC_UNAVAILABLE; }
    Uint32 last = SDL_GetTicks();
    bool redraw = true;
    plat_keys_t pk;
    plat_poll(&pk);   /* apre i dispositivi: la pressione che ci ha avviati è già contata */
    while (SDL_GetTicks() - last < 1400) {
        plat_poll(&pk);
        int d = delta + pk.vol;
        delta = 0;
        if (d) {
            int v = UI.set.volume + d;
            UI.set.volume = v < 0 ? 0 : v > 20 ? 20 : v;
            settings_save(&UI.set, UI.root);
            last = SDL_GetTicks();
            redraw = true;
        }
        plat_apply(&UI.set);
        if (redraw) {
            theme_t *t = &UI.th;
            int w = M(0.62f), h = M(0.1f), x = (UI.w - w) / 2, y = M(0.05f), pad = M(0.03f);
            ui_overlay_begin(true);
            ui_rrect(x, y, w, h, h / 2, rgba(0, 0, 0, 180));
            int lw = ui_text(UI.f_small, T("settings.volume"), x + pad, y + (h - TTF_FontHeight(UI.f_small)) / 2, rgba(255, 255, 255, 255), ALIGN_LEFT);
            int bx = x + pad + lw + M(0.03f), bw = x + w - pad - bx, bh = M(0.014f);
            ui_rrect(bx, y + (h - bh) / 2, bw, bh, bh / 2, rgba(255, 255, 255, 70));
            ui_rrect(bx, y + (h - bh) / 2, bw * UI.set.volume / 20, bh, bh / 2, t->accent);
            layer_show(ui_overlay_end(), x, y, w, h);
            redraw = false;
        }
        SDL_Delay(16);
    }
    layer_close();
    return RC_RESUME;
}

int main(int argc, char **argv) {
    const char *root = getenv("ROOT") ? getenv("ROOT") : "/mnt/SDCARD/Arcade", *shot = NULL;
    const char *title = getenv("ARCADE_TITLE");
    int volume = 0;
    bool only_ask = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--volume") && i + 1 < argc) { volume = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--ask-suspend")) { asking = only_ask = true; continue; }   /* solo la finestra, sopra il gioco */
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
    }
    if (!ui_init_offscreen("Arcade", root)) return RC_UNAVAILABLE;
    if (volume) return volume_osd(volume);
    if (!title || !*title) title = T("game.linux_game");
    if (shot) {   /* prova sul Mac: menu già aperto, seconda voce scelta */
        sel = 1;
        ui_screenshot(shot);
        ui_overlay_begin(true);
        if (!only_ask) { draw_static(title); draw_rows(); }
        if (asking) draw_ask();
        ui_overlay_end();
        return 0;
    }
    if (!layer_open()) { layer_close(); return RC_UNAVAILABLE; }
    int rc = -1;
    /* il tasto Menu è ancora premuto da quando si è aperto il menu: si aspetta che si lasci */
    bool armed = false;
    /* Le parti fisse si disegnano e si copiano una volta; poi a ogni tasto si ridisegnano e si
       copiano solo le righe delle voci (senza GPU ogni pixel costa). Con un avviso a schermo
       si ridisegna tutto. I tempi dei primi disegni vanno nel log. */
    int last_sel = -1, last_vol = -1, last_bri = -1, logged = 0, last_ask = -1;
    bool full = true, toast_was = false;
    while (rc < 0) {
        /* qui si ridisegna solo se cambia qualcosa: senza orologio aggiornato ogni pressione
           sembrerebbe tenuta premuta da tempo e farebbe un passo in più */
        ui_clock();
        ui_poll();
        if (asking) {
            if (UI.in.power_long) rc = RC_POWER;
            else {
                int r = ui_confirm_input(&ask_sel);
                if (r >= 0) {
                    asking = false;
                    if (only_ask) rc = r ? RC_SLEEP : RC_RESUME;
                    else if (r) ui_suspend();
                }
            }
        } else if (!armed) {
            armed = !UI.in.down[BTN_MENU];
            memset(UI.in.pressed, 0, sizeof UI.in.pressed);
        } else rc = input();
        if (rc >= 0) break;
        plat_apply(&UI.set);
        bool toast = ui_toast_active();
        if (toast || toast_was) full = true;
        int ask_state = asking ? ask_sel : -1;
        if (ask_state != last_ask) { last_ask = ask_state; full = true; }
        toast_was = toast;
        if (full || sel != last_sel || UI.set.volume != last_vol || UI.set.brightness != last_bri) {
            last_sel = sel;
            last_vol = UI.set.volume;
            last_bri = UI.set.brightness;
            Uint64 f = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
            ui_overlay_begin(full);
            if (full && !only_ask) draw_static(title);
            if (!only_ask) draw_rows();
            if (asking) draw_ask();
            SDL_Surface *img = ui_overlay_end();
            Uint64 t1 = SDL_GetPerformanceCounter();
            if (full) layer_show(img, 0, 0, UI.w, UI.h);
            else layer_show(img, g_px, g_rows_y, g_pw, GM_COUNT * g_rh);
            Uint64 t2 = SDL_GetPerformanceCounter();
            if (logged < 4) {
                logged++;
                ui_log("menu: disegno %.1f ms, copia %.1f ms (%s)", (t1 - t0) * 1000.0 / f, (t2 - t1) * 1000.0 / f, full ? "tutto" : "righe");
            }
            full = false;
        }
        SDL_Delay(8);
    }
    layer_close();
    ui_log("menu del gioco Linux: %s", rc == RC_RESUME ? "riprendi" : rc == RC_QUIT ? "esci" : "spegni");
    return rc;
}
