#include "ui.h"
#include "wifi.h"
#include <unistd.h>
#include "platform.h"
#include "json.h"
#include "lang.h"
#include <SDL_image.h>
/* NON includere <linux/input.h> qui: definisce BTN_A, BTN_LEFT, BTN_RIGHT... con valori
   evdev che coprirebbero i tasti di ui.h (è successo: ogni tasto scriveva fuori dagli
   array e il launcher andava in crash). Le letture evdev stanno in platform.c. */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>

ui_t UI;

static FILE *logf_;
static char log_path[1200];

#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#include <signal.h>
/* crash: scrive nel log la pila delle chiamate (indirizzi da tradurre con addr2line sul
   programma non ridotto), poi esce col codice del segnale */
static void on_crash(int sig) {
    void *frames[48];
    int n = backtrace(frames, 48);
    if (logf_) {
        fprintf(logf_, "CRASH: segnale %d, pila:\n", sig);
        fflush(logf_);
        backtrace_symbols_fd(frames, n, fileno(logf_));
        fsync(fileno(logf_));
    }
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void crash_handlers(void) {
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGFPE, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGABRT, on_crash);
}
#else
static void crash_handlers(void) {}
#endif
static Uint32 frame_no;

/* ------------------------------------------------------------------ log */

static void audio_open(void);
static double vol_save_at;   /* quando salvare il volume cambiato con i tasti, 0 = niente */

void ui_busy(const char *msg, double seconds) {
    if (!UI.ren) return;
    if (UI.automation) seconds = 0;
    Uint32 t0 = SDL_GetTicks();
    do {
        double t = (SDL_GetTicks() - t0) / 1000.0;
        ui_begin();
        theme_t *th = &UI.th;
        int cx = UI.w / 2, cy = UI.h / 2 - M(0.04f), r = M(0.055f), dot = M(0.011f);
        /* otto punti in cerchio, quello "in testa" pieno e gli altri via via più tenui */
        int head = (int)(t * 12) % 8;
        for (int i = 0; i < 8; i++) {
            double a = i * M_PI / 4 - M_PI / 2;
            int age = (head - i + 8) % 8;
            ui_circle(cx + (int)(cos(a) * r), cy + (int)(sin(a) * r), dot, with_alpha(th->accent, 255 - age * 28));
        }
        if (msg && *msg) {   /* i messaggi lunghi vanno a capo, fino a tre righe */
            char lines[3][160];
            int n = ui_wrap(UI.f_normal, msg, UI.w * 85 / 100, lines, 3), fh = TTF_FontHeight(UI.f_normal);
            for (int k = 0; k < n; k++) ui_text(UI.f_normal, lines[k], cx, cy + r + M(0.05f) + k * fh, th->text, ALIGN_CENTER);
        }
        ui_end();
        SDL_Delay(16);
    } while ((SDL_GetTicks() - t0) / 1000.0 < seconds);
}

/* finestra di conferma con due pulsanti (0 = no, 1 = sì) */
void ui_confirm_draw(const char *title, const char *body, const char *no, const char *yes, int sel) {
    theme_t *t = &UI.th;
    char buf[300];
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    int w = M(0.75f), h = M(0.36f), x = (UI.w - w) / 2, y = (UI.h - h) / 2, r = M(t->panel_radius) * 2;
    ui_shadow(x, y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(x, y, w, h, r, t->surface);
    ui_text_fit(UI.f_title, title, w - M(0.06f), buf, sizeof buf);
    ui_text(UI.f_title, buf, x + w / 2, y + M(0.06f), t->text, ALIGN_CENTER);
    ui_text_fit(UI.f_small, body, w - M(0.06f), buf, sizeof buf);
    ui_text(UI.f_small, buf, x + w / 2, y + M(0.06f) + TTF_FontHeight(UI.f_title) + M(0.01f), t->text_dim, ALIGN_CENTER);
    const char *labels[2] = { no, yes };
    ui_popup_buttons(x, y, w, h, labels, 2, sel, -1);
}

int ui_popup_buttons_h(void) { return M(0.085f) + M(0.04f) + M(0.02f); }

void ui_popup_buttons(int x, int y, int w, int h, const char *const *labels, int n, int sel, int danger) {
    theme_t *t = &UI.th;
    if (n < 1) return;
    /* un pulsante solo: al centro, largo come uno di due */
    int gap = M(0.03f), bw = n == 1 ? (w - gap * 3) / 2 : (w - gap * (n + 1)) / n, bh = M(0.085f), by = y + h - bh - M(0.04f);
    for (int i = 0; i < n; i++) {
        int bx = n == 1 ? x + (w - bw) / 2 : x + gap + i * (bw + gap);
        bool on = sel == i;
        color_t bg = on ? (i == danger ? rgba(220, 70, 70, 255) : t->accent) : with_alpha(t->text_dim, 40);
        ui_rrect(bx, by, bw, bh, bh / 2, bg);
        TTF_Font *f = ui_text_w(UI.f_normal, labels[i]) > bw - M(0.02f) ? UI.f_small : UI.f_normal;
        color_t fg = on ? (i == danger ? rgba(255, 255, 255, 255) : t->accent_text) : t->text;
        ui_text(f, labels[i], bx + bw / 2, by + (bh - TTF_FontHeight(f)) / 2, fg, ALIGN_CENTER);
    }
}

int ui_popup_input(int *sel, int n) {
    input_t *in = &UI.in;
    if (n > 1 && in->pressed[BTN_LEFT] && *sel > 0) { (*sel)--; ui_sound(SND_MOVE); }
    if (n > 1 && in->pressed[BTN_RIGHT] && *sel < n - 1) { (*sel)++; ui_sound(SND_MOVE); }
    if (*sel >= n) *sel = n - 1;
    if (*sel < 0) *sel = 0;
    if (in->pressed[BTN_B]) return -2;
    if (in->pressed[BTN_A]) return *sel;
    return -1;
}

/* tasti della finestra: 1 sì, 0 no, -1 ancora aperta. Il tasto di accensione conferma. */
int ui_confirm_input(int *sel) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_LEFT] || in->pressed[BTN_RIGHT]) { *sel = !*sel; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) { ui_sound(SND_BACK); return 0; }
    if (in->power) { ui_sound(SND_OK); return 1; }
    if (in->pressed[BTN_A]) { ui_sound(*sel ? SND_OK : SND_BACK); return *sel; }
    return -1;
}

/* riga del log scritta subito sulla SD: dopo un riavvio forzato quelle ancora in memoria si perdono */
static void log_durable(const char *msg) {
    ui_log("%s", msg);
    if (logf_) fsync(fileno(logf_));
}

/* chiusura dell'uscita audio in un thread a parte. Con l'altoparlante in muto (il launcher lo
   spegne mentre lavora su SD o Wi-Fi, per esempio con lo scraper) l'uscita si ferma e la
   chiusura aspetta finché la sospensione non la sblocca: sulla Smart Pro la console restava
   ferma sulla finestra, coi tasti morti (provato due volte). Dopo 3 s si prosegue senza audio. */
static SDL_sem *audio_closed;
static bool audio_stuck;
static int audio_close_thread(void *dev) {
    SDL_CloseAudioDevice((SDL_AudioDeviceID)(uintptr_t)dev);
    SDL_SemPost(audio_closed);
    return 0;
}
static bool audio_close_timed(SDL_AudioDeviceID dev) {
    if (!audio_closed && !(audio_closed = SDL_CreateSemaphore(0))) { SDL_CloseAudioDevice(dev); return true; }
    SDL_Thread *t = SDL_CreateThread(audio_close_thread, "audio-close", (void *)(uintptr_t)dev);
    if (!t) { SDL_CloseAudioDevice(dev); return true; }
    if (SDL_SemWaitTimeout(audio_closed, 3000) == 0) { SDL_WaitThread(t, NULL); return true; }
    SDL_DetachThread(t);   /* il semaforo resta: il thread lo userà se mai finisce */
    return false;
}

void ui_suspend(void) {
    if (!plat_standalone()) return;
    log_durable("sospensione richiesta");
    /* l'uscita audio si chiude e si riapre: dopo il risveglio chiuderla poteva bloccarsi */
    bool had_audio = UI.audio != 0 && !audio_stuck;
    if (had_audio) {
        SDL_AudioDeviceID dev = UI.audio;
        UI.audio = 0;
        if (!audio_close_timed(dev)) {
            audio_stuck = true;
            had_audio = false;
            log_durable("sospensione: l'uscita audio non si chiude entro 3 s, si prosegue senza audio");
        } else log_durable("sospensione: uscita audio chiusa");
    }
    /* schermo spento subito: il sistema può metterci qualche secondo ad addormentarsi */
    plat_screen_off(true);
    plat_apply(&UI.set);
    plat_suspend();
    plat_screen_off(false);
    /* l'amplificatore si riattiva solo con l'uscita già aperta e assestata: riaprirla ad
       amplificatore acceso fa gracchiare l'altoparlante */
    /* la chiusura bloccata di prima finisce di solito con la sospensione (l'uscita era ferma
       con l'altoparlante in muto): allora l'audio si riapre */
    if (audio_stuck && audio_closed && SDL_SemTryWait(audio_closed) == 0) {
        audio_stuck = false;
        had_audio = true;
        ui_log("risveglio: l'uscita audio si è chiusa, si riapre");
    }
    if (had_audio) { audio_open(); SDL_Delay(400); }
    plat_audio_unmute();
    /* tasti premuti prima o durante il sonno: non devono valere dopo il risveglio */
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    memset(UI.in.pressed, 0, sizeof UI.in.pressed);
    UI.in.power = UI.in.power_long = false;
}

/* Registro diagnostico: 0 spento, 1 essenziale, 2 prestazioni, 3 completo.
   ui_debug_on(1) = prestazioni, ui_debug_on(2) = completo. */
bool ui_debug_on(int level) { return UI.set.log_level > level; }

/* spento: nessun file e niente su stderr. Sul Mac sempre acceso; ARCADE_LOG lo accende quando
   il launcher è caduto, perché a quel punto dalle impostazioni non si può più farlo */
static bool log_on(void) {
    static int forced = -1;
    if (forced < 0) forced = !plat_is_device() || getenv("ARCADE_LOG") != NULL;
    return forced || UI.set.log_level > 0;
}


void ui_mark(const char *what) {
    if (!ui_debug_on(1)) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ui_log("tempi: %s a %.2f s", what, ts.tv_sec + ts.tv_nsec / 1e9);
}

