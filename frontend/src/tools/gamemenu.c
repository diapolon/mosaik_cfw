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

enum { GM_RESUME, GM_VOLUME, GM_BRIGHT, GM_QUIT, GM_POWER, GM_COUNT };
enum { RC_RESUME = 0, RC_UNAVAILABLE = 3, RC_QUIT = 10, RC_POWER = 20 };

static int sel;
static float anim, hl;

static void row_text(int i, char *label, size_t ln, char *value, size_t vn) {
    static const char *keys[GM_COUNT] = { "game.back_to_game", "settings.volume", "settings.brightness", "game.exit", "game.power" };
    snprintf(label, ln, "%s", T(keys[i]));
    value[0] = 0;
    if (i == GM_VOLUME) snprintf(value, vn, "%d", UI.set.volume);
    if (i == GM_BRIGHT) snprintf(value, vn, "%d", UI.set.brightness);
}

/* come menu_draw del runner, pagina principale */
static void draw(const char *title) {
    theme_t *t = &UI.th;
    anim = ui_approach(anim, 1);
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, (Uint8)(150 * anim)));
    int hb = M(t->hintbar_h), my = M(0.035f);
    int pw = M(0.80f);
    if (pw > UI.w - 2 * M(t->margin)) pw = UI.w - 2 * M(t->margin);
    int ph = UI.h - hb - 2 * my;
    int px = M(t->margin) - (int)((1 - anim) * pw), py = my, r = M(t->panel_radius) * 2;
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 110));
    ui_rrect(px, py, pw, ph, r, t->surface);

    char buf[300], label[200], value[120];
    int pad = M(0.04f), y = py + pad;
    ui_text_fit(UI.f_title, title, pw - 2 * pad, buf, sizeof buf);
    ui_text(UI.f_title, buf, px + pad, y, t->text, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_title);
    ui_text(UI.f_small, T("game.linux_game"), px + pad, y, t->accent, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_small) + M(0.02f);

    int note_h = TTF_FontHeight(UI.f_small) + M(0.02f);
    ui_text_fit(UI.f_small, T("game.linux_note"), pw - 2 * pad, buf, sizeof buf);
    ui_text(UI.f_small, buf, px + pad, py + ph - pad / 2 - note_h + M(0.01f), t->text_dim, ALIGN_LEFT);

    int rh = M(t->list_row_h);
    hl = ui_approach(hl, (float)sel * rh);
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    ui_rrect(px + M(0.02f), y + (int)hl, pw - M(0.04f), rh, M(t->panel_radius), t->accent);
    for (int i = 0; i < GM_COUNT; i++) {
        int ry = y + i * rh;
        bool is_sel = i == sel;
        row_text(i, label, sizeof label, value, sizeof value);
        color_t c = is_sel ? t->accent_text : t->text;
        int ty = ry + (rh - fh) / 2, right = px + pw - M(0.05f), vw = 0;
        if (value[0]) {
            snprintf(buf, sizeof buf, is_sel ? "‹ %s ›" : "%s", value);
            vw = ui_text(f, buf, right, ty, is_sel ? c : t->text_dim, ALIGN_RIGHT) + M(0.03f);
        }
        ui_text_fit(f, label, right - vw - (px + M(0.05f)), buf, sizeof buf);
        ui_text(f, buf, px + M(0.05f), ty, c, ALIGN_LEFT);
    }

    ui_rect(0, UI.h - hb, UI.w, hb, with_alpha(t->bg, (int)(235 * anim)));
    const char *h[] = { "B", T("game.back_to_game"), "<>", T("common.change"), "A", T("common.ok") };
    ui_hints(h, 6);
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
        if (sel == GM_POWER) return RC_POWER;
    }
    if (UI.in.power) ui_suspend();
    if (UI.in.power_long) return RC_POWER;
    return -1;
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
    size_t off = (fx.smem_len - need) & ~(size_t)4095;
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

/* immagine del menu (UI.w x UI.h) nel livello, ruotata se serve */
static void layer_show(SDL_Surface *s) {
    const uint32_t *src = s->pixels;
    int sp = s->pitch / 4, W = s->w, H = s->h;
    if (ov_rot == 0) {
        for (int y = 0; y < H && y < ov_h; y++) memcpy(ov + (size_t)y * ov_w, src + (size_t)y * sp, (size_t)(W < ov_w ? W : ov_w) * 4);
    } else if (ov_rot == 270) {   /* (x, y) -> (y, ov_h - 1 - x) */
        for (int y = 0; y < H && y < ov_w; y++)
            for (int x = 0; x < W && x < ov_h; x++) ov[(size_t)(ov_h - 1 - x) * ov_w + y] = src[(size_t)y * sp + x];
    } else {                      /* 90: (x, y) -> (ov_w - 1 - y, x) */
        for (int y = 0; y < H && y < ov_w; y++)
            for (int x = 0; x < W && x < ov_h; x++) ov[(size_t)x * ov_w + (ov_w - 1 - y)] = src[(size_t)y * sp + x];
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
static void layer_show(SDL_Surface *s) { (void)s; }
static void layer_close(void) {}
#endif

int main(int argc, char **argv) {
    const char *root = getenv("ROOT") ? getenv("ROOT") : "/mnt/SDCARD/Arcade", *shot = NULL;
    const char *title = getenv("ARCADE_TITLE");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
    }
    if (!ui_init_offscreen("Arcade", root)) return RC_UNAVAILABLE;
    if (!title || !*title) title = T("game.linux_game");
    if (shot) {   /* prova sul Mac: menu già aperto, seconda voce scelta */
        sel = 1;
        anim = 1;
        hl = (float)sel * M(UI.th.list_row_h);
        ui_screenshot(shot);
        ui_overlay_begin();
        draw(title);
        ui_overlay_end();
        return 0;
    }
    if (!layer_open()) { layer_close(); return RC_UNAVAILABLE; }
    int rc = -1;
    /* il tasto Menu è ancora premuto da quando si è aperto il menu: si aspetta che si lasci */
    bool armed = false;
    while (rc < 0) {
        ui_poll();
        if (!armed) {
            armed = !UI.in.down[BTN_MENU];
            memset(UI.in.pressed, 0, sizeof UI.in.pressed);
        } else rc = input();
        plat_apply(&UI.set);
        ui_overlay_begin();
        draw(title);
        layer_show(ui_overlay_end());
        SDL_Delay(16);
    }
    layer_close();
    ui_log("menu del gioco Linux: %s", rc == RC_RESUME ? "riprendi" : rc == RC_QUIT ? "esci" : "spegni");
    return rc;
}
