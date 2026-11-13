/* Prova di un core libretro compilato per la console, senza console: carica il .so, il gioco,
   fa girare N fotogrammi e salva l'ultimo come PPM. Serve a vedere che un core nuovo parte e
   disegna davvero, su arm64 (Debian in Docker), prima di provarlo sulla console.
     gcc -O1 -I frontend/src/runner -o coretest toolchain/coretest.c -ldl
     ./coretest core.so gioco cartella_di_sistema fotogrammi uscita.ppm [tasto@fotogramma ...]
   tasto: indice RETRO_DEVICE_ID_JOYPAD (0 B, 3 Start, 8 A...), premuto per 10 fotogrammi. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const char *sysdir;
static enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t *last;
static unsigned lw, lh;
static int frame, press_btn[16], press_at[16], npress, nvar, audio;
static bool want_kbd;

/* opzioni del core: come il runner, si risponde con il valore predefinito di ognuna */
static struct { const char *key, *val; } opts[600];
static int nopts;
static void opt_add(const char *k, const char *v) { if (k && v && nopts < 600) { opts[nopts].key = k; opts[nopts].val = v; nopts++; } }
static void opts_v2(const struct retro_core_options_v2 *o) {
    for (const struct retro_core_option_v2_definition *d = o ? o->definitions : NULL; d && d->key; d++)
        opt_add(d->key, d->default_value ? d->default_value : d->values[0].value);
}

static void logcb(enum retro_log_level l, const char *f, ...) { va_list a; va_start(a, f); if (l >= RETRO_LOG_WARN) vfprintf(stderr, f, a); va_end(a); }
static bool env(unsigned cmd, void *d) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: *(const char **)d = sysdir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt = *(enum retro_pixel_format *)d; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)d)->log = logcb; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)d = 2; return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: opts_v2(d); return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: opts_v2(((struct retro_core_options_v2_intl *)d)->us); return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = d;
        nvar++;
        /* CORETEST_OPTS="chiave=valore,chiave=valore" cambia i predefiniti */
        const char *ov = getenv("CORETEST_OPTS");
        static char buf[100];
        size_t kl = strlen(v->key);
        for (const char *p = ov; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
            if (!strncmp(p, v->key, kl) && p[kl] == '=') {
                snprintf(buf, sizeof buf, "%.*s", (int)strcspn(p + kl + 1, ","), p + kl + 1);
                v->value = buf;
                return true;
            }
        for (int i = 0; i < nopts; i++) if (!strcmp(opts[i].key, v->key)) { v->value = opts[i].val; return true; }
        v->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)d = false; return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: want_kbd = true; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)d = true; return true;
    default: return false;
    }
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    last = realloc(last, (size_t)w * h * 3);
    lw = w; lh = h;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        uint8_t *o = last + ((size_t)y * w + x) * 3;
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) { uint32_t p = ((const uint32_t *)row)[x]; o[0] = p >> 16; o[1] = p >> 8; o[2] = p; }
        else if (fmt == RETRO_PIXEL_FORMAT_RGB565) { uint16_t p = ((const uint16_t *)row)[x]; o[0] = (p >> 11) << 3; o[1] = ((p >> 5) & 63) << 2; o[2] = (p & 31) << 3; }
        else { uint16_t p = ((const uint16_t *)row)[x]; o[0] = ((p >> 10) & 31) << 3; o[1] = ((p >> 5) & 31) << 3; o[2] = (p & 31) << 3; }
    }
}
static void apoll(void) {}
static int16_t istate(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)idx;
    if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
    for (int i = 0; i < npress; i++) if ((int)id == press_btn[i] && frame >= press_at[i] && frame < press_at[i] + 10) return 1;
    return 0;
}
static void asample(int16_t l, int16_t r) { (void)l; (void)r; audio++; }
static size_t abatch(const int16_t *d, size_t n) { (void)d; audio += (int)n; return n; }

int main(int c, char **v) {
    if (c < 6) { fprintf(stderr, "uso: coretest core.so gioco sistema fotogrammi uscita.ppm [tasto@fotogramma ...]\n"); return 2; }
    sysdir = v[3];
    int frames = atoi(v[4]);
    for (int i = 6; i < c && npress < 16; i++, npress++) sscanf(v[i], "%d@%d", &press_btn[npress], &press_at[npress]);
    void *h = dlopen(v[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define S(t, n) t = dlsym(h, n); if (!t) { fprintf(stderr, "manca %s\n", n); return 1; }
    void (*set_env)(retro_environment_t), (*set_video)(retro_video_refresh_t), (*set_poll)(retro_input_poll_t), (*set_state)(retro_input_state_t);
    void (*set_as)(retro_audio_sample_t), (*set_ab)(retro_audio_sample_batch_t), (*init)(void), (*run)(void), (*unload)(void), (*deinit)(void);
    void (*sysinfo)(struct retro_system_info *), (*avinfo)(struct retro_system_av_info *);
    bool (*load)(const struct retro_game_info *);
    size_t (*sersize)(void);
    S(set_env, "retro_set_environment") S(set_video, "retro_set_video_refresh") S(set_poll, "retro_set_input_poll") S(set_state, "retro_set_input_state")
    S(set_as, "retro_set_audio_sample") S(set_ab, "retro_set_audio_sample_batch") S(init, "retro_init") S(run, "retro_run") S(unload, "retro_unload_game")
    S(deinit, "retro_deinit") S(sysinfo, "retro_get_system_info") S(avinfo, "retro_get_system_av_info") S(load, "retro_load_game") S(sersize, "retro_serialize_size")
    set_env(env); set_video(video); set_poll(apoll); set_state(istate); set_as(asample); set_ab(abatch);
    init();
    struct retro_system_info si = { 0 };
    sysinfo(&si);
    struct retro_game_info gi = { .path = v[2] };
    void *buf = NULL;
    if (!si.need_fullpath) {
        FILE *f = fopen(v[2], "rb");
        if (!f) { perror(v[2]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
        buf = malloc((size_t)n); gi.size = fread(buf, 1, (size_t)n, f); gi.data = buf; fclose(f);
    }
    if (getenv("CORETEST_TRACE")) fprintf(stderr, "carico il gioco\n");
    if (!load(&gi)) { fprintf(stderr, "retro_load_game: rifiutato\n"); return 1; }
    struct retro_system_av_info av = { 0 };
    avinfo(&av);
    for (frame = 0; frame < frames; frame++) {
        if (getenv("CORETEST_TRACE") && frame % 50 == 0) fprintf(stderr, "fotogramma %d\n", frame);
        run();
    }
    printf("%s %s · %ux%u · %.2f fps · audio %s · stati %s · tastiera %s · estensioni %s\n", si.library_name, si.library_version, lw, lh,
           av.timing.fps, audio ? "sì" : "no", sersize() ? "sì" : "no", want_kbd ? "chiesta" : "no", si.valid_extensions);
    if (!last) { fprintf(stderr, "nessun fotogramma\n"); return 1; }
    FILE *o = fopen(v[5], "wb");
    fprintf(o, "P6\n%u %u\n255\n", lw, lh);
    fwrite(last, 1, (size_t)lw * lh * 3, o);
    fclose(o);
    fflush(stdout);
    if (getenv("CORETEST_TRACE")) fprintf(stderr, "chiudo il gioco\n");
    unload();
    if (getenv("CORETEST_TRACE")) fprintf(stderr, "gioco chiuso, chiudo il core\n");
    deinit();
    if (getenv("CORETEST_TRACE")) fprintf(stderr, "core chiuso\n");
    return 0;
}