void ui_log(const char *fmt, ...) {
    if (!log_on()) return;
    if (!logf_ && log_path[0]) logf_ = fopen(log_path, "a");
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    fprintf(stderr, "%s\n", msg);
    if (logf_) {
        fprintf(logf_, "%02d:%02d:%02d %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
        fflush(logf_);
    }
}

/* ------------------------------------------------------------- colori */

int M(float f) { return (int)lroundf(f * (float)UI.h); }

color_t rgba(Uint8 r, Uint8 g, Uint8 b, Uint8 a) { return (color_t){ r, g, b, a }; }

color_t with_alpha(color_t c, int a) {
    c.a = (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a);
    return c;
}

color_t mix(color_t a, color_t b, float t) {
    return (color_t){ (Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t),
                      (Uint8)(a.b + (b.b - a.b) * t), (Uint8)(a.a + (b.a - a.a) * t) };
}

static color_t parse_color(const char *s, color_t def) {
    if (!s || s[0] != '#') return def;
    unsigned v = (unsigned)strtoul(s + 1, NULL, 16);
    size_t n = strlen(s + 1);
    if (n == 6) return rgba(v >> 16, (v >> 8) & 255, v & 255, 255);
    if (n == 8) return rgba(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return def;
}

/* -------------------------------------------------------------- forme */
/* Ogni forma arrotondata è una texture "9-slice": angoli disegnati una volta
   con antialiasing, bordi e centro stirati. Funziona per qualsiasi misura. */

typedef struct { int r, thick, blur, k; SDL_Texture *tex; } shape_t;
static shape_t shapes[96];
static int nshapes;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static shape_t *shape_get(int r, int thick, int blur) {
    for (int i = 0; i < nshapes; i++)
        if (shapes[i].r == r && shapes[i].thick == thick && shapes[i].blur == blur) return &shapes[i];
    if (nshapes == (int)(sizeof shapes / sizeof shapes[0])) {
        SDL_DestroyTexture(shapes[0].tex);
        memmove(shapes, shapes + 1, sizeof(shape_t) * (size_t)(--nshapes));
    }
    int k = r + blur + 1, size = 2 * k + 1;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    Uint32 *px = s->pixels;
    float lo = (float)blur, hi = (float)(size - blur), half = (hi - lo) / 2, cx = (float)size / 2;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float qx = fabsf(x + 0.5f - cx) - (half - r), qy = fabsf(y + 0.5f - cx) - (half - r);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
            float a;
            if (blur > 0) {
                float t = clampf((d + blur * 0.15f) / (blur * 1.15f), 0, 1);
                a = (1 - t) * (1 - t);
            } else {
                a = clampf(0.5f - d, 0, 1);
                if (thick > 0) a *= clampf(0.5f + d + thick, 0, 1);
            }
            px[y * (s->pitch / 4) + x] = ((Uint32)(a * 255) << 24) | 0xFFFFFF;
        }
    SDL_Texture *t = SDL_CreateTextureFromSurface(UI.ren, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    shapes[nshapes] = (shape_t){ r, thick, blur, k, t };
    return &shapes[nshapes++];
}

static void nine(shape_t *sh, int x, int y, int w, int h, color_t c) {
    int k = sh->k, s = 2 * k + 1;
    SDL_SetTextureColorMod(sh->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(sh->tex, c.a);
    int cw = w - 2 * k, ch = h - 2 * k;
    SDL_Rect src[9] = {
        { 0, 0, k, k }, { k, 0, 1, k }, { k + 1, 0, k, k },
        { 0, k, k, 1 }, { k, k, 1, 1 }, { k + 1, k, k, 1 },
        { 0, k + 1, k, k }, { k, k + 1, 1, k }, { k + 1, k + 1, k, k },
    };
    SDL_Rect dst[9] = {
        { x, y, k, k }, { x + k, y, cw, k }, { x + w - k, y, k, k },
        { x, y + k, k, ch }, { x + k, y + k, cw, ch }, { x + w - k, y + k, k, ch },
        { x, y + h - k, k, k }, { x + k, y + h - k, cw, k }, { x + w - k, y + h - k, k, k },
    };
    (void)s;
    for (int i = 0; i < 9; i++)
        if (dst[i].w > 0 && dst[i].h > 0) SDL_RenderCopy(UI.ren, sh->tex, &src[i], &dst[i]);
}

static int fit_radius(int r, int w, int h) {
    int m = (w < h ? w : h) / 2;
    return r > m ? m : r < 0 ? 0 : r;
}

void ui_rect(int x, int y, int w, int h, color_t c) {
    SDL_SetRenderDrawColor(UI.ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(UI.ren, &(SDL_Rect){ x, y, w, h });
}

void ui_rrect(int x, int y, int w, int h, int r, color_t c) {
    if (w <= 0 || h <= 0) return;
    r = fit_radius(r, w, h);
    shape_t *sh = shape_get(r, 0, 0);
    /* la texture ha un pixel di margine per l'antialiasing */
    nine(sh, x - 1, y - 1, w + 2, h + 2, c);
}

void ui_rborder(int x, int y, int w, int h, int r, int thick, color_t c) {
    if (w <= 0 || h <= 0 || thick <= 0) return;
    r = fit_radius(r, w, h);
    nine(shape_get(r, thick, 0), x - 1, y - 1, w + 2, h + 2, c);
}

void ui_shadow(int x, int y, int w, int h, int r, int blur, color_t c) {
    if (blur <= 0) return;
    r = fit_radius(r, w, h);
    nine(shape_get(r, 0, blur), x - blur - 1, y - blur - 1, w + 2 * blur + 2, h + 2 * blur + 2, c);
}

void ui_circle(int cx, int cy, int rad, color_t c) { ui_rrect(cx - rad, cy - rad, 2 * rad, 2 * rad, rad, c); }

void ui_ring(int cx, int cy, int rad, int thick, color_t c) {
    ui_rborder(cx - rad, cy - rad, 2 * rad, 2 * rad, rad, thick, c);
}

/* Simboli vettoriali (stella, ingranaggio): disegnati a 4x in una texture e
   rimpiccioliti con filtro lineare, così i bordi restano morbidi a ogni dimensione. */
static void fan(float cx, float cy, const float *xs, const float *ys, int n, SDL_Color col) {
    SDL_Vertex v[130];
    int idx[384], k = 0;
    v[0] = (SDL_Vertex){ { cx, cy }, col, { 0, 0 } };
    for (int i = 0; i < n; i++) v[i + 1] = (SDL_Vertex){ { xs[i], ys[i] }, col, { 0, 0 } };
    for (int i = 0; i < n; i++) {
        idx[k++] = 0;
        idx[k++] = i + 1;
        idx[k++] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(UI.ren, NULL, v, n + 1, idx, k);
}

enum { GLYPH_STAR, GLYPH_GEAR, GLYPH_WIFI_DOT, GLYPH_WIFI_1, GLYPH_WIFI_2, GLYPH_WIFI_3 };

/* arco spesso con le estremità arrotondate, centrato in (cx, cy) */
static void arc_paint(float cx, float cy, float rad, float thick, float half, SDL_Color col) {
    enum { STEPS = 28 };
    SDL_Vertex v[(STEPS + 1) * 2];
    int idx[STEPS * 6], k = 0;
    float xs[24], ys[24];
    for (int i = 0; i <= STEPS; i++) {
        float a = -(float)M_PI / 2 - half + 2 * half * i / STEPS, ca = cosf(a), sa = sinf(a);
        v[i * 2] = (SDL_Vertex){ { cx + ca * (rad - thick / 2), cy + sa * (rad - thick / 2) }, col, { 0, 0 } };
        v[i * 2 + 1] = (SDL_Vertex){ { cx + ca * (rad + thick / 2), cy + sa * (rad + thick / 2) }, col, { 0, 0 } };
    }
    for (int i = 0; i < STEPS; i++) {
        int b = i * 2;
        idx[k++] = b; idx[k++] = b + 1; idx[k++] = b + 2;
        idx[k++] = b + 1; idx[k++] = b + 3; idx[k++] = b + 2;
    }
    SDL_RenderGeometry(UI.ren, NULL, v, (STEPS + 1) * 2, idx, k);
    for (int e = -1; e <= 1; e += 2) {
        float a = -(float)M_PI / 2 + e * half, ex = cx + cosf(a) * rad, ey = cy + sinf(a) * rad;
        for (int i = 0; i < 24; i++) {
            float b = 2 * (float)M_PI * i / 24;
            xs[i] = ex + cosf(b) * thick / 2;
            ys[i] = ey + sinf(b) * thick / 2;
        }
        fan(ex, ey, xs, ys, 24, col);
    }
}

static void glyph_paint(int kind, float o, float r) {
    SDL_Color white = { 255, 255, 255, 255 }, clear = { 0, 0, 0, 0 };
    float xs[128], ys[128];
    int n = 0;
    if (kind >= GLYPH_WIFI_DOT) {
        /* ventaglio del Wi-Fi: punto in basso e tre archi concentrici */
        float ax = o, ay = o + r * 0.78f, half = 40 * (float)M_PI / 180;
        if (kind == GLYPH_WIFI_DOT) {
            for (int i = 0; i < 32; i++) {
                float a = 2 * (float)M_PI * i / 32;
                xs[i] = ax + cosf(a) * r * 0.17f;
                ys[i] = ay + sinf(a) * r * 0.17f;
            }
            fan(ax, ay, xs, ys, 32, white);
        } else {
            arc_paint(ax, ay, r * (0.60f + 0.40f * (kind - GLYPH_WIFI_1)), r * 0.25f, half, white);
        }
        return;
    }
    if (kind == GLYPH_STAR) {
        /* il centro della stella è un punto valido per il ventaglio di triangoli */
        for (int i = 0; i < 10; i++) {
            float a = (float)M_PI * (-0.5f + i / 5.0f), rr = (i & 1) ? r * 0.45f : r;
            xs[n] = o + cosf(a) * rr;
            ys[n] = o + sinf(a) * rr;
            n++;
        }
        fan(o, o, xs, ys, n, white);
        return;
    }
    /* ingranaggio: 8 denti trapezoidali sopra un disco, foro centrale trasparente */
    const int teeth = 8, steps = 12;
    for (int t = 0; t < teeth; t++)
        for (int i = 0; i < steps; i++) {
            float u = (float)i / steps, a = 2 * (float)M_PI * (t + u) / teeth;
            float rr = u < 0.12f ? r * (0.74f + 0.26f * u / 0.12f)
                     : u < 0.42f ? r
                     : u < 0.54f ? r * (1 - 0.26f * (u - 0.42f) / 0.12f)
                     : r * 0.74f;
            xs[n] = o + cosf(a) * rr;
            ys[n] = o + sinf(a) * rr;
            n++;
        }
    fan(o, o, xs, ys, n, white);
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_NONE);
    n = 0;
    for (int i = 0; i < 48; i++) {
        float a = 2 * (float)M_PI * i / 48;
        xs[n] = o + cosf(a) * r * 0.32f;
        ys[n] = o + sinf(a) * r * 0.32f;
        n++;
    }
    fan(o, o, xs, ys, n, clear);
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
}

typedef struct { int kind, rad; SDL_Texture *tex; } glyph_t;
static glyph_t glyphs[24];
static int nglyphs;

static void glyph_draw(int kind, int cx, int cy, int rad, color_t c) {
    if (rad < 2) return;
    glyph_t *g = NULL;
    for (int i = 0; i < nglyphs; i++)
        if (glyphs[i].kind == kind && glyphs[i].rad == rad) g = &glyphs[i];
    if (!g) {
        if (nglyphs == (int)(sizeof glyphs / sizeof glyphs[0])) {
            SDL_DestroyTexture(glyphs[0].tex);
            memmove(glyphs, glyphs + 1, sizeof(glyph_t) * (size_t)(--nglyphs));
        }
        g = &glyphs[nglyphs++];
        int S = rad * 2 * 4 + 8;
        *g = (glyph_t){ kind, rad, SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, S, S) };
        SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g->tex, SDL_ScaleModeLinear);
        SDL_Texture *prev = SDL_GetRenderTarget(UI.ren);
        SDL_Rect clip;
        SDL_bool clipped = SDL_RenderIsClipEnabled(UI.ren);
        SDL_RenderGetClipRect(UI.ren, &clip);
        SDL_SetRenderTarget(UI.ren, g->tex);
        SDL_RenderSetClipRect(UI.ren, NULL);
        SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, 0);
        SDL_RenderClear(UI.ren);
        glyph_paint(kind, S / 2.0f, rad * 4.0f);
        SDL_SetRenderTarget(UI.ren, prev);
        SDL_RenderSetClipRect(UI.ren, clipped ? &clip : NULL);
    }
    SDL_SetTextureColorMod(g->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(g->tex, c.a);
    int d = rad * 2 + 2;
    SDL_RenderCopy(UI.ren, g->tex, NULL, &(SDL_Rect){ cx - d / 2, cy - d / 2, d, d });
}

void ui_star(int cx, int cy, int rad, color_t c) { glyph_draw(GLYPH_STAR, cx, cy, rad, c); }
void ui_gear(int cx, int cy, int rad, color_t c) { glyph_draw(GLYPH_GEAR, cx, cy, rad, c); }

void ui_wifi_icon(int x, int y, int h, int level, color_t on, color_t off) {
    int cx = x + h / 2, cy = y + h / 2, rad = h * 6 / 10;
    glyph_draw(GLYPH_WIFI_DOT, cx, cy, rad, level > 0 ? on : off);
    for (int i = 0; i < 3; i++) glyph_draw(GLYPH_WIFI_1 + i, cx, cy, rad, i < level ? on : off);
}

void ui_triangle(int x1, int y1, int x2, int y2, int x3, int y3, color_t c) {
    SDL_Color col = { c.r, c.g, c.b, c.a };
    SDL_Vertex v[3] = {
        { { (float)x1, (float)y1 }, col, { 0, 0 } },
        { { (float)x2, (float)y2 }, col, { 0, 0 } },
        { { (float)x3, (float)y3 }, col, { 0, 0 } },
    };
    SDL_RenderGeometry(UI.ren, NULL, v, 3, NULL, 0);
}

static SDL_Rect clips[8];
static int nclips;

static void clip_apply(void) {
    SDL_RenderSetClipRect(UI.ren, nclips ? &clips[nclips - 1] : NULL);
}

void ui_clip_push(int x, int y, int w, int h) {
    SDL_Rect r = { x, y, w, h };
    if (nclips) SDL_IntersectRect(&clips[nclips - 1], &r, &r);
    if (r.w <= 0 || r.h <= 0) r = (SDL_Rect){ 0, 0, 1, 1 };   /* un rettangolo vuoto per SDL significa "nessun ritaglio" */
    if (nclips < 8) clips[nclips++] = r;
    clip_apply();
}

void ui_clip_pop(void) {
    if (nclips) nclips--;
    clip_apply();
}

void ui_vgradient(int x, int y, int w, int h, color_t top, color_t bottom) {
    SDL_Color a = { top.r, top.g, top.b, top.a }, b = { bottom.r, bottom.g, bottom.b, bottom.a };
    SDL_Vertex v[4] = {
        { { (float)x, (float)y }, a, { 0, 0 } }, { { (float)(x + w), (float)y }, a, { 0, 0 } },
        { { (float)(x + w), (float)(y + h) }, b, { 0, 0 } }, { { (float)x, (float)(y + h) }, b, { 0, 0 } },
    };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(UI.ren, NULL, v, 4, idx, 6);
}

/* -------------------------------------------------------------- testo */

typedef struct { char path[1024]; int px; TTF_Font *f; } font_t;
static font_t fonts[16];
static int nfonts;

typedef struct { TTF_Font *f; Uint32 hash; char *s; SDL_Texture *tex; int w, h; Uint32 used; } textc_t;
static textc_t texts[384];

static Uint32 hash_str(const char *s) {
    Uint32 h = 2166136261u;
    while (*s) h = (h ^ (Uint8)*s++) * 16777619u;
    return h;
}

static const char *fallback_font(void) {
    static char p[1100];
    snprintf(p, sizeof p, "%s/Themes/_shared/fonts/Nunito.ttf:3", UI.root);
    return p;
}

/* "file.ttf:N" sceglie l'istanza N di un font variabile (es. Nunito: 3 Regular, 6 Bold).
   FreeType codifica l'istanza nei 16 bit alti dell'indice del font. */
TTF_Font *ui_font(const char *path, int px) {
    if (px < 6) px = 6;
    for (int i = 0; i < nfonts; i++)
        if (fonts[i].px == px && !strcmp(fonts[i].path, path)) return fonts[i].f;
    char file[1024];
    long instance = 0;
    snprintf(file, sizeof file, "%s", path);
    char *colon = strrchr(file, ':');
    if (colon && colon[1] >= '0' && colon[1] <= '9') {
        instance = strtol(colon + 1, NULL, 10);
        *colon = 0;
    }
    TTF_Font *f = TTF_OpenFontIndex(file, px, instance << 16);
    if (!f) {
        ui_log("font non trovato: %s (%s)", path, TTF_GetError());
        if (strcmp(path, fallback_font())) return ui_font(fallback_font(), px);
        return NULL;
    }
    if (nfonts == 16) { TTF_CloseFont(fonts[0].f); memmove(fonts, fonts + 1, sizeof(font_t) * 15); nfonts--; }
    snprintf(fonts[nfonts].path, sizeof fonts[0].path, "%s", path);
    fonts[nfonts].px = px;
    fonts[nfonts].f = f;
    nfonts++;
    return f;
}

static void text_cache_clear(void) {
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        if (texts[i].tex) SDL_DestroyTexture(texts[i].tex);
        free(texts[i].s);
    }
    memset(texts, 0, sizeof texts);
}

static void fonts_clear(void) {
    text_cache_clear();
    for (int i = 0; i < nfonts; i++) TTF_CloseFont(fonts[i].f);
    nfonts = 0;
}

static textc_t *text_get(TTF_Font *f, const char *s) {
    if (!f || !s || !*s) return NULL;
    Uint32 h = hash_str(s);
    size_t n = sizeof texts / sizeof texts[0], victim = 0;
    for (size_t i = 0; i < n; i++) {
        if (texts[i].tex && texts[i].f == f && texts[i].hash == h && !strcmp(texts[i].s, s)) {
            texts[i].used = frame_no;
            return &texts[i];
        }
        if (!texts[victim].tex) continue;
        if (!texts[i].tex || texts[i].used < texts[victim].used) victim = i;
    }
    textc_t *e = &texts[victim];
    if (e->tex) SDL_DestroyTexture(e->tex);
    free(e->s);
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, (SDL_Color){ 255, 255, 255, 255 });
    if (!surf) { memset(e, 0, sizeof *e); return NULL; }
    e->tex = SDL_CreateTextureFromSurface(UI.ren, surf);
    e->w = surf->w;
    e->h = surf->h;
    SDL_FreeSurface(surf);
    e->f = f;
    e->hash = h;
    e->s = strdup(s);
    e->used = frame_no;
    return e;
}

