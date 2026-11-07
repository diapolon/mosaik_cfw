/* libui: finestra, disegno, testo, input, suoni e temi, condivisa da launcher e runner. */
#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>
#include "settings.h"
#include "lang.h"

typedef struct { Uint8 r, g, b, a; } color_t;

enum {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_X, BTN_Y,
    BTN_L, BTN_R, BTN_L2, BTN_R2,
    BTN_SELECT, BTN_START, BTN_MENU,
    BTN_COUNT
};

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
enum { SND_MOVE, SND_OK, SND_BACK, SND_ERROR, SND_COUNT };

typedef struct {
    bool down[BTN_COUNT];
    bool pressed[BTN_COUNT];   /* fronte di pressione, con ripetizione per le direzioni */
    bool quit;
    bool any;
    bool home, f1, f2;   /* tasti extra della Brick Pro (HOME laterale, barrette F1/F2) */
    bool power;          /* tasto di accensione, pressione breve (solo nel sistema su SD) */
    bool power_long;     /* tasto di accensione tenuto premuto */
    int fn;              /* interruttore FN: -1 ignoto, 0 o 1 */
} input_t;

typedef struct {
    char id[64], name[64], author[64], dir[1024], layout[16], title[64];
    char font_path[1024], title_font_path[1024], bg_image[1024];
    color_t bg, bg2, surface, text, text_dim, accent, accent_text, tile, shadow, line;
    /* misure in frazioni dell'altezza dello schermo */
    float topbar_h, hintbar_h, margin, tile_size, grid_tile, tile_gap, tile_radius;
    float select_border, icon_size, font_small, font_normal, font_title, font_big;
    float list_row_h, panel_radius;
    float tile_aspect;   /* larghezza/altezza dei riquadri: 1 quadrati, 1.333 per le schermate 4:3 */
    float anim_speed;
    bool pulse, scanlines, title_upper, waves;
    char sound_style[16];
    char variants[4][16];
    int nvariants;
    /* immagini dei giochi (Media/<tipo>/<rom>.png), in ordine di preferenza */
    char cover_types[4][16], preview_types[4][16];
    int ncover, npreview;
    bool cover_fit;      /* true: immagine intera su fondo scuro; false: ritaglio a riempire */
    bool show_marquee;   /* marquee sopra l'anteprima nel layout a elenco */
} theme_t;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *canvas;
    int w, h;
    double t, dt;
    input_t in;
    bool port_down[4][BTN_COUNT];   /* tasti premuti per giocatore (porte 1-4) */
    theme_t th;
    settings_t set;
    char root[1024];
    TTF_Font *f_small, *f_normal, *f_title, *f_big;
    SDL_AudioDeviceID audio;
    int audio_rate;
    bool automation;
} ui_t;

extern ui_t UI;

/* ciclo di vita */
bool ui_init(const char *app, const char *root, const char *auto_script, int audio_rate, bool vsync);
void ui_shutdown(void);
/* chiusura con codice di uscita, scritto prima in System/exit.txt */
void ui_shutdown_code(int code);
/* sospensione (sistema su SD): audio in pausa, console in sospensione, poi si riprende */
void ui_suspend(void);
void ui_apply_device(void);          /* ridimensiona finestra e canvas secondo il modello */
void ui_poll(void);
/* controller collegati: nome e giocatore assegnato (0 = giocatore 1) */
int ui_pads(char names[][64], int *ports, int max);
void ui_begin(void);                 /* disegno sul canvas con lo sfondo del tema */
void ui_end(void);                   /* presentazione, luminosità, screenshot automatici */
void ui_log(const char *fmt, ...);

/* misure: frazione dell'altezza -> pixel */
int M(float f);
color_t rgba(Uint8 r, Uint8 g, Uint8 b, Uint8 a);
color_t with_alpha(color_t c, int a);
color_t mix(color_t a, color_t b, float t);

/* forme */
void ui_rect(int x, int y, int w, int h, color_t c);
void ui_rrect(int x, int y, int w, int h, int r, color_t c);
void ui_rborder(int x, int y, int w, int h, int r, int thick, color_t c);
void ui_shadow(int x, int y, int w, int h, int r, int blur, color_t c);
void ui_circle(int cx, int cy, int rad, color_t c);
void ui_ring(int cx, int cy, int rad, int thick, color_t c);
void ui_star(int cx, int cy, int rad, color_t c);
void ui_vgradient(int x, int y, int w, int h, color_t top, color_t bottom);
void ui_triangle(int x1, int y1, int x2, int y2, int x3, int y3, color_t c);
void ui_gear(int cx, int cy, int rad, color_t c);

/* ritagli annidati: ogni push restringe l'area di disegno, pop la ripristina */
void ui_clip_push(int x, int y, int w, int h);
void ui_clip_pop(void);

/* testo */
TTF_Font *ui_font(const char *path, int px);
int ui_text(TTF_Font *f, const char *s, int x, int y, color_t c, int align);
int ui_text_w(TTF_Font *f, const char *s);
void ui_text_fit(TTF_Font *f, const char *s, int maxw, char *out, size_t n);
int ui_wrap(TTF_Font *f, const char *s, int maxw, char lines[][160], int maxlines);

/* immagini e suoni */
SDL_Texture *ui_image(const char *path);
/* dimentica i file segnati come mancanti: dopo un download vanno ricercati */
void ui_image_forget_missing(void);
/* immagine ritagliata per riempire il riquadro, con angoli arrotondati */
void ui_image_fill(SDL_Texture *img, int x, int y, int w, int h, int r);
/* immagine intera dentro il riquadro, proporzioni conservate */
void ui_image_fit(SDL_Texture *img, int x, int y, int w, int h);
/* versione sfocata dell'immagine (in cache), da disegnare ingrandita come sfondo */
SDL_Texture *ui_image_blur(SDL_Texture *img);
void ui_sound(int kind);
float ui_gain(void);
/* percentuale batteria (100 se ignota); TRIMUI_FAKE_BATTERY=n la simula nelle prove */
int ui_battery(bool *charging);                 /* guadagno audio dal volume delle impostazioni */
void ui_play_pcm(const Sint16 *stereo, int frames);
/* il launcher può cedere l'uscita audio a SDL_mixer (musica): i suoni passano da play;
   con NULL si riapre l'uscita normale */
void ui_audio_takeover(void (*play)(const Sint16 *stereo, int frames));

/* animazione: avvicinamento esponenziale al valore obiettivo */
float ui_approach(float v, float target);

/* elementi comuni del tema */
void ui_topbar(const char *title);
void ui_hints(const char *const *items, int n);   /* coppie tasto, etichetta */
void ui_button_glyph(const char *btn, int cx, int cy, int size);
void ui_toast(const char *msg);
/* salva il prossimo frame del canvas in un PNG */
void ui_screenshot(const char *path);
void ui_draw_toast(void);

/* temi */
bool theme_load(theme_t *t, const char *root, const char *id, const char *variant);
int theme_list(const char *root, char ids[][64], char names[][64], int max);
void ui_reload_theme(void);