int ui_text_w(TTF_Font *f, const char *s) {
    int w = 0;
    if (f && s && *s) TTF_SizeUTF8(f, s, &w, NULL);
    return w;
}

int ui_text(TTF_Font *f, const char *s, int x, int y, color_t c, int align) {
    textc_t *e = text_get(f, s);
    if (!e) return 0;
    if (align == ALIGN_CENTER) x -= e->w / 2;
    else if (align == ALIGN_RIGHT) x -= e->w;
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_RenderCopy(UI.ren, e->tex, NULL, &(SDL_Rect){ x, y, e->w, e->h });
    return e->w;
}

void ui_text_fit(TTF_Font *f, const char *s, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    if (ui_text_w(f, out) <= maxw) return;
    size_t len = strlen(out);
    while (len > 0) {
        do len--; while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        while (len > 0 && out[len - 1] == ' ') len--;
        if (len + 4 > n) continue;
        memcpy(out + len, "…", 4);
        if (ui_text_w(f, out) <= maxw) return;
        out[len] = 0;
    }
}

int ui_wrap(TTF_Font *f, const char *s, int maxw, char lines[][160], int maxlines) {
    int n = 0;
    char cur[160] = "";
    const char *p = s;
    while (*p && n < maxlines) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        char word[160];
        snprintf(word, sizeof word, "%.*s", (int)(e - p), p);
        char test[330];
        snprintf(test, sizeof test, "%s%s%s", cur, *cur ? " " : "", word);
        if (*cur && ui_text_w(f, test) > maxw) {
            snprintf(lines[n++], 160, "%s", cur);
            snprintf(cur, sizeof cur, "%s", word);
        } else {
            snprintf(cur, sizeof cur, "%s", test);
        }
        p = e;
    }
    if (*cur && n < maxlines) snprintf(lines[n++], 160, "%s", cur);
    else if (*cur && n == maxlines) {
        char tmp[330];
        snprintf(tmp, sizeof tmp, "%s %s", lines[n - 1], cur);
        ui_text_fit(f, tmp, maxw, lines[n - 1], 160);
    }
    for (int i = 0; i < n; i++)
        if (ui_text_w(f, lines[i]) > maxw) {
            char tmp[160];
            snprintf(tmp, sizeof tmp, "%s", lines[i]);
            ui_text_fit(f, tmp, maxw, lines[i], 160);
        }
    return n;
}

/* ----------------------------------------------------------- immagini */

/* Cache LRU: anche i file mancanti restano in cache, così non si interroga la SD a ogni frame. */
typedef struct { char path[1024]; SDL_Texture *tex; Uint32 used; } img_t;
static img_t imgs[256];
static int nimgs;

/* copie con angoli arrotondati, legate alla texture d'origine */
typedef struct { SDL_Texture *src, *out; int w, h, r; Uint32 used; } rimg_t;
static rimg_t rimgs[64];

/* versioni sfocate delle immagini, per gli sfondi d'ambiente */
typedef struct { SDL_Texture *src, *out; Uint32 used; } blur_t;
static blur_t blurs[16];

static void rimg_forget(SDL_Texture *src) {
    for (size_t i = 0; i < sizeof rimgs / sizeof rimgs[0]; i++)
        if (rimgs[i].src == src && rimgs[i].out) {
            SDL_DestroyTexture(rimgs[i].out);
            memset(&rimgs[i], 0, sizeof rimgs[i]);
        }
    for (size_t i = 0; i < sizeof blurs / sizeof blurs[0]; i++)
        if (blurs[i].src == src && blurs[i].out) {
            rimg_forget(blurs[i].out);
            SDL_DestroyTexture(blurs[i].out);
            memset(&blurs[i], 0, sizeof blurs[i]);
        }
}

SDL_Texture *ui_image(const char *path) {
    for (int i = 0; i < nimgs; i++)
        if (!strcmp(imgs[i].path, path)) { imgs[i].used = frame_no; return imgs[i].tex; }
    img_t *e;
    if (nimgs < (int)(sizeof imgs / sizeof imgs[0])) {
        e = &imgs[nimgs++];
    } else {
        e = &imgs[0];
        for (int i = 1; i < nimgs; i++)
            if (imgs[i].used < e->used) e = &imgs[i];
        if (e->tex) { rimg_forget(e->tex); SDL_DestroyTexture(e->tex); }
    }
    snprintf(e->path, sizeof e->path, "%s", path);
    struct stat st;
    e->tex = stat(path, &st) == 0 ? IMG_LoadTexture(UI.ren, path) : NULL;
    e->used = frame_no;
    if (e->tex) SDL_SetTextureScaleMode(e->tex, SDL_ScaleModeLinear);
    return e->tex;
}

void ui_image_forget_missing(void) {
    int w = 0;
    for (int i = 0; i < nimgs; i++)
        if (imgs[i].tex) imgs[w++] = imgs[i];
    nimgs = w;
}

static SDL_Rect crop_fill(SDL_Texture *img, int w, int h) {
    int iw, ih;
    SDL_QueryTexture(img, NULL, NULL, &iw, &ih);
    SDL_Rect s = { 0, 0, iw, ih };
    if ((float)iw / ih > (float)w / h) {
        s.w = ih * w / h;
        s.x = (iw - s.w) / 2;
    } else {
        s.h = iw * h / w;
        s.y = (ih - s.h) / 4;   /* volantini e immagini verticali: meglio tenere la parte alta */
    }
    return s;
}

/* Sfocatura: l'immagine si dimezza più volte con filtro lineare fino a una ventina di
   pixel (ogni passo fa la media di 2x2), poi si ingrandisce di nuovo, sempre lineare. */
SDL_Texture *ui_image_blur(SDL_Texture *img) {
    blur_t *victim = &blurs[0];
    for (size_t i = 0; i < sizeof blurs / sizeof blurs[0]; i++) {
        blur_t *b = &blurs[i];
        if (b->out && b->src == img) { b->used = frame_no; return b->out; }
        if (!b->out || (victim->out && b->used < victim->used)) victim = b;
    }
    int w, h;
    SDL_QueryTexture(img, NULL, NULL, &w, &h);
    SDL_Texture *prev = SDL_GetRenderTarget(UI.ren), *cur = img;
    SDL_Rect clip;
    SDL_bool clipped = SDL_RenderIsClipEnabled(UI.ren);
    SDL_RenderGetClipRect(UI.ren, &clip);
    SDL_SetTextureBlendMode(img, SDL_BLENDMODE_NONE);
    while (w > 24 || h > 24) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        SDL_Texture *next = SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!next) break;
        SDL_SetTextureScaleMode(next, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(next, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(UI.ren, next);
        SDL_RenderSetClipRect(UI.ren, NULL);
        SDL_RenderCopy(UI.ren, cur, NULL, NULL);
        if (cur != img) SDL_DestroyTexture(cur);
        cur = next;
    }
    SDL_SetTextureBlendMode(img, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(UI.ren, prev);
    SDL_RenderSetClipRect(UI.ren, clipped ? &clip : NULL);
    if (cur == img) return img;
    SDL_SetTextureBlendMode(cur, SDL_BLENDMODE_BLEND);
    if (victim->out) { rimg_forget(victim->out); SDL_DestroyTexture(victim->out); }
    *victim = (blur_t){ img, cur, frame_no };
    return cur;
}

void ui_image_fit(SDL_Texture *img, int x, int y, int w, int h) {
    int iw, ih;
    SDL_QueryTexture(img, NULL, NULL, &iw, &ih);
    float s = fminf((float)w / iw, (float)h / ih);
    int dw = (int)(iw * s), dh = (int)(ih * s);
    SDL_RenderCopy(UI.ren, img, NULL, &(SDL_Rect){ x + (w - dw) / 2, y + (h - dh) / 2, dw, dh });
}

/* Angoli arrotondati: l'immagine si disegna in una texture, poi la forma arrotondata
   ne moltiplica il canale alfa con un blend personalizzato. */
void ui_image_fill(SDL_Texture *img, int x, int y, int w, int h, int r) {
    static SDL_BlendMode mask_mode;
    static int mask_ok = -1;
    SDL_Rect src = crop_fill(img, w, h);
    if (r <= 0 || mask_ok == 0) {
        SDL_RenderCopy(UI.ren, img, &src, &(SDL_Rect){ x, y, w, h });
        return;
    }
    rimg_t *e = NULL, *victim = &rimgs[0];
    for (size_t i = 0; i < sizeof rimgs / sizeof rimgs[0]; i++) {
        rimg_t *c = &rimgs[i];
        if (c->out && c->src == img && c->w == w && c->h == h && c->r == r) { e = c; break; }
        if (!c->out || (victim->out && c->used < victim->used)) victim = c;
    }
    if (!e) {
        if (mask_ok < 0) {
            mask_mode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
                                                   SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
        }
        e = victim;
        if (e->out) SDL_DestroyTexture(e->out);
        *e = (rimg_t){ img, SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h), w, h, r, 0 };
        SDL_SetTextureBlendMode(e->out, SDL_BLENDMODE_BLEND);
        SDL_Texture *prev = SDL_GetRenderTarget(UI.ren);
        SDL_Rect clip;
        SDL_bool clipped = SDL_RenderIsClipEnabled(UI.ren);
        SDL_RenderGetClipRect(UI.ren, &clip);
        SDL_SetRenderTarget(UI.ren, e->out);
        SDL_RenderSetClipRect(UI.ren, NULL);
        SDL_SetTextureBlendMode(img, SDL_BLENDMODE_NONE);
        SDL_RenderCopy(UI.ren, img, &src, NULL);
        SDL_SetTextureBlendMode(img, SDL_BLENDMODE_BLEND);
        shape_t *sh = shape_get(fit_radius(r, w, h), 0, 0);
        if (mask_ok < 0) mask_ok = SDL_SetTextureBlendMode(sh->tex, mask_mode) == 0;
        else SDL_SetTextureBlendMode(sh->tex, mask_mode);
        if (mask_ok) nine(sh, -1, -1, w + 2, h + 2, rgba(255, 255, 255, 255));
        else ui_log("blend personalizzato non supportato: immagini senza angoli arrotondati");
        SDL_SetTextureBlendMode(sh->tex, SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(UI.ren, prev);
        SDL_RenderSetClipRect(UI.ren, clipped ? &clip : NULL);
    }
    e->used = frame_no;
    SDL_RenderCopy(UI.ren, e->out, NULL, &(SDL_Rect){ x, y, w, h });
}

/* -------------------------------------------------------------- suoni */
/* Suoni di navigazione sintetizzati: i temi scelgono lo stile ("soft" o "retro"). */

static Sint16 *snd_buf[SND_COUNT];
static int snd_len[SND_COUNT];

int ui_battery(bool *charging) {
    const char *fake = getenv("TRIMUI_FAKE_BATTERY");
    if (fake) { *charging = getenv("TRIMUI_FAKE_CHARGING") != NULL; return atoi(fake); }
    int secs, pct = plat_battery(charging);
    if (pct >= 0) return pct;
    SDL_PowerState ps = SDL_GetPowerInfo(&secs, &pct);
    *charging = ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED;
    return pct;   /* -1: livello sconosciuto */
}

float ui_gain(void) {
    if (plat_is_device()) return 1.0f;   /* sulla console il volume è quello del codec */
    float v = UI.set.volume / 20.0f;
    return v * v;
}

static void synth(int kind, float f0, float f1, float dur, bool square) {
    int rate = UI.audio_rate, n = (int)(dur * rate);
    free(snd_buf[kind]);
    snd_buf[kind] = malloc(sizeof(Sint16) * 2 * (size_t)n);
    snd_len[kind] = n;
    double ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / n, f = f0 + (f1 - f0) * t;
        ph += 2 * M_PI * f / rate;
        float env = (1 - t) * (1 - t) * fminf(1, i / (rate * 0.002f));
        float s = square ? (sin(ph) > 0 ? 0.6f : -0.6f) : (float)sin(ph);
        Sint16 v = (Sint16)(s * env * 9000);
        snd_buf[kind][i * 2] = snd_buf[kind][i * 2 + 1] = v;
    }
}

static void (*pcm_hook)(const Sint16 *stereo, int frames);

static void sounds_build(void) {
    if (!UI.audio && !pcm_hook) return;
    bool sq = !strcmp(UI.th.sound_style, "retro");
    synth(SND_MOVE, sq ? 660 : 1250, sq ? 660 : 1150, sq ? 0.035f : 0.03f, sq);
    synth(SND_OK, sq ? 880 : 1400, sq ? 1320 : 1900, 0.08f, sq);
    synth(SND_BACK, sq ? 520 : 1000, sq ? 330 : 700, 0.07f, sq);
    synth(SND_ERROR, 220, 180, 0.14f, true);
}

static void audio_open(void) {
    /* il sistema su SD lo imposta se la scheda audio non riproduce: aprirla bloccherebbe */
    if (getenv("ARCADE_NO_AUDIO")) { ui_log("audio disattivato (ARCADE_NO_AUDIO)"); UI.audio = 0; return; }
    SDL_AudioSpec want = { .freq = UI.audio_rate, .format = AUDIO_S16SYS, .channels = 2, .samples = 512 }, have;
    UI.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (UI.audio) {
        SDL_PauseAudioDevice(UI.audio, 0);
        if (plat_is_device())
            ui_log("audio: driver %s, %d Hz, %d canali, %d campioni", SDL_GetCurrentAudioDriver(), have.freq, have.channels, have.samples);
    } else {
        ui_log("audio non disponibile: %s", SDL_GetError());
    }
}

void ui_audio_takeover(void (*play)(const Sint16 *stereo, int frames)) {
    if (play && UI.audio) { SDL_CloseAudioDevice(UI.audio); UI.audio = 0; }
    pcm_hook = play;
    if (!play && !UI.audio && !audio_stuck) audio_open();
}

void ui_play_pcm(const Sint16 *stereo, int frames) {
    if ((!UI.audio && !pcm_hook) || frames <= 0) return;
    static Sint16 *tmp;
    static int cap;
    if (frames * 2 > cap) { cap = frames * 2; tmp = realloc(tmp, sizeof(Sint16) * (size_t)cap); }
    float g = ui_gain();
    for (int i = 0; i < frames * 2; i++) tmp[i] = (Sint16)(stereo[i] * g);
    if (pcm_hook) pcm_hook(tmp, frames);
    else SDL_QueueAudio(UI.audio, tmp, (Uint32)(frames * 4));
}

void ui_sound(int kind) {
    if ((!UI.audio && !pcm_hook) || UI.automation || !snd_buf[kind]) return;
    if (UI.audio) SDL_ClearQueuedAudio(UI.audio);
    ui_play_pcm(snd_buf[kind], snd_len[kind]);
}

/* -------------------------------------------------------------- temi */

static void load_variant_colors(theme_t *t, jval *c) {
    t->bg = parse_color(json_str(c, "background", NULL), t->bg);
    t->bg2 = parse_color(json_str(c, "background2", NULL), t->bg);
    t->surface = parse_color(json_str(c, "surface", NULL), t->surface);
    t->text = parse_color(json_str(c, "text", NULL), t->text);
    t->text_dim = parse_color(json_str(c, "text_dim", NULL), t->text_dim);
    t->accent = parse_color(json_str(c, "accent", NULL), t->accent);
    t->accent_text = parse_color(json_str(c, "accent_text", NULL), t->accent_text);
    t->tile = parse_color(json_str(c, "tile", NULL), t->tile);
    t->shadow = parse_color(json_str(c, "shadow", NULL), t->shadow);
    t->line = parse_color(json_str(c, "line", NULL), t->line);
}

bool theme_load(theme_t *t, const char *root, const char *id, const char *variant) {
    char path[1200];
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", id);
    snprintf(t->dir, sizeof t->dir, "%s/Themes/%s", root, id);
    snprintf(path, sizeof path, "%s/theme.json", t->dir);
    /* valori di riserva: tema chiaro essenziale */
    t->bg = rgba(235, 235, 235, 255);
    t->bg2 = t->bg;
    t->surface = rgba(255, 255, 255, 255);
    t->text = rgba(45, 45, 45, 255);
    t->text_dim = rgba(120, 120, 120, 255);
    t->accent = rgba(0, 190, 220, 255);
    t->accent_text = rgba(255, 255, 255, 255);
    t->tile = rgba(255, 255, 255, 255);
    t->shadow = rgba(0, 0, 0, 40);
    t->line = rgba(0, 0, 0, 40);
    strcpy(t->layout, "carousel");
    strcpy(t->sound_style, "soft");
    strcpy(t->title, "Mosaik");
    t->topbar_h = 0.12f; t->hintbar_h = 0.10f; t->margin = 0.07f;
    t->tile_size = 0.36f; t->grid_tile = 0.25f; t->tile_gap = 0.02f; t->tile_radius = 0.01f;
    t->select_border = 0.007f; t->icon_size = 0.105f;
    t->font_small = 0.030f; t->font_normal = 0.040f; t->font_title = 0.052f; t->font_big = 0.075f;
    t->list_row_h = 0.075f; t->panel_radius = 0.02f; t->tile_aspect = 1;
    t->anim_speed = 14; t->pulse = true;
    strcpy(t->cover_types[0], "title");
    strcpy(t->cover_types[1], "snap");
    t->ncover = 2;
    strcpy(t->preview_types[0], "snap");
    strcpy(t->preview_types[1], "title");
    t->npreview = 2;

    jval *j = json_load(path);
    if (!j) {
        ui_log("tema %s non trovato, uso i valori di riserva", id);
        snprintf(t->name, sizeof t->name, "%s", id);
        snprintf(t->font_path, sizeof t->font_path, "%s/Themes/_shared/fonts/Nunito.ttf:3", root);
        snprintf(t->title_font_path, sizeof t->title_font_path, "%s", t->font_path);
        return false;
    }
    snprintf(t->name, sizeof t->name, "%s", json_str(j, "name", id));
    snprintf(t->author, sizeof t->author, "%s", json_str(j, "author", ""));
    snprintf(t->layout, sizeof t->layout, "%s", json_str(j, "layout", t->layout));
    snprintf(t->title, sizeof t->title, "%s", json_str(j, "title", t->title));
    snprintf(t->sound_style, sizeof t->sound_style, "%s", json_str(j, "sound_style", t->sound_style));

    const char *fr = json_str(j, "fonts.regular", NULL), *ft = json_str(j, "fonts.title", NULL);
    if (fr) snprintf(t->font_path, sizeof t->font_path, "%s/%s", t->dir, fr);
    else snprintf(t->font_path, sizeof t->font_path, "%s/Themes/_shared/fonts/Nunito.ttf:3", root);
    if (ft) snprintf(t->title_font_path, sizeof t->title_font_path, "%s/%s", t->dir, ft);
    else snprintf(t->title_font_path, sizeof t->title_font_path, "%s", t->font_path);
    const char *bgi = json_str(j, "background_image", NULL);
    if (bgi) snprintf(t->bg_image, sizeof t->bg_image, "%s/%s", t->dir, bgi);

#define MET(field) t->field = (float)json_num(j, "metrics." #field, t->field)
    MET(topbar_h); MET(hintbar_h); MET(margin); MET(tile_size); MET(grid_tile); MET(tile_gap);
    MET(tile_radius); MET(select_border); MET(icon_size); MET(font_small); MET(font_normal);
    MET(font_title); MET(font_big); MET(list_row_h); MET(panel_radius); MET(tile_aspect);
#undef MET
    t->anim_speed = (float)json_num(j, "animation.speed", t->anim_speed);
    t->pulse = json_bool(j, "animation.pulse", t->pulse);
    t->scanlines = json_bool(j, "effects.scanlines", false);
    t->waves = json_bool(j, "effects.waves", false);
    t->title_upper = json_bool(j, "title_uppercase", false);
    t->cover_fit = !strcmp(json_str(j, "media.cover_style", "fill"), "fit");
    t->show_marquee = json_bool(j, "media.marquee", false);
    jval *cv = json_get(j, "media.cover"), *pv = json_get(j, "media.preview");
    if (cv && cv->t == J_ARR) {
        t->ncover = 0;
        for (jval *v = cv->child; v && t->ncover < 4; v = v->next)
            if (v->t == J_STR) snprintf(t->cover_types[t->ncover++], 16, "%s", v->s);
    }
    if (pv && pv->t == J_ARR) {
        t->npreview = 0;
        for (jval *v = pv->child; v && t->npreview < 4; v = v->next)
            if (v->t == J_STR) snprintf(t->preview_types[t->npreview++], 16, "%s", v->s);
    }

    jval *vars = json_get(j, "variants");
    if (vars && vars->t == J_OBJ) {
        for (jval *v = vars->child; v && t->nvariants < 8; v = v->next)
            snprintf(t->variants[t->nvariants++], 16, "%s", v->key);
        jval *chosen = json_get(vars, variant);
        load_variant_colors(t, chosen ? chosen : vars->child);
    } else {
        load_variant_colors(t, json_get(j, "colors"));
    }
    json_free(j);
    return true;
}

int theme_list(const char *root, char ids[][64], char names[][64], int max) {
    char dirp[1100];
    snprintf(dirp, sizeof dirp, "%s/Themes", root);
    DIR *d = opendir(dirp);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s/theme.json", dirp, e->d_name);
        jval *j = json_load(p);
        if (!j) continue;
        snprintf(ids[n], 64, "%s", e->d_name);
        snprintf(names[n], 64, "%s", json_str(j, "name", e->d_name));
        json_free(j);
        n++;
    }
    closedir(d);
    /* ordine stabile per nome */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (strcasecmp(names[a], names[b]) > 0) {
                char t[64];
                memcpy(t, ids[a], 64); memcpy(ids[a], ids[b], 64); memcpy(ids[b], t, 64);
                memcpy(t, names[a], 64); memcpy(names[a], names[b], 64); memcpy(names[b], t, 64);
            }
    return n;
}

void ui_reload_theme(void) {
    fonts_clear();
    theme_load(&UI.th, UI.root, UI.set.theme, UI.set.variant);
    UI.f_small = ui_font(UI.th.font_path, M(UI.th.font_small));
    UI.f_normal = ui_font(UI.th.font_path, M(UI.th.font_normal));
    UI.f_title = ui_font(UI.th.title_font_path, M(UI.th.font_title));
    UI.f_big = ui_font(UI.th.title_font_path, M(UI.th.font_big));
    sounds_build();
}

/* -------------------------------------------------------------- input */

static double hold_t[BTN_COUNT], next_rep[BTN_COUNT];
/* Fonti dei tasti: tastiera (e automazione) più fino a 4 controller. I controller
   integrati nella console (nome con "TRIMUI", o TRIMUI_BUILTIN_PAD) sono il giocatore 1
   insieme alla tastiera; gli esterni diventano 2, 3, 4 nell'ordine in cui si collegano.
   Con "controller esterno come giocatore 1" il primo esterno passa al giocatore 1. */
typedef struct {
    SDL_GameController *gc;
    SDL_Joystick *js;    /* joystick senza mappatura SDL (i tasti integrati della TrimUI) */
    SDL_JoystickID id;
    bool builtin, down[BTN_COUNT], stick[4];
    char name[64];
} pad_t;
static pad_t pads[4];
static int npads;
static bool kb_down[BTN_COUNT], auto_down[BTN_COUNT];

/* nome vero del dispositivo (SDL rinomina i controller noti: il gamepad di trimui_inputd
   si presenta come Xbox 360 e SDL lo chiama "X360 Controller") */
static bool device_is_builtin(SDL_Joystick *js) {
    const char *path = js ? SDL_JoystickPath(js) : NULL;
    char name[128];
    if (!path || !plat_device_name(path, name, sizeof name)) return false;
    const char *hint = getenv("TRIMUI_BUILTIN_PAD");
    return strstr(name, hint && *hint ? hint : "TRIMUI") != NULL;
}

static bool is_builtin(const char *name) {
    const char *hint = getenv("TRIMUI_BUILTIN_PAD");
    if (hint && *hint) return strstr(name, hint) != NULL;
    return strstr(name, "TRIMUI") || strstr(name, "TrimUI") || strstr(name, "trimui");
}

/* giocatore di ogni fonte: -1 = tastiera, poi indice del controller */
static int port_of(int src) {
    bool builtin = src < 0 || pads[src].builtin;
    int ext_index = 0;
    for (int i = 0; i < npads && i < src; i++) ext_index += !pads[i].builtin;
    if (UI.set.ext_p1) {
        if (builtin) return 1;
        return ext_index == 0 ? 0 : ext_index + 1 > 3 ? 3 : ext_index + 1;
    }
    return builtin ? 0 : ext_index + 1 > 3 ? 3 : ext_index + 1;
}

/* Joystick "grezzi": numeri di pulsante come li espone il demone dei tasti TrimUI (da MinUI):
   B=0 A=1 Y=2 X=3 L=4 R=5 Select=6 Start=7 Menu=8, croce sul cappello 0, L2/R2 sugli assi
   2 e 5, levetta sinistra sugli assi 0 e 1. TRIMUI_JOYMAP="a=1,b=0,l2=a2" li cambia. */
static int joy_btn[BTN_COUNT] = { -1, -1, -1, -1, 1, 0, 3, 2, 4, 5, -1, -1, 6, 7, 8 };
static int joy_axis_l2 = 2, joy_axis_r2 = 5;

static void joymap_load(void) {
    static const char *names[BTN_COUNT] = { "up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "l2", "r2", "select", "start", "menu" };
    const char *env = getenv("TRIMUI_JOYMAP");
    if (!env) return;
    char buf[256], *save = NULL;
    snprintf(buf, sizeof buf, "%s", env);
    for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(it, '=');
        if (!eq) continue;
        *eq++ = 0;
        for (int b = 0; b < BTN_COUNT; b++) {
            if (strcmp(names[b], it)) continue;
            if (*eq == 'a' && (b == BTN_L2 || b == BTN_R2)) *(b == BTN_L2 ? &joy_axis_l2 : &joy_axis_r2) = atoi(eq + 1);
            else joy_btn[b] = atoi(eq);
        }
    }
}

static int joy_to_btn(int jb) {
    for (int b = 0; b < BTN_COUNT; b++)
        if (joy_btn[b] == jb) return b;
    return -1;
}

int ui_pads_external(void) {
    int n = 0;
    for (int i = 0; i < npads; i++) n += !pads[i].builtin;
    return n;
}

int ui_pads(char names[][64], int *ports, int max) {
    int n = 0;
    for (int i = 0; i < npads && n < max; i++, n++) {
        snprintf(names[n], 64, "%s", pads[i].name);
        ports[n] = port_of(i);
    }
    return n;
}

static void ports_update(void) {
    memset(UI.port_down, 0, sizeof UI.port_down);
    int kp = port_of(-1);
    for (int b = 0; b < BTN_COUNT; b++) UI.port_down[kp][b] = kb_down[b] || auto_down[b];
    for (int i = 0; i < npads; i++) {
        int p = port_of(i);
        for (int b = 0; b < BTN_COUNT; b++) {
            bool d = pads[i].down[b];
            if (b <= BTN_RIGHT) d = d || pads[i].stick[b - BTN_UP];
            UI.port_down[p][b] = UI.port_down[p][b] || d;
        }
    }
}

static void pad_announce(const pad_t *pd, bool added) {
    char msg[160];
    int idx = (int)(pd - pads);
    if (added) snprintf(msg, sizeof msg, T("pad.connected"), pd->name, port_of(idx) + 1);
    else snprintf(msg, sizeof msg, T("pad.removed"), pd->name);
    ui_log("%s%s", msg, pd->builtin ? " (integrato)" : "");
    /* i tasti della console sono sempre il giocatore 1: si annunciano solo i controller esterni */
    if (!pd->builtin) ui_toast(msg);
}

static pad_t *pad_by_id(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++)
        if (pads[i].id == id) return &pads[i];
    return NULL;
}

/* un tasto della fonte src cambia: aggiorna giocatori, stato complessivo e pressioni */
static void source_set(bool *src, int b, bool down) {
    if (src) src[b] = down;
    ports_update();
    bool agg = false;
    for (int p = 0; p < 4; p++) agg |= UI.port_down[p][b];
    if (down && !UI.in.down[b]) {
        UI.in.pressed[b] = true;
        hold_t[b] = UI.t;
        next_rep[b] = UI.t + 0.32;
        UI.in.any = true;
    }
    UI.in.down[b] = agg;
}

/* Tastiera del Mac: frecce, X=A, Z=B, S=X, A=Y, Q/W=L/R, 1/2=L2/R2,
   Invio=Start, Shift destro o Backspace=Select, Esc=Menu. */
static int key_btn(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_x: return BTN_A;
    case SDLK_z: return BTN_B;
    case SDLK_s: return BTN_X;
    case SDLK_a: return BTN_Y;
    case SDLK_q: return BTN_L;
    case SDLK_w: return BTN_R;
    case SDLK_1: return BTN_L2;
    case SDLK_2: return BTN_R2;
    case SDLK_RETURN: return BTN_START;
    case SDLK_RSHIFT: case SDLK_BACKSPACE: return BTN_SELECT;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return -1;
    }
}

/* Layout Nintendo come sulle TrimUI: A a destra, B in basso. */
static int pad_btn(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
    case SDL_CONTROLLER_BUTTON_B: return BTN_A;
    case SDL_CONTROLLER_BUTTON_A: return BTN_B;
    case SDL_CONTROLLER_BUTTON_Y: return BTN_X;
    case SDL_CONTROLLER_BUTTON_X: return BTN_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return BTN_L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return BTN_R;
    case SDL_CONTROLLER_BUTTON_BACK: return BTN_SELECT;
    case SDL_CONTROLLER_BUTTON_START: return BTN_START;
    case SDL_CONTROLLER_BUTTON_GUIDE: return BTN_MENU;
    default: return -1;
    }
}

/* ------------------------------------------------- automazione (test) */
/* Script: "wait:30,right,a,shot:/percorso.png,quit". Serve a provare
   l'interfaccia senza mani e a produrre screenshot. */

static char auto_buf[4096];
static char *auto_pos;
static int auto_wait;
static int auto_release = -1;
static char shot_path[1024];

void ui_screenshot(const char *path) { snprintf(shot_path, sizeof shot_path, "%s", path); }
static bool auto_quit;

static int btn_by_name(const char *s) {
    static const char *names[BTN_COUNT] = { "up", "down", "left", "right", "a", "b", "x", "y",
                                            "l", "r", "l2", "r2", "select", "start", "menu" };
    for (int i = 0; i < BTN_COUNT; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

static void auto_step(void) {
    if (auto_release >= 0) { UI.in.down[auto_release] = auto_down[auto_release] = false; auto_release = -1; }
    if (!auto_pos) return;
    if (auto_wait > 0) { auto_wait--; return; }
    if (!*auto_pos) { auto_pos = NULL; return; }
    char tok[1100];
    char *e = strchr(auto_pos, ',');
    size_t len = e ? (size_t)(e - auto_pos) : strlen(auto_pos);
    snprintf(tok, sizeof tok, "%.*s", (int)len, auto_pos);
    auto_pos = e ? e + 1 : auto_pos + len;
    if (!strncmp(tok, "wait:", 5)) { auto_wait = atoi(tok + 5); return; }
    if (!strncmp(tok, "shot:", 5)) { snprintf(shot_path, sizeof shot_path, "%s", tok + 5); return; }
    if (!strcmp(tok, "quit")) { auto_quit = true; return; }
    if (!strcmp(tok, "power")) { UI.in.power = true; return; }
    if (!strncmp(tok, "down:", 5) || !strncmp(tok, "up:", 3)) {
        bool d = tok[0] == 'd';
        int b = btn_by_name(tok + (d ? 5 : 3));
        if (b >= 0) {
            UI.in.down[b] = auto_down[b] = d;
            UI.in.pressed[b] = d;
            UI.in.any = true;
            auto_wait = 2;
        }
        return;
    }
    int b = btn_by_name(tok);
    if (b >= 0) {
        UI.in.pressed[b] = UI.in.down[b] = auto_down[b] = true;
        UI.in.any = true;
        auto_release = b;
        auto_wait = 8;
    }
}

void ui_poll(void) {
    memset(UI.in.pressed, 0, sizeof UI.in.pressed);
    UI.in.any = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: UI.in.quit = true; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int b = key_btn(ev.key.keysym.sym);
            if (b >= 0 && !(ev.type == SDL_KEYDOWN && ev.key.repeat)) source_set(kb_down, b, ev.type == SDL_KEYDOWN);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            if (npads < 4) {
                SDL_GameController *gc = SDL_GameControllerOpen(ev.cdevice.which);
                if (!gc) break;
                SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
                if (pad_by_id(id)) break;
                pad_t *pd = &pads[npads++];
                memset(pd, 0, sizeof *pd);
                pd->gc = gc;
                pd->id = id;
                snprintf(pd->name, sizeof pd->name, "%s", SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "Controller");
                /* integrato: per nome del dispositivo, oppure, sulla console, il primo trovato */
                bool other_builtin = false;
                for (int i = 0; i < npads - 1; i++) other_builtin |= pads[i].builtin;
                pd->builtin = is_builtin(pd->name) || device_is_builtin(SDL_GameControllerGetJoystick(gc)) ||
                              (plat_is_device() && !other_builtin);
                pad_announce(pd, true);
            }
            break;
        case SDL_JOYDEVICEADDED:
            if (npads < 4 && !SDL_IsGameController(ev.jdevice.which)) {
                SDL_Joystick *js = SDL_JoystickOpen(ev.jdevice.which);
                if (!js) break;
                SDL_JoystickID id = SDL_JoystickInstanceID(js);
                if (pad_by_id(id)) break;
                static bool map_loaded;
                if (!map_loaded) { joymap_load(); map_loaded = true; }
                pad_t *pd = &pads[npads++];
                memset(pd, 0, sizeof *pd);
                pd->js = js;
                pd->id = id;
                snprintf(pd->name, sizeof pd->name, "%s", SDL_JoystickName(js) ? SDL_JoystickName(js) : "Joystick");
                /* sulla console il primo joystick senza mappatura è quello integrato */
                bool other_builtin = false;
                for (int i = 0; i < npads - 1; i++) other_builtin |= pads[i].builtin;
                pd->builtin = is_builtin(pd->name) || (plat_is_device() && !other_builtin);
                ui_log("joystick: %s (%d pulsanti, %d assi, %d cappelli)", pd->name, SDL_JoystickNumButtons(js), SDL_JoystickNumAxes(js), SDL_JoystickNumHats(js));
                pad_announce(pd, true);
            }
            break;
        case SDL_JOYDEVICEREMOVED: {
            pad_t *pd = pad_by_id(ev.jdevice.which);
            if (!pd || !pd->js) break;
            pad_announce(pd, false);
            SDL_JoystickClose(pd->js);
            int i = (int)(pd - pads);
            memmove(&pads[i], &pads[i + 1], sizeof(pad_t) * (size_t)(npads - i - 1));
            npads--;
            for (int b = 0; b < BTN_COUNT; b++) source_set(NULL, b, false);
            break;
        }
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP: {
            pad_t *pd = pad_by_id(ev.jbutton.which);
            if (!pd || !pd->js) break;
            int b = joy_to_btn(ev.jbutton.button);
            if (getenv("TRIMUI_JOYDEBUG") || ui_debug_on(2)) ui_log("pulsante %d %s", ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN ? "giù" : "su");
            if (b >= 0) source_set(pd->down, b, ev.type == SDL_JOYBUTTONDOWN);
            break;
        }
        case SDL_JOYHATMOTION: {
            pad_t *pd = pad_by_id(ev.jhat.which);
            if (!pd || !pd->js || ev.jhat.hat != 0) break;
            int v = ev.jhat.value;
            bool want[4] = { v & SDL_HAT_UP, v & SDL_HAT_DOWN, v & SDL_HAT_LEFT, v & SDL_HAT_RIGHT };
            for (int d = 0; d < 4; d++)
                if (want[d] != pd->down[BTN_UP + d]) source_set(pd->down, BTN_UP + d, want[d]);
            break;
        }
        case SDL_JOYAXISMOTION: {
            pad_t *pd = pad_by_id(ev.jaxis.which);
            if (!pd || !pd->js) break;
            int a = ev.jaxis.axis, v = ev.jaxis.value;
            if (a == 0 || a == 1) UI.in.axis[a] = v;
            else if (a == 3 || a == 4) UI.in.axis[a - 1] = v;
            if (a == joy_axis_l2 || a == joy_axis_r2) {
                int b = a == joy_axis_l2 ? BTN_L2 : BTN_R2;
                if ((v > 16000) != pd->down[b]) source_set(pd->down, b, v > 16000);
            } else if (a == 0 || a == 1) {
                int lo = a == 0 ? BTN_LEFT : BTN_UP, hi = a == 0 ? BTN_RIGHT : BTN_DOWN;
                bool nlo = v < -20000, nhi = v > 20000;
                if (nlo != pd->stick[lo - BTN_UP]) { pd->stick[lo - BTN_UP] = nlo; source_set(NULL, lo, nlo); }
                if (nhi != pd->stick[hi - BTN_UP]) { pd->stick[hi - BTN_UP] = nhi; source_set(NULL, hi, nhi); }
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            pad_t *pd = pad_by_id(ev.cdevice.which);
            if (!pd || !pd->gc) break;
            pad_announce(pd, false);
            SDL_GameControllerClose(pd->gc);
            int i = (int)(pd - pads);
            memmove(&pads[i], &pads[i + 1], sizeof(pad_t) * (size_t)(npads - i - 1));
            npads--;
            for (int b = 0; b < BTN_COUNT; b++) source_set(NULL, b, false);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            pad_t *pd = pad_by_id(ev.cbutton.which);
            int b = pad_btn(ev.cbutton.button);
            if (getenv("TRIMUI_JOYDEBUG") || ui_debug_on(2))
                ui_log("controller: pulsante SDL %d %s -> %d", ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN ? "giù" : "su", b);
            if (pd && b >= 0) source_set(pd->down, b, ev.type == SDL_CONTROLLERBUTTONDOWN);
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            pad_t *pd = pad_by_id(ev.caxis.which);
            if (!pd) break;
            int a = ev.caxis.axis, v = ev.caxis.value;
            if (a >= SDL_CONTROLLER_AXIS_LEFTX && a <= SDL_CONTROLLER_AXIS_RIGHTY) UI.in.axis[a] = v;
            if ((getenv("TRIMUI_JOYDEBUG") || ui_debug_on(2)) && (v > 16000 || v < -16000)) ui_log("controller: asse SDL %d = %d", a, v);
            if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                int b = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? BTN_L2 : BTN_R2;
                if ((v > 16000) != pd->down[b]) source_set(pd->down, b, v > 16000);
            } else if (a == SDL_CONTROLLER_AXIS_LEFTX || a == SDL_CONTROLLER_AXIS_LEFTY) {
                /* levetta sinistra come croce direzionale */
                int lo = a == SDL_CONTROLLER_AXIS_LEFTX ? BTN_LEFT : BTN_UP, hi = a == SDL_CONTROLLER_AXIS_LEFTX ? BTN_RIGHT : BTN_DOWN;
                bool nlo = v < -20000, nhi = v > 20000;
                if (nlo != pd->stick[lo - BTN_UP]) { pd->stick[lo - BTN_UP] = nlo; source_set(NULL, lo, nlo); }
                if (nhi != pd->stick[hi - BTN_UP]) { pd->stick[hi - BTN_UP] = nhi; source_set(NULL, hi, nhi); }
            }
            break;
        }
        }
    }
    ports_update();
    plat_keys_t pk;
    plat_poll(&pk);
    UI.in.home = pk.home;
    UI.in.f1 = pk.f1;
    UI.in.f2 = pk.f2;
    /* sul firmware originale il tasto di accensione lo gestisce TrimUI (sospensione) */
    UI.in.power = pk.power && plat_standalone();
    UI.in.power_long = pk.power_long && plat_standalone();
    UI.in.fn = pk.fn;
    if (pk.home || pk.f1 || pk.f2) UI.in.any = true;
    if (pk.fn_changed) ui_toast(T(pk.fn == 1 ? "fn.on" : "fn.off"));
    /* volume dai tasti laterali: sul firmware originale li gestisce keymon, nel sistema
       su SD li gestiamo noi */
    UI.in.vol_changed = false;
    /* volume dai tasti: si salva un secondo dopo l'ultima pressione, non a ogni passo */
    if (vol_save_at > 0 && UI.t > vol_save_at) { vol_save_at = 0; settings_save(&UI.set, UI.root); }
    if (pk.vol && plat_standalone()) {
        int v = UI.set.volume + pk.vol;
        UI.set.volume = v < 0 ? 0 : v > 20 ? 20 : v;
        char m[64];
        snprintf(m, sizeof m, "%s %d", T("settings.volume"), UI.set.volume);
        if (!UI.vol_bar) ui_toast(m);
        UI.in.vol_changed = true;
        vol_save_at = UI.t + 1.0;
        UI.in.any = true;
    }
    /* ripetizione automatica sulle direzioni tenute premute */
    for (int b = BTN_UP; b <= BTN_RIGHT; b++)
        if (UI.in.down[b] && UI.t >= next_rep[b] && !UI.in.pressed[b] && hold_t[b] > 0) {
            UI.in.pressed[b] = true;
            next_rep[b] = UI.t + 0.075;
        }
    if (UI.automation) { auto_step(); ports_update(); }   /* i tasti simulati arrivano anche ai giocatori */
    if (auto_quit) UI.in.quit = true;
}

/* -------------------------------------------------------------- toast */

static char toast_msg[160];
static double toast_until;
bool ui_toast_active(void) { return toast_msg[0] && UI.t <= toast_until; }

void ui_toast(const char *msg) {
    snprintf(toast_msg, sizeof toast_msg, "%s", msg);
    toast_until = UI.t + 2.2;
}

void ui_draw_toast(void) {
    if (UI.t > toast_until || !toast_msg[0]) return;
    float a = (float)fmin(1.0, (toast_until - UI.t) / 0.3);
    TTF_Font *f = UI.f_normal;
    int tw = ui_text_w(f, toast_msg), pad = M(0.03f), h = M(0.075f);
    /* in alto, sotto la barra di stato: in basso coprirebbe le etichette delle icone */
    int w = tw + pad * 2, x = (UI.w - w) / 2, y = M(UI.th.topbar_h) - h / 2;
    ui_shadow(x, y + M(0.005f), w, h, h / 2, M(0.02f), with_alpha(UI.th.shadow, (int)(UI.th.shadow.a * a)));
    ui_rrect(x, y, w, h, h / 2, with_alpha(UI.th.text, (int)(235 * a)));
    ui_text(f, toast_msg, x + pad, y + (h - TTF_FontHeight(f)) / 2, with_alpha(UI.th.bg, (int)(255 * a)), ALIGN_LEFT);
}

/* ------------------------------------------------ barra stato e tasti */

static void battery_icon(int x, int y, int w, int h, int pct, bool charging, color_t c) {
    int t = h > 14 ? 2 : 1;
    bool low = pct <= 15 && !charging;
    color_t edge = low ? rgba(230, 60, 60, 255) : c;
    ui_rborder(x, y, w, h, h / 4, t, edge);
    ui_rect(x + w + 1, y + h / 3, t + 1, h / 3, edge);
    int shown = pct;
    if (charging && pct < 100) shown = pct + (int)((100 - pct) * fmod(UI.t, 2.0) / 2.0);   /* in carica: si riempie a ciclo */
    if (pct <= 5 && !charging && fmod(UI.t, 1.0) > 0.5) return;   /* quasi scarica: lampeggia */
    int inner = (w - 4 * t) * shown / 100;
    if (inner > 0) ui_rrect(x + 2 * t, y + 2 * t, inner, h - 4 * t, h / 6, edge);
}

/* level: -1 spento, 0 acceso ma non collegato, 1..3 tacche di segnale */
static void wifi_icon(int x, int y, int h, int level, color_t c) {
    ui_wifi_icon(x, y, h, level, with_alpha(c, level < 0 ? 45 : 255), with_alpha(c, level < 0 ? 45 : 90));
}

/* barra dei tasti su una riga: terne tasto, etichetta, etichetta corta. Se non c'è posto si usano
   le etichette corte, poi si tolgono le voci con etichetta corta "" (le meno importanti, dalla
   fine); il primo tasto resta sempre. */
int ui_keybar_h(void) { return M(0.045f) + M(0.04f); }
int ui_keybar(const char *const *items, int n) {
    theme_t *t = &UI.th;
    TTF_Font *f = UI.f_small;
    int mx = M(t->margin), y0 = UI.h - ui_keybar_h(), g = M(0.042f), avail = UI.w - 2 * mx;
    int cnt = n / 3, cy = y0 + ui_keybar_h() / 2;
    bool keep[16], shrt = false;
    for (int i = 0; i < cnt && i < 16; i++) keep[i] = true;
    /* larghezza dei pulsanti come in ui_hints: tondi per una lettera, a pillola per le parole */
    #define KB_GW(b) (!strcmp(b, "<>") || !strcmp(b, "^v") ? g * 3 / 2 : strlen(b) == 1 ? g : ui_text_w(f, b) + g / 2)
    #define KB_LABEL(i) (shrt && items[(i) * 3 + 2] && items[(i) * 3 + 2][0] ? items[(i) * 3 + 2] : items[(i) * 3 + 1])
    for (int pass = 0; pass < 20; pass++) {
        int tot = 0, k = 0;
        for (int i = 0; i < cnt; i++) {
            if (!keep[i]) continue;
            tot += (k++ ? M(0.03f) : 0) + KB_GW(items[i * 3]) + M(0.012f) + ui_text_w(f, KB_LABEL(i));
        }
        if (tot <= avail) break;
        if (!shrt) { shrt = true; continue; }
        int drop = -1;
        for (int i = cnt - 1; i > 0 && drop < 0; i--) if (keep[i] && items[i * 3 + 2] && !items[i * 3 + 2][0]) drop = i;
        if (drop < 0) for (int i = cnt - 1; i > 0 && drop < 0; i--) if (keep[i]) drop = i;
        if (drop < 0) break;
        keep[drop] = false;
    }
    ui_rect(mx, y0, UI.w - 2 * mx, 1, t->line);
    /* da destra: il primo tasto (l'azione principale) è l'ultimo a destra, come in ui_hints */
    int x = UI.w - mx;
    for (int i = 0; i < cnt; i++) {
        if (!keep[i]) continue;
        const char *btn = items[i * 3];
        x -= ui_text(f, KB_LABEL(i), x, cy - TTF_FontHeight(f) / 2, t->text, ALIGN_RIGHT) + M(0.012f);
        int gw = KB_GW(btn);
        ui_button_glyph(btn, x - gw / 2, cy, g);
        x -= gw + M(0.03f);
    }
    #undef KB_GW
    #undef KB_LABEL
    return x + M(0.03f);   /* dove comincia la barra: a sinistra resta posto libero */
}

void ui_topbar(const char *title) {
    theme_t *t = &UI.th;
    int bh = M(t->topbar_h), mx = M(t->margin), cy = bh / 2;
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    if (title) {
        char tt[128];
        snprintf(tt, sizeof tt, "%s", title);
        if (t->title_upper)
            for (char *p = tt; *p; p++)
                if (*p >= 'a' && *p <= 'z') *p -= 32;
        ui_text(UI.f_title, tt, mx, cy - TTF_FontHeight(UI.f_title) / 2, t->text, ALIGN_LEFT);
    }
    bool charging;
    int pct = ui_battery(&charging);
    char buf[32];
    int x = UI.w - mx;
    if (pct >= 0) {   /* livello sconosciuto: meglio nessuna indicazione che una falsa */
        snprintf(buf, sizeof buf, "%d%%", pct);
        x -= ui_text(f, buf, x, cy - fh / 2, t->text, ALIGN_RIGHT) + M(0.012f);
        int biw = M(0.05f), bih = M(0.026f);
        x -= biw + M(0.006f);
        battery_icon(x, cy - bih / 2, biw, bih, pct, charging, t->text);
    }
    int wh = M(0.026f);
    x -= M(0.03f) + wh;
    int level = UI.set.wifi ? 3 : -1;
    bool waiting = false;
    if (wifi_supported()) {
        const wifi_status_t *ws = wifi_status(false);
        level = !UI.set.wifi ? -1 : ws->state == WIFI_CONNECTED ? (ws->bars ? ws->bars : 1) : 0;
        waiting = UI.set.wifi && ws->state != WIFI_CONNECTED && (ws->state == WIFI_CONNECTING || wifi_starting());
    }
    if (waiting) {   /* sta partendo o si sta collegando: pulsa, per distinguerlo da "nessuna rete" */
        color_t pulse = with_alpha(t->text, 60 + (int)(195 * (0.5 + 0.5 * sin(UI.t * 4))));
        ui_wifi_icon(x, cy - wh / 2, wh, 3, pulse, pulse);
    } else wifi_icon(x, cy - wh / 2, wh, level, t->text);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    x -= M(0.03f);
    ui_text(f, buf, x, cy - fh / 2, t->text, ALIGN_RIGHT);
}

void ui_button_glyph(const char *btn, int cx, int cy, int size) {
    theme_t *t = &UI.th;
    TTF_Font *f = UI.f_small;
    if (!strcmp(btn, "^v")) {
        /* frecce su/giù */
        int w = size * 3 / 2, a = size / 5;
        ui_rrect(cx - w / 2, cy - size / 2, w, size, size / 2, t->text);
        ui_triangle(cx - a * 11 / 5, cy + a * 3 / 5, cx - a * 6 / 5, cy - a * 4 / 5, cx - a / 5, cy + a * 3 / 5, t->bg);
        ui_triangle(cx + a / 5, cy - a * 3 / 5, cx + a * 11 / 5, cy - a * 3 / 5, cx + a * 6 / 5, cy + a * 4 / 5, t->bg);
    } else if (!strcmp(btn, "<>")) {
        /* frecce sinistra/destra disegnate: molti font non hanno i simboli */
        int w = size * 3 / 2, a = size / 5;
        ui_rrect(cx - w / 2, cy - size / 2, w, size, size / 2, t->text);
        ui_triangle(cx - a / 2 - a * 3 / 2, cy, cx - a / 2, cy - a, cx - a / 2, cy + a, t->bg);
        ui_triangle(cx + a / 2 + a * 3 / 2, cy, cx + a / 2, cy - a, cx + a / 2, cy + a, t->bg);
    } else if (strlen(btn) == 1) {
        ui_circle(cx, cy, size / 2, t->text);
        ui_text(f, btn, cx, cy - TTF_FontHeight(f) / 2, t->bg, ALIGN_CENTER);
    } else {
        int w = ui_text_w(f, btn) + size / 2;
        ui_rrect(cx - w / 2, cy - size / 2, w, size, size / 2, t->text);
        ui_text(f, btn, cx, cy - TTF_FontHeight(f) / 2, t->bg, ALIGN_CENTER);
    }
}

void ui_hints(const char *const *items, int n) {
    theme_t *t = &UI.th;
    int bh = M(t->hintbar_h), y0 = UI.h - bh, mx = M(t->margin), cy = y0 + bh / 2, g = M(0.042f);
    ui_rect(mx / 2, y0, UI.w - mx, 1, t->line);
    TTF_Font *f = UI.f_small;
    int x = UI.w - mx;
    for (int i = n - 2; i >= 0; i -= 2) {
        const char *btn = items[i], *label = items[i + 1];
        x -= ui_text(f, label, x, cy - TTF_FontHeight(f) / 2, t->text, ALIGN_RIGHT) + M(0.012f);
        int gw = !strcmp(btn, "<>") || !strcmp(btn, "^v") ? g * 3 / 2 : strlen(btn) == 1 ? g : ui_text_w(f, btn) + g / 2;
        ui_button_glyph(btn, x - gw / 2, cy, g);
        x -= gw + M(0.035f);
    }
}

float ui_approach(float v, float target) {
    float k = 1 - expf(-(float)UI.dt * UI.th.anim_speed);
    v += (target - v) * k;
    if (fabsf(target - v) < 0.01f) v = target;
    return v;
}

/* ------------------------------------------------------- ciclo frame */

static Uint64 t0, tprev;

void ui_apply_device(void) {
    const char *model = plat_model();   /* sulla console il modello è quello vero */
    if (model) snprintf(UI.set.device, sizeof UI.set.device, "%s", model);
    device_size(UI.set.device, &UI.w, &UI.h);
    if (UI.canvas) SDL_DestroyTexture(UI.canvas);
    UI.canvas = SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, UI.w, UI.h);
    SDL_SetTextureScaleMode(UI.canvas, SDL_ScaleModeLinear);
    if (UI.win && !UI.automation && !plat_is_device()) {
        SDL_Rect usable;
        int ww = UI.w, wh = UI.h;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            float s = fminf(1.0f, fminf((float)usable.w / ww, (float)(usable.h - 40) / wh));
            ww = (int)(ww * s);
            wh = (int)(wh * s);
        }
        SDL_SetWindowSize(UI.win, ww, wh);
        SDL_SetWindowPosition(UI.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    ui_reload_theme();
}

bool ui_init(const char *app, const char *root, const char *auto_script, int audio_rate, bool vsync) {
    memset(&UI, 0, sizeof UI);
    snprintf(UI.root, sizeof UI.root, "%s", root);
    char p[1200];
    snprintf(p, sizeof p, "%s/Logs", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Logs/%s.log", root, app);
    snprintf(log_path, sizeof log_path, "%s", p);
    crash_handlers();
    settings_load(&UI.set, root);
    ui_log("---- avvio %s", app);
    ui_mark("inizio");
    settings_apply_tz(&UI.set);
    if (plat_standalone()) settings_save_tz_for_boot(&UI.set, root);
    lang_load(root, UI.set.lang);
    UI.automation = auto_script && *auto_script;
    if (UI.automation) {
        snprintf(auto_buf, sizeof auto_buf, "%s", auto_script);
        auto_pos = auto_buf;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        ui_log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    ui_mark("SDL_Init");
    TTF_Init();
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    /* Sulla console la finestra ha la misura del modello, come in MinUI: lo schermo della
       Smart Pro è fisicamente verticale (720x1280) e la rotazione la fa l'SDL2 di TrimUI,
       quindi la modalità video riportata dal sistema non va usata. */
    Uint32 wflags = plat_is_device() ? SDL_WINDOW_SHOWN : SDL_WINDOW_RESIZABLE | (UI.automation ? SDL_WINDOW_HIDDEN : 0);
    int dw = 1280, dh = 720;
    if (plat_is_device()) {
        const char *model = plat_model();
        device_size(model ? model : UI.set.device, &dw, &dh);
        SDL_ShowCursor(SDL_DISABLE);
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0) ui_log("modalità video del sistema: %dx%d", dm.w, dm.h);
    }
    UI.win = SDL_CreateWindow(app, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dw, dh, wflags);
    UI.ren = SDL_CreateRenderer(UI.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE |
                                                 (vsync && !UI.automation ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (UI.win && !UI.ren) {
        /* senza GPU (driver mancante o prove in un container) si ripiega sul disegno software */
        ui_log("renderer accelerato non disponibile (%s): uso quello software", SDL_GetError());
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        UI.ren = SDL_CreateRenderer(UI.win, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    }
    ui_mark("finestra e renderer");
    if (UI.ren) {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(UI.ren, &ri);
        ui_log("renderer: %s", ri.name);
    }
    if (!UI.win || !UI.ren) {
        ui_log("finestra/renderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
    UI.audio_rate = audio_rate > 0 ? audio_rate : 48000;
    audio_open();
    ui_mark("audio");
    ui_apply_device();
    ui_mark("tema e caratteri");
    t0 = tprev = SDL_GetPerformanceCounter();
    return true;
}

/* passo della chiusura: sulla console va scritto subito, se la chiusura si blocca il log
   deve dire dove */
static void shut_step(const char *what) {
    if (!plat_is_device()) return;
    ui_log("chiusura: %s", what);
    if (logf_) fsync(fileno(logf_));
}

static void write_exit_code(int code) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/exit.txt", UI.root);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", code); fflush(f); fsync(fileno(f)); fclose(f); }
}

/* Riavvio dopo un aggiornamento: la chiusura dell'audio lì si blocca e bisogna aspettare che lo
   script chiuda il programma. Tanto la console si riavvia: si esce subito, senza chiudere SDL. */
void ui_exit_now(int code) {
    plat_cpu_release(&UI.set);
    write_exit_code(code);
    ui_log("chiusura immediata (%d)", code);
    if (logf_) fclose(logf_);
    logf_ = NULL;
    _exit(code);
}

void ui_shutdown_code(int code) {
    plat_cpu_release(&UI.set);
    if (vol_save_at > 0) { vol_save_at = 0; settings_save(&UI.set, UI.root); }
    /* System/exit.txt: chi ci ha avviati sa già come siamo usciti, anche se la chiusura
       dovesse bloccarsi (sul sistema su SD lo script ci chiude dopo qualche secondo) */
    if (code >= 0) write_exit_code(code);
    shut_step("font");
    fonts_clear();
    shut_step("audio");
    if (UI.audio) SDL_CloseAudioDevice(UI.audio);
    shut_step("renderer");
    if (UI.canvas) SDL_DestroyTexture(UI.canvas);
    SDL_DestroyRenderer(UI.ren);
    shut_step("finestra");
    SDL_DestroyWindow(UI.win);
    TTF_Quit();
    IMG_Quit();
    shut_step("SDL_Quit");
    SDL_Quit();
    shut_step("fatto");
    if (logf_) fclose(logf_);
    logf_ = NULL;
    log_path[0] = 0;
}

void ui_shutdown(void) { ui_shutdown_code(-1); }

/* Onde animate di sfondo: tre fasce sinusoidali sfumate, colore del testo a bassa opacità. */
static void draw_waves(void) {
    enum { N = 64 };
    SDL_Vertex v[(N + 1) * 3];
    int idx[N * 12];
    color_t c = UI.th.text;
    float t = (float)UI.t;
    for (int b = 0; b < 3; b++) {
        float base = UI.h * (0.58f + 0.06f * b), amp = UI.h * (0.05f + 0.02f * b);
        float freq = 2 * (float)M_PI / (UI.w * (1.1f + 0.35f * b)), speed = 0.25f + 0.12f * b;
        int k = 0;
        for (int i = 0; i <= N; i++) {
            float x = (float)UI.w * i / N;
            float y = base + amp * sinf(x * freq + t * speed + b * 2.1f);
            float th = UI.h * (0.035f + 0.02f * sinf(x * freq * 0.7f + t * 0.3f + b));
            /* tre vertici per colonna: bordo superiore trasparente, centro visibile, bordo inferiore trasparente */
            v[i * 3] = (SDL_Vertex){ { x, y - th }, { c.r, c.g, c.b, 0 }, { 0, 0 } };
            v[i * 3 + 1] = (SDL_Vertex){ { x, y }, { c.r, c.g, c.b, (Uint8)(26 - 6 * b) }, { 0, 0 } };
            v[i * 3 + 2] = (SDL_Vertex){ { x, y + th * 2.5f }, { c.r, c.g, c.b, 0 }, { 0, 0 } };
        }
        for (int i = 0; i < N; i++)
            for (int r = 0; r < 2; r++) {
                int a = i * 3 + r, d = (i + 1) * 3 + r;
                idx[k++] = a; idx[k++] = d; idx[k++] = a + 1;
                idx[k++] = a + 1; idx[k++] = d; idx[k++] = d + 1;
            }
        SDL_RenderGeometry(UI.ren, NULL, v, (N + 1) * 3, idx, k);
    }
}

static SDL_Surface *offscreen;

bool ui_init_offscreen(const char *app, const char *root) {
    memset(&UI, 0, sizeof UI);
    snprintf(UI.root, sizeof UI.root, "%s", root);
    char p[1200];
    snprintf(p, sizeof p, "%s/Logs/%s.log", root, app);
    snprintf(log_path, sizeof log_path, "%s", p);
    settings_load(&UI.set, root);
    ui_log("---- avvio %s (fuori schermo)", app);
    settings_apply_tz(&UI.set);
    lang_load(root, UI.set.lang);
    /* solo tasti: lo schermo e l'audio restano al gioco */
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        ui_log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    TTF_Init();
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    const char *model = plat_model();
    int w, h;
    device_size(model ? model : UI.set.device, &w, &h);
    offscreen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    UI.ren = offscreen ? SDL_CreateSoftwareRenderer(offscreen) : NULL;
    if (!UI.ren) {
        ui_log("renderer software: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
    ui_apply_device();
    t0 = tprev = SDL_GetPerformanceCounter();
    return true;
}

void ui_clock(void) {
    UI.t = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
}

void ui_overlay_begin(bool clear) {
    Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
    UI.dt = (double)(now - tprev) / (double)freq;
    if (UI.dt > 0.1) UI.dt = 0.1;
    UI.t = (double)(now - t0) / (double)freq;
    tprev = now;
    frame_no++;
    SDL_SetRenderTarget(UI.ren, UI.canvas);
    if (!clear) return;   /* si disegna sopra l'immagine precedente */
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, 0);
    SDL_RenderClear(UI.ren);
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
}

SDL_Surface *ui_overlay_end(void) {
    ui_draw_toast();
    /* l'immagine passa così com'è, trasparenza compresa */
    SDL_SetRenderTarget(UI.ren, NULL);
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, 0);
    SDL_RenderClear(UI.ren);
    SDL_SetTextureBlendMode(UI.canvas, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(UI.ren, UI.canvas, NULL, NULL);
    SDL_SetTextureBlendMode(UI.canvas, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawBlendMode(UI.ren, SDL_BLENDMODE_BLEND);
    SDL_RenderPresent(UI.ren);
    if (shot_path[0]) {
        IMG_SavePNG(offscreen, shot_path);
        ui_log("screenshot: %s", shot_path);
        shot_path[0] = 0;
    }
    return offscreen;
}

void ui_begin(void) {
    Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
    UI.dt = UI.automation ? 1.0 / 60 : (double)(now - tprev) / (double)freq;
    if (UI.dt > 0.1) UI.dt = 0.1;
    UI.t = UI.automation ? UI.t + UI.dt : (double)(now - t0) / (double)freq;
    tprev = now;
    frame_no++;
    SDL_SetRenderTarget(UI.ren, UI.canvas);
    ui_background();
}

/* sfondo del tema: colore o sfumatura, immagine, onde */
void ui_background(void) {
    theme_t *t = &UI.th;
    if (t->bg2.r != t->bg.r || t->bg2.g != t->bg.g || t->bg2.b != t->bg.b) ui_vgradient(0, 0, UI.w, UI.h, t->bg, t->bg2);
    else {
        SDL_SetRenderDrawColor(UI.ren, t->bg.r, t->bg.g, t->bg.b, 255);
        SDL_RenderClear(UI.ren);
    }
    if (t->bg_image[0]) {
        SDL_Texture *img = ui_image(t->bg_image);
        if (img) SDL_RenderCopy(UI.ren, img, NULL, NULL);
    }
    if (t->waves) draw_waves();
}

void ui_end(void) {
    ui_draw_toast();
    if (UI.th.scanlines) {
        for (int y = 0; y < UI.h; y += 3) ui_rect(0, y, UI.w, 1, rgba(0, 0, 0, 38));
    }
    if (shot_path[0]) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, UI.w, UI.h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (SDL_RenderReadPixels(UI.ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
            IMG_SavePNG(s, shot_path);
            ui_log("screenshot: %s", shot_path);
        } else {
            ui_log("screenshot fallito: %s", SDL_GetError());
        }
        SDL_FreeSurface(s);
        shot_path[0] = 0;
    }
    SDL_SetRenderTarget(UI.ren, NULL);
    SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, 255);
    SDL_RenderClear(UI.ren);
    int ow, oh;
    SDL_GetRendererOutputSize(UI.ren, &ow, &oh);
    float s = fminf((float)ow / UI.w, (float)oh / UI.h);
    SDL_Rect dst = { (int)((ow - UI.w * s) / 2), (int)((oh - UI.h * s) / 2), (int)(UI.w * s), (int)(UI.h * s) };
    SDL_RenderCopy(UI.ren, UI.canvas, NULL, &dst);
    /* luminosità: sulla console agisce la retroilluminazione, sul Mac si scurisce l'immagine */
    plat_led_theme((unsigned)UI.th.accent.r << 16 | UI.th.accent.g << 8 | UI.th.accent.b);
    plat_apply(&UI.set);
    int dark = plat_is_device() ? 0 : (10 - UI.set.brightness) * 19;
    if (dark > 0) {
        SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, (Uint8)dark);
        SDL_RenderFillRect(UI.ren, &dst);
    }
    SDL_RenderPresent(UI.ren);
}

/* i tasti di ui.h (BTN_A, BTN_LEFT...) non devono essere coperti da <linux/input.h> */
#ifdef BTN_TRIGGER_HAPPY
#error "linux/input.h incluso: coprirebbe i tasti di ui.h"
#endif
