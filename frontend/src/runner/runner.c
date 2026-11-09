/* Runner libretro: esegue un core (FBNeo, mame2003-plus) con una ROM.
   Esce con 0 per tornare al launcher, 20 per spegnere la console. */
#include "../common/ui.h"
#include "../common/platform.h"
#include "libretro.h"
#include "glshader.h"
#include "ratectl.h"
#include "../common/filters.h"
#include "../common/json.h"
#include <SDL_image.h>
#include <dirent.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum { EXIT_MENU = 0, EXIT_POWEROFF = 20 };

static struct {
    void *h;
    void (*init)(void);
    void (*deinit)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
} core;

static char root[1024], core_path[1024], rom_path[1024], title[256], core_id[64], rom_name[128];
static char sys_dir[1100], save_dir[1100];
static struct retro_system_av_info av;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned rotation;
static SDL_Texture *frame_tex;
static int tex_w, tex_h, frame_w, frame_h;
static bool have_frame;
static void *rom_data;

/* ---------------------------------------------------------- opzioni core */
/* Il core dichiara le opzioni come "Descrizione; valore1|valore2|...": il primo è il
   predefinito. Valgono poi, in ordine, System/core-options.txt (per tutti i giochi) e
   Saves/<core>/<rom>.opt, scritto dal menu "Opzioni del gioco". FBNeo espone qui anche
   i DIP switch del gioco e l'accesso al menu di servizio. */

typedef struct {
    char key[96], desc[128], value[96];
    char *raw;           /* copia dei valori separati da '\0' */
    const char *vals[96];
    int nvals;
} coreopt_t;

static coreopt_t copts[256];
static int ncopts;
static bool copts_dirty;

static coreopt_t *copt_find(const char *key) {
    for (int i = 0; i < ncopts; i++)
        if (!strcmp(copts[i].key, key)) return &copts[i];
    return NULL;
}

static void copt_declare(const char *key, const char *spec) {
    coreopt_t *o = copt_find(key);
    if (!o) {
        if (ncopts == 256) return;
        o = &copts[ncopts++];
        memset(o, 0, sizeof *o);
        snprintf(o->key, sizeof o->key, "%s", key);
    }
    const char *semi = strchr(spec, ';');
    snprintf(o->desc, sizeof o->desc, "%.*s", semi ? (int)(semi - spec) : (int)strlen(spec), spec);
    free(o->raw);
    o->raw = strdup(semi ? semi + 1 : "");
    o->nvals = 0;
    char *p = o->raw;
    while (*p == ' ') p++;
    while (*p && o->nvals < 96) {
        o->vals[o->nvals++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0;
        p = bar + 1;
    }
    if (o->nvals && !o->value[0]) snprintf(o->value, sizeof o->value, "%s", o->vals[0]);
}

static void opts_load_file(const char *path) {
    char line[320];
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && e[-1] == ' '; ) *--e = 0;
        while (*v == ' ' || *v == '"') v++;
        v[strcspn(v, "\"\r\n")] = 0;
        coreopt_t *o = copt_find(k);
        if (o) snprintf(o->value, sizeof o->value, "%s", v);
    }
    fclose(f);
}

static void game_file(char *out, size_t n, const char *ext) {
    snprintf(out, n, "%s/%s.%s", save_dir, rom_name, ext);
}

static void opts_load_overrides(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/System/core-options.txt", root);
    opts_load_file(p);
    game_file(p, sizeof p, "opt");
    opts_load_file(p);
}

/* nel file del gioco finiscono solo le opzioni diverse dal predefinito */
static void opts_save_game(void) {
    char p[1200];
    game_file(p, sizeof p, "opt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    int n = 0;
    for (int i = 0; i < ncopts; i++)
        if (copts[i].nvals && strcmp(copts[i].value, copts[i].vals[0])) {
            fprintf(f, "%s = \"%s\"\n", copts[i].key, copts[i].value);
            n++;
        }
    fclose(f);
    if (!n) remove(p);
}

static void copt_cycle(coreopt_t *o, int dir) {
    if (o->nvals < 2) return;
    int cur = 0;
    for (int i = 0; i < o->nvals; i++)
        if (!strcmp(o->vals[i], o->value)) cur = i;
    snprintf(o->value, sizeof o->value, "%s", o->vals[(cur + dir + o->nvals) % o->nvals]);
    copts_dirty = true;
    opts_save_game();
}

/* ----------------------------------------------- impostazioni del gioco */
/* Saves/<core>/<rom>.cfg: proporzioni, filtro, rotazione e tasti di questo gioco.
   Senza file valgono proporzioni e filtro scelti nelle impostazioni generali. */

enum { ROT_NONE = 0, ROT_LEFT = 1, ROT_RIGHT = 3 };
static const int rot_values[3] = { ROT_NONE, ROT_LEFT, ROT_RIGHT };

/* tasto della console per ogni tasto RetroPad; -1 = nessuno */
static const int default_map[16] = {
    [RETRO_DEVICE_ID_JOYPAD_B] = BTN_B, [RETRO_DEVICE_ID_JOYPAD_Y] = BTN_Y,
    [RETRO_DEVICE_ID_JOYPAD_SELECT] = BTN_SELECT, [RETRO_DEVICE_ID_JOYPAD_START] = BTN_START,
    [RETRO_DEVICE_ID_JOYPAD_UP] = BTN_UP, [RETRO_DEVICE_ID_JOYPAD_DOWN] = BTN_DOWN,
    [RETRO_DEVICE_ID_JOYPAD_LEFT] = BTN_LEFT, [RETRO_DEVICE_ID_JOYPAD_RIGHT] = BTN_RIGHT,
    [RETRO_DEVICE_ID_JOYPAD_A] = BTN_A, [RETRO_DEVICE_ID_JOYPAD_X] = BTN_X,
    [RETRO_DEVICE_ID_JOYPAD_L] = BTN_L, [RETRO_DEVICE_ID_JOYPAD_R] = BTN_R,
    [RETRO_DEVICE_ID_JOYPAD_L2] = BTN_L2, [RETRO_DEVICE_ID_JOYPAD_R2] = BTN_R2,
    [RETRO_DEVICE_ID_JOYPAD_L3] = -1, [RETRO_DEVICE_ID_JOYPAD_R3] = -1,
};

static struct {
    int aspect, rotate, bezel, cpu, map[16];   /* cpu: 0 automatico, 1 minimo, 2 risparmio, 3 bilanciato, 4 prestazioni */
    char filter[32];
    char sp_name[32][48];   /* parametri shader cambiati dal menu */
    float sp_val[32];
    int nsp;
    int slot;   /* slot degli stati in uso: 0..2 */
} gcfg;

static void gamecfg_load(void) {
    gcfg.aspect = UI.set.aspect;
    snprintf(gcfg.filter, sizeof gcfg.filter, "%s", UI.set.filter);
    gcfg.bezel = UI.set.bezel;
    gcfg.rotate = ROT_NONE;
    gcfg.cpu = 0;
    gcfg.slot = 0;
    memcpy(gcfg.map, default_map, sizeof gcfg.map);
    char p[1200], line[128];
    game_file(p, sizeof p, "cfg");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int id, v;
        if (sscanf(line, "aspect=%d", &v) == 1) { if (v >= 0 && v < 3) gcfg.aspect = v; }
        else if (!strncmp(line, "filter=", 7)) {
            char id[32];
            if (sscanf(line + 7, "%31s", id) == 1)
                snprintf(gcfg.filter, sizeof gcfg.filter, "%s", id[0] >= '0' && id[0] <= '9' ? filter_legacy_id(atoi(id)) : id);
        }
        else if (sscanf(line, "bezel=%d", &v) == 1) gcfg.bezel = v != 0;
        else if (!strncmp(line, "sp.", 3) && gcfg.nsp < 32) {
            char *eq = strchr(line, '=');
            if (eq && eq - line - 3 < 48) {
                snprintf(gcfg.sp_name[gcfg.nsp], sizeof gcfg.sp_name[0], "%.*s", (int)(eq - line - 3), line + 3);
                gcfg.sp_val[gcfg.nsp++] = (float)atof(eq + 1);
            }
        }
        else if (sscanf(line, "rotate=%d", &v) == 1) gcfg.rotate = v & 3;
        else if (sscanf(line, "cpu=%d", &v) == 1) gcfg.cpu = v >= 0 && v <= 4 ? v : 0;
        else if (sscanf(line, "slot=%d", &v) == 1) gcfg.slot = v >= 0 && v <= 2 ? v : 0;
        else if (sscanf(line, "map.%d=%d", &id, &v) == 2 && id >= 0 && id < 16 && v >= -1 && v < BTN_COUNT) gcfg.map[id] = v;
    }
    fclose(f);
}

/* profilo predefinito dell'emulatore: voce "cpu" in Emulators/<id>.json (0-4, come gcfg.cpu) */
static int emu_cpu;
static int emulator_cpu(void) {
    char base[256], dir[1200], p[1600];
    const char *s = strrchr(core_path, '/');
    snprintf(base, sizeof base, "%s", s ? s + 1 : core_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(dir, sizeof dir, "%s/Emulators", root);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int v = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json") || e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        jval *j = json_load(p);
        if (j && !strcmp(json_str(j, "core", ""), base)) v = json_int(j, "cpu", 0);
        json_free(j);
    }
    closedir(d);
    return v >= 0 && v <= 4 ? v : 0;
}

/* frequenza per questo gioco: la sua scelta, poi quella dell'emulatore, poi il profilo generale */
static void apply_cpu(void) {
    static const int khz[5] = { 0, 816000, 1200000, 1608000, 2000000 };
    plat_cpu_game(khz[gcfg.cpu ? gcfg.cpu : emu_cpu]);
}

static void gamecfg_save(void) {
    char p[1200];
    game_file(p, sizeof p, "cfg");
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "aspect=%d\nfilter=%s\nbezel=%d\nrotate=%d\ncpu=%d\n", gcfg.aspect, gcfg.filter, gcfg.bezel, gcfg.rotate, gcfg.cpu);
    fprintf(f, "slot=%d\n", gcfg.slot);
    for (int i = 0; i < 16; i++)
        if (gcfg.map[i] != default_map[i]) fprintf(f, "map.%d=%d\n", i, gcfg.map[i]);
    for (int i = 0; i < glsh_param_count(); i++) {
        glsh_param_t *q = glsh_param(i);
        if (q->value != q->def) fprintf(f, "sp.%s=%g\n", q->name, q->value);
    }
    fclose(f);
}

/* ------------------------------------------------------------ callback */

/* ultimo errore del core: il launcher lo mostra nella diagnosi, anche con il registro spento */
static char core_last_error[256];

static void note_error(const char *s) {
    snprintf(core_last_error, sizeof core_last_error, "%s", s);
    for (char *c = core_last_error; *c; c++)
        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
}

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    if (level < RETRO_LOG_WARN) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg[strcspn(msg, "\n")] = 0;
    ui_log("[core] %s", msg);
    if (level >= RETRO_LOG_ERROR) note_error(msg);
}

static char lang_code[8] = "it";

static unsigned retro_language(void) {
    static const struct { const char *code; unsigned id; } map[] = {
        { "en", RETRO_LANGUAGE_ENGLISH }, { "it", RETRO_LANGUAGE_ITALIAN }, { "fr", RETRO_LANGUAGE_FRENCH },
        { "es", RETRO_LANGUAGE_SPANISH }, { "de", RETRO_LANGUAGE_GERMAN }, { "pt", RETRO_LANGUAGE_PORTUGUESE_PORTUGAL },
        { "ja", RETRO_LANGUAGE_JAPANESE }, { "ru", RETRO_LANGUAGE_RUSSIAN },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i].code, lang_code)) return map[i].id;
    return RETRO_LANGUAGE_ENGLISH;
}

/* nomi dei comandi dichiarati dal core per la porta 1 ("Light Punch", "Coin"...) */
static char input_desc[16][64];

static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = sys_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f > RETRO_PIXEL_FORMAT_RGB565) return false;
        pixfmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_ROTATION:
        rotation = *(const unsigned *)data & 3;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        for (const struct retro_variable *v = data; v && v->key; v++) copt_declare(v->key, v->value ? v->value : "");
        opts_load_overrides();
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        coreopt_t *o = copt_find(v->key);
        v->value = o ? o->value : NULL;
        return o != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = copts_dirty;
        copts_dirty = false;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        memset(input_desc, 0, sizeof input_desc);
        for (const struct retro_input_descriptor *d = data; d && d->description; d++)
            if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD && d->index == 0 && d->id < 16)
                snprintf(input_desc[d->id], sizeof input_desc[0], "%s", d->description);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *g = data;
        av.geometry.base_width = g->base_width;
        av.geometry.base_height = g->base_height;
        av.geometry.aspect_ratio = g->aspect_ratio;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av = *(const struct retro_system_av_info *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = retro_language();
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true;
    default:
        return false;
    }
}

/* diagnosi del video: primo fotogramma ed errori (solo le prime volte, per non riempire il log) */
static int video_errors;
static bool video_first_logged;

static void video_error(const char *what) {
    if (video_errors++ < 5) ui_log("video: %s non riuscito: %s", what, SDL_GetError());
}

/* Miniatura per le anteprime degli stati: a gioco fermo nel menu il core non disegna più,
   quindi si tiene una copia piccola aggiornata ogni mezzo secondo circa. */
static SDL_Surface *thumb;

static void thumb_update(const void *data, unsigned w, unsigned h, size_t pitch) {
    int tw = w > 200 ? 200 : (int)w, th = (int)(h * (unsigned)tw / w);
    if (th < 1) th = 1;
    if (!thumb || thumb->w != tw || thumb->h != th) {
        if (thumb) SDL_FreeSurface(thumb);
        thumb = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!thumb) return;
    }
    for (int y = 0; y < th; y++) {
        const uint8_t *row = (const uint8_t *)data + pitch * ((unsigned)y * h / (unsigned)th);
        uint32_t *dst = (uint32_t *)((uint8_t *)thumb->pixels + y * thumb->pitch);
        for (int x = 0; x < tw; x++) {
            unsigned sx = (unsigned)x * w / (unsigned)tw;
            if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                dst[x] = ((const uint32_t *)row)[sx] | 0xFF000000u;
            } else {
                unsigned p = ((const uint16_t *)row)[sx], r, g, b;
                if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) { r = p >> 11 & 31; g = (p >> 5 & 63) * 255 / 63; b = p & 31; }
                else { r = p >> 10 & 31; g = (p >> 5 & 31) * 255 / 31; b = p & 31; }
                dst[x] = 0xFF000000u | (r * 255 / 31) << 16 | g << 8 | (b * 255 / 31);
            }
        }
    }
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;   /* frame ripetuto: resta quello precedente */
    if (!video_first_logged) {
        video_first_logged = true;
        static const char *fmts[] = { "0RGB1555", "XRGB8888", "RGB565" };
        ui_log("video: primo fotogramma %ux%u, pitch %zu, formato %s", w, h, pitch, pixfmt <= 2 ? fmts[pixfmt] : "?");
    }
    if ((int)w > tex_w || (int)h > tex_h || !frame_tex) {
        if (frame_tex) SDL_DestroyTexture(frame_tex);
        /* formati SENZA trasparenza: il byte X di XRGB8888 (e il bit alto di 0RGB1555) vale di
           solito 0; con ARGB il gioco finiva trasparente nella tela e sulla console, dove il
           livello del display tiene conto dell'alfa, si vedeva solo il nero di fondo */
        Uint32 fmt = pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? SDL_PIXELFORMAT_RGB888
                   : pixfmt == RETRO_PIXEL_FORMAT_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB555;
        tex_w = (int)w > (int)av.geometry.max_width ? (int)w : (int)av.geometry.max_width;
        tex_h = (int)h > (int)av.geometry.max_height ? (int)h : (int)av.geometry.max_height;
        frame_tex = SDL_CreateTexture(UI.ren, fmt, SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
        if (!frame_tex) { video_error("SDL_CreateTexture"); return; }
        SDL_SetTextureBlendMode(frame_tex, SDL_BLENDMODE_NONE);
        ui_log("video: texture %dx%d %s creata (disegno %s)", tex_w, tex_h, SDL_GetPixelFormatName(fmt),
               glsh_loaded() ? "con shader OpenGL ES" : "con SDL_RenderCopyEx");
    }
    if (SDL_UpdateTexture(frame_tex, &(SDL_Rect){ 0, 0, (int)w, (int)h }, data, (int)pitch) != 0) video_error("SDL_UpdateTexture");
    frame_w = (int)w;
    frame_h = (int)h;
    have_frame = true;
    static unsigned thumb_tick;
    if (thumb_tick++ % 30 == 0) thumb_update(data, w, h, pitch);
}

static bool audio_on;

static bool fast_forward;

static ratectl_t rate;
static double rate_target;   /* scorta di audio voluta, in byte (due fotogrammi e mezzo) */

static size_t audio_batch(const int16_t *data, size_t frames) {
    if (!audio_on || fast_forward) return frames;
    static int16_t *buf;
    static size_t cap;
    size_t need = (size_t)((double)frames * 1.03) + 4;
    if (need > cap) { cap = need; buf = realloc(buf, cap * 4); }
    int n = ratectl_run(&rate, data, (int)frames, (double)SDL_GetQueuedAudioSize(UI.audio), rate_target, buf);
    ui_play_pcm(buf, n);
    return frames;
}

static void audio_sample(int16_t l, int16_t r) {
    int16_t s[2] = { l, r };
    audio_batch(s, 1);
}

static void input_poll(void) {}

static bool menu_open;

/* Con lo schermo ruotato (TATE) la console si tiene di lato: la croce gira con lei.
   Direzioni in senso orario: su, destra, giù, sinistra. */
static bool retro_pressed(int port, int id) {
    const bool *down = UI.port_down[port];
    static const int dirs[4] = { RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_RIGHT,
                                 RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT };
    if (gcfg.rotate) {
        for (int d = 0; d < 4; d++)
            if (dirs[d] == id) {
                int console_dir = dirs[(d - gcfg.rotate + 4) % 4];
                return down[default_map[console_dir]];
            }
    }
    int b = gcfg.map[id];
    return b >= 0 && down[b];
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    /* con Menu premuto i tasti servono alle scorciatoie, non al gioco */
    if (port > 3 || device != RETRO_DEVICE_JOYPAD || menu_open || UI.in.down[BTN_MENU]) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        int16_t mask = 0;
        for (int i = 0; i < 16; i++)
            if (retro_pressed((int)port, i)) mask |= (int16_t)(1 << i);
        return mask;
    }
    return id < 16 && retro_pressed((int)port, (int)id);
}

/* ----------------------------------------------------- caricamento */

static bool load_core(void) {
    core.h = dlopen(core_path, RTLD_LAZY | RTLD_LOCAL);
    if (!core.h) {
        const char *e = dlerror();
        ui_log("dlopen: %s", e ? e : "?");
        note_error(e ? e : "");
        return false;
    }
#define SYM(field, name) if (!(*(void **)&core.field = dlsym(core.h, name))) { ui_log("simbolo mancante: %s", name); return false; }
    SYM(init, "retro_init"); SYM(deinit, "retro_deinit");
    SYM(get_system_info, "retro_get_system_info"); SYM(get_system_av_info, "retro_get_system_av_info");
    SYM(set_environment, "retro_set_environment"); SYM(set_video_refresh, "retro_set_video_refresh");
    SYM(set_audio_sample, "retro_set_audio_sample"); SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(set_input_poll, "retro_set_input_poll"); SYM(set_input_state, "retro_set_input_state");
    SYM(set_controller_port_device, "retro_set_controller_port_device");
    SYM(reset, "retro_reset"); SYM(run, "retro_run");
    SYM(serialize_size, "retro_serialize_size"); SYM(serialize, "retro_serialize"); SYM(unserialize, "retro_unserialize");
    SYM(load_game, "retro_load_game"); SYM(unload_game, "retro_unload_game");
#undef SYM
    core.set_environment(environment);
    core.init();
    core.set_video_refresh(video_refresh);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    return true;
}

static bool load_game(void) {
    struct retro_system_info si = { 0 };
    core.get_system_info(&si);
    struct retro_game_info gi = { .path = rom_path };
    if (!si.need_fullpath) {
        FILE *f = fopen(rom_path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        gi.size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        rom_data = malloc(gi.size);
        gi.size = fread(rom_data, 1, gi.size, f);
        fclose(f);
        gi.data = rom_data;
    }
    ui_log("core %s %s", si.library_name, si.library_version);
    if (!core.load_game(&gi)) return false;
    core.get_system_av_info(&av);
    for (unsigned p = 0; p < 4; p++) core.set_controller_port_device(p, RETRO_DEVICE_JOYPAD);
    ui_log("video %ux%u (max %ux%u) aspetto %.3f, %.2f fps, audio %.0f Hz, rotazione %u, %d opzioni del core",
           av.geometry.base_width, av.geometry.base_height, av.geometry.max_width, av.geometry.max_height,
           av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate, rotation, ncopts);
    char keys[1600] = "";
    for (int i = 0; i < ncopts; i++) {
        size_t len = strlen(keys);
        snprintf(keys + len, sizeof keys - len, "%s%s", i ? ", " : "", copts[i].key);
    }
    ui_log("opzioni: %s", keys);
    return true;
}

/* ---------------------------------------------------------- stati */

/* Righe della pagina Salvataggi: tre slot, il salvataggio automatico e la sua copia precedente.
   Il primo slot è il file "state" di sempre, così gli stati già salvati restano validi. */
enum { NSLOTS = 3, SAVE_AUTO = NSLOTS, SAVE_PREV, SAVE_ROWS };
static const char *const save_ext[SAVE_ROWS] = { "state", "2.state", "3.state", "auto.state", "auto.state.prev" };

static void state_path(char *out, size_t n, bool autosave) {
    game_file(out, n, save_ext[autosave ? SAVE_AUTO : gcfg.slot]);
}

/* copia passando da un file provvisorio: la destinazione o è quella di prima o è completa */
static bool copy_whole(const char *from, const char *to) {
    char t[1500], buf[65536];
    snprintf(t, sizeof t, "%s.tmp", to);
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(t, "wb");
    bool ok = out != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    if (out) {
        ok = fflush(out) == 0 && fsync(fileno(out)) == 0 && ok;
        if (fclose(out) != 0) ok = false;
    }
    fclose(in);
    ok = ok && rename(t, to) == 0;
    if (!ok) remove(t);
    return ok;
}

/* Il salvataggio automatico di prima resta come copia, con la sua anteprima. Si copia e non si
   rinomina: così quello in uso c'è sempre, anche se la scrittura del nuovo non va a buon fine. */
static void auto_keep_previous(void) {
    char a[1400], b[1400], ap[1450], bp[1450];
    game_file(a, sizeof a, save_ext[SAVE_AUTO]);
    game_file(b, sizeof b, save_ext[SAVE_PREV]);
    snprintf(ap, sizeof ap, "%s.png", a);
    snprintf(bp, sizeof bp, "%s.png", b);
    if (copy_whole(a, b) && !copy_whole(ap, bp)) remove(bp);
}

static bool state_save(bool autosave) {
    size_t n = core.serialize_size();
    if (!n) return false;
    void *buf = malloc(n);
    bool ok = core.serialize(buf, n);
    if (ok) {
        char p[1400];
        state_path(p, sizeof p, autosave);
        /* su un file provvisorio: se la scrittura si interrompe, il salvataggio di prima resta */
        char t[1450];
        snprintf(t, sizeof t, "%s.tmp", p);
        FILE *f = fopen(t, "wb");
        ok = f && fwrite(buf, 1, n, f) == n;
        if (f) { ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok; fclose(f); }
        if (ok && autosave) auto_keep_previous();
        ok = ok && rename(t, p) == 0;
        if (!ok) remove(t);
        if (ok) {
            char png[1450];
            snprintf(png, sizeof png, "%s.png", p);
            if (thumb) IMG_SavePNG(thumb, png);
            else remove(png);   /* l'anteprima di prima non sarebbe più di questo salvataggio */
        }
    }
    free(buf);
    return ok;
}

static bool state_load_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = sz > 0 ? (size_t)sz : 0;
    void *buf = n && n <= (256u << 20) ? malloc(n) : NULL;
    if (!buf) { fclose(f); return false; }
    bool ok = fread(buf, 1, n, f) == n && core.unserialize(buf, n);
    fclose(f);
    free(buf);
    return ok;
}

static bool state_load(bool autosave) {
    char p[1400];
    state_path(p, sizeof p, autosave);
    return state_load_file(p);
}

/* ---------------------------------------------------------- video */

/* Filtro attivo: maschera ripetuta sull'immagine (moltiplicata, bianco = invariato)
   e vignettatura. La maschera "pixel" si costruisce alla risoluzione del gioco e si
   disegna con la stessa rotazione, così resta allineata ai pixel anche in TATE. */
static filter_t flt;
static SDL_Surface *flt_mask;
static SDL_Texture *flt_overlay, *flt_vignette;
static int ov_w, ov_h;

static void filter_apply(void) {
    if (!filter_load(root, UI.set.lang, gcfg.filter, &flt)) {
        filter_load(root, UI.set.lang, "nitido", &flt);
        ui_log("filtro %s non trovato", gcfg.filter);
    }
    if (flt_mask) SDL_FreeSurface(flt_mask);
    if (flt_overlay) SDL_DestroyTexture(flt_overlay);
    if (flt_vignette) SDL_DestroyTexture(flt_vignette);
    flt_mask = NULL;
    flt_overlay = flt_vignette = NULL;
    ov_w = ov_h = 0;
    if (flt.mask[0]) {
        SDL_Surface *m = IMG_Load(flt.mask);
        if (m) {
            flt_mask = SDL_ConvertSurfaceFormat(m, SDL_PIXELFORMAT_ARGB8888, 0);
            SDL_FreeSurface(m);
            /* intensità: ogni pixel della maschera si avvicina al bianco */
            Uint32 *px = flt_mask->pixels;
            for (int i = 0; i < flt_mask->w * flt_mask->h; i++) {
                Uint8 r, g, b;
                SDL_GetRGB(px[i], flt_mask->format, &r, &g, &b);
                float k = flt.strength;
                px[i] = SDL_MapRGBA(flt_mask->format, (Uint8)(255 - (255 - r) * k), (Uint8)(255 - (255 - g) * k),
                                    (Uint8)(255 - (255 - b) * k), 255);
            }
        } else ui_log("maschera non leggibile: %s", flt.mask);
    }
    if (flt.shader[0] && glsh_load(flt.shader)) {
        for (int i = 0; i < gcfg.nsp; i++) {
            glsh_param_t *q = glsh_param_find(gcfg.sp_name[i]);
            if (q) q->value = gcfg.sp_val[i];
        }
    }
    else glsh_unload();
    if (flt.vignette > 0) {
        enum { V = 128 };
        SDL_Surface *v = SDL_CreateRGBSurfaceWithFormat(0, V, V, 32, SDL_PIXELFORMAT_ARGB8888);
        Uint32 *px = v->pixels;
        for (int y = 0; y < V; y++)
            for (int x = 0; x < V; x++) {
                float dx = (x + 0.5f) / V * 2 - 1, dy = (y + 0.5f) / V * 2 - 1;
                float r = sqrtf(dx * dx + dy * dy) / 1.4142f, t = r < 0.45f ? 0 : (r - 0.45f) / 0.55f;
                Uint8 c = (Uint8)(255 * (1 - flt.vignette * t * t));
                px[y * (v->pitch / 4) + x] = SDL_MapRGBA(v->format, c, c, c, 255);
            }
        flt_vignette = SDL_CreateTextureFromSurface(UI.ren, v);
        SDL_FreeSurface(v);
        SDL_SetTextureBlendMode(flt_vignette, SDL_BLENDMODE_MOD);
        SDL_SetTextureScaleMode(flt_vignette, SDL_ScaleModeLinear);
    }
}

/* maschera ripetuta su una superficie tiles_x × tiles_y volte */
static SDL_Texture *build_overlay(int tiles_x, int tiles_y, int w, int h) {
    int mw = flt_mask->w, mh = flt_mask->h;
    SDL_Surface *row = SDL_CreateRGBSurfaceWithFormat(0, tiles_x * mw, mh, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *all = SDL_CreateRGBSurfaceWithFormat(0, tiles_x * mw, tiles_y * mh, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_SetSurfaceBlendMode(flt_mask, SDL_BLENDMODE_NONE);
    SDL_SetSurfaceBlendMode(row, SDL_BLENDMODE_NONE);
    for (int x = 0; x < tiles_x; x++) SDL_BlitSurface(flt_mask, NULL, row, &(SDL_Rect){ x * mw, 0, mw, mh });
    for (int y = 0; y < tiles_y; y++) SDL_BlitSurface(row, NULL, all, &(SDL_Rect){ 0, y * mh, row->w, mh });
    SDL_Texture *t = SDL_CreateTextureFromSurface(UI.ren, all);
    SDL_FreeSurface(row);
    SDL_FreeSurface(all);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_MOD);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    ov_w = w;
    ov_h = h;
    return t;
}

/* Cornici: Bezels/<core>/<rom>.png, Bezels/<rom>.png, poi Bezels/_default.png o
   _default_vertical.png. La finestra del gioco è il rettangolo trasparente della PNG,
   oppure "viewport": [x, y, w, h] in un .json con lo stesso nome. */
static SDL_Texture *bez_tex;
static SDL_Rect bez_vp;
static int bez_w, bez_h, bez_vertical = -1;

static bool bezel_open(const char *path) {
    SDL_Surface *s0 = IMG_Load(path);
    if (!s0) return false;
    SDL_Surface *s = SDL_ConvertSurfaceFormat(s0, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s0);
    if (!s) return false;
    int x0 = s->w, y0 = s->h, x1 = -1, y1 = -1;
    Uint32 *px = s->pixels;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if ((px[y * (s->pitch / 4) + x] >> 24) < 32) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    bez_vp = x1 >= x0 ? (SDL_Rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 } : (SDL_Rect){ 0, 0, s->w, s->h };
    char json[1200];
    snprintf(json, sizeof json, "%.*s.json", (int)(strlen(path) - 4), path);
    jval *j = json_load(json), *vp = json_get(j, "viewport");
    if (vp && vp->t == J_ARR && vp->child && vp->child->next && vp->child->next->next && vp->child->next->next->next) {
        jval *c = vp->child;
        bez_vp = (SDL_Rect){ json_to_int(c->n, 0), json_to_int(c->next->n, 0), json_to_int(c->next->next->n, 0), json_to_int(c->next->next->next->n, 0) };
    }
    json_free(j);
    bez_w = s->w;
    bez_h = s->h;
    bez_tex = SDL_CreateTextureFromSurface(UI.ren, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(bez_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(bez_tex, SDL_ScaleModeLinear);
    ui_log("cornice %s, finestra %d,%d %dx%d", path, bez_vp.x, bez_vp.y, bez_vp.w, bez_vp.h);
    return true;
}

static void bezel_load(bool vertical) {
    if (bez_tex) SDL_DestroyTexture(bez_tex);
    bez_tex = NULL;
    bez_vertical = vertical;
    char p[1400];
    snprintf(p, sizeof p, "%s/Bezels/%s/%s.png", root, core_id, rom_name);
    if (bezel_open(p)) return;
    snprintf(p, sizeof p, "%s/Bezels/%s.png", root, rom_name);
    if (bezel_open(p)) return;
    snprintf(p, sizeof p, "%s/Bezels/_default%s.png", root, vertical ? "_vertical" : "");
    bezel_open(p);
}

static void draw_game(void) {
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 255));
    if (!have_frame) return;
    float aspect = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio : (float)frame_w / (float)frame_h;
    /* Ruotato dal core di 90° il gioco è verticale. Alcuni core (mame2003-plus) indicano già
       le proporzioni verticali, altri quelle del frame non ruotato: vale la minore. */
    if (rotation & 1) aspect = fminf(aspect, 1 / aspect);
    if (gcfg.rotate & 1) aspect = 1 / aspect;
    unsigned total = (rotation + (unsigned)gcfg.rotate) & 3;
    bool odd = total & 1;
    int src_w = odd ? frame_h : frame_w, src_h = odd ? frame_w : frame_h;

    /* area del gioco: tutto lo schermo, o la finestra della cornice */
    SDL_Rect area = { 0, 0, UI.w, UI.h }, bez_dst = { 0 };
    if (gcfg.bezel) {
        if (bez_vertical != (aspect < 1)) bezel_load(aspect < 1);
        if (bez_tex) {
            float s = fminf((float)UI.w / bez_w, (float)UI.h / bez_h);
            bez_dst = (SDL_Rect){ (int)((UI.w - bez_w * s) / 2), (int)((UI.h - bez_h * s) / 2), (int)(bez_w * s), (int)(bez_h * s) };
            area = (SDL_Rect){ bez_dst.x + (int)(bez_vp.x * s), bez_dst.y + (int)(bez_vp.y * s), (int)(bez_vp.w * s), (int)(bez_vp.h * s) };
        }
    }
    int dw, dh;
    switch (gcfg.aspect) {
    case ASPECT_INTEGER: {
        int s = (int)fminf((float)area.w / src_w, (float)area.h / src_h);
        if (s < 1) s = 1;
        dw = src_w * s;
        dh = src_h * s;
        break;
    }
    case ASPECT_FULL: dw = area.w; dh = area.h; break;
    default:
        dh = area.h;
        dw = (int)(dh * aspect);
        if (dw > area.w) { dw = area.w; dh = (int)(dw / aspect); }
    }
    int x0 = area.x + (area.w - dw) / 2, y0 = area.y + (area.h - dh) / 2;
    SDL_SetTextureScaleMode(frame_tex, flt.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_Rect src = { 0, 0, frame_w, frame_h };
    int rw = odd ? dh : dw, rh = odd ? dw : dh;
    SDL_Rect dst = { x0 + (dw - rw) / 2, y0 + (dh - rh) / 2, rw, rh };
    static unsigned frame_count;
    frame_count++;
    if (glsh_loaded()) glsh_draw(frame_tex, tex_w, tex_h, frame_w, frame_h, (SDL_Rect){ x0, y0, dw, dh }, total, flt.smooth, frame_count);
    else if (SDL_RenderCopyEx(UI.ren, frame_tex, &src, &dst, -90.0 * total, NULL, SDL_FLIP_NONE) != 0) video_error("SDL_RenderCopyEx");

    if (flt_mask) {
        if (flt.mask_per_pixel) {
            if (!flt_overlay || ov_w != frame_w || ov_h != frame_h) {
                if (flt_overlay) SDL_DestroyTexture(flt_overlay);
                flt_overlay = build_overlay(frame_w, frame_h, frame_w, frame_h);
            }
            SDL_RenderCopyEx(UI.ren, flt_overlay, NULL, &dst, -90.0 * total, NULL, SDL_FLIP_NONE);
        } else {
            if (!flt_overlay || ov_w != dw || ov_h != dh) {
                if (flt_overlay) SDL_DestroyTexture(flt_overlay);
                flt_overlay = build_overlay((dw + flt_mask->w - 1) / flt_mask->w, (dh + flt_mask->h - 1) / flt_mask->h, dw, dh);
            }
            SDL_RenderCopy(UI.ren, flt_overlay, &(SDL_Rect){ 0, 0, dw, dh }, &(SDL_Rect){ x0, y0, dw, dh });
        }
    }
    if (flt_vignette) SDL_RenderCopy(UI.ren, flt_vignette, NULL, &(SDL_Rect){ x0, y0, dw, dh });
    if (gcfg.bezel && bez_tex) SDL_RenderCopy(UI.ren, bez_tex, NULL, &bez_dst);
}

/* ------------------------------------------------------ menu in gioco */
/* Tre pagine con la stessa grafica: principale, opzioni del gioco (core), comandi. */

enum { PAGE_MAIN, PAGE_OPTIONS, PAGE_CONTROLS, PAGE_SHADER, PAGE_SAVES, PAGE_HOTKEYS };
enum { MI_RESUME, MI_SAVE, MI_LOAD, MI_SAVES, MI_ASPECT, MI_FILTER, MI_BEZEL, MI_ROTATE, MI_CPU, MI_SHADER, MI_OPTIONS, MI_CONTROLS, MI_HOTKEYS,
       MI_RESET, MI_EXIT, MI_COUNT };
static const char *main_keys[MI_COUNT] = { "game.resume", "game.save_state", "game.load_state", "saves.title", "game.aspect",
                                           "game.filter", "game.bezel", "game.rotation", "game.cpu", "game.shader_params", "game.options", "game.controls", "hotkey.page",
                                           "game.reset", "game.exit" };
static const char *cpu_names[] = { "cpu.auto", "cpu.min", "cpu.save", "cpu.balanced", "cpu.perf" };
static const char *aspect_names[] = { "aspect.original", "aspect.integer", "aspect.full" };
static filter_t filters[24];
static int nfilters;
static const char *rotate_names[] = { "rotate.none", "rotate.left", "rotate.right" };

/* tasti RetroPad modificabili nella pagina comandi, nell'ordine in cui compaiono */
static const int remap_ids[] = { RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
                                 RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
                                 RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
                                 RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START };
#define NREMAP ((int)(sizeof remap_ids / sizeof remap_ids[0]))
static const char *retro_names[16] = { "B", "Y", "Select", "Start", "↑", "↓", "←", "→", "A", "X", "L", "R", "L2", "R2", "L3", "R3" };
/* tasti della console assegnabili, nell'ordine in cui si scorrono */
static const int console_btns[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_L2, BTN_R2, BTN_SELECT, BTN_START, -1 };
#define NCONSOLE ((int)(sizeof console_btns / sizeof console_btns[0]))

static const char *console_name(int b) {
    switch (b) {
    case BTN_A: return "A"; case BTN_B: return "B"; case BTN_X: return "X"; case BTN_Y: return "Y";
    case BTN_L: return "L"; case BTN_R: return "R"; case BTN_L2: return "L2"; case BTN_R2: return "R2";
    case BTN_SELECT: return "Select"; case BTN_START: return "Start";
    default: return T("button.none");
    }
}

/* tasti rapidi mostrati nella pagina comandi ("@" = testo da tradurre) */
static const struct { const char *combo, *action; } hotkeys_list[] = {
    { "R", "game.save_state" }, { "L", "game.load_state" }, { "R2", "hotkey.ff" }, { "X", "hotkey.shot" },
    { "@hotkey.updown", "settings.volume" }, { "@hotkey.leftright", "settings.brightness" },
};
#define NHOTKEYS ((int)(sizeof hotkeys_list / sizeof hotkeys_list[0]))

static int page, sel[6];

/* Opzioni mostrate: prima i DIP switch del gioco (i più utili in sala giochi), poi le
   altre; i "debug-dip" di FBNeo servono solo agli sviluppatori e restano nascosti. */
static int optv[256], noptv;

static void build_option_view(void) {
    noptv = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ncopts; i++) {
            bool dip = strstr(copts[i].key, "-dipswitch-") != NULL;
            if (strstr(copts[i].key, "debug-dip") || copts[i].nvals < 2) continue;
            if (dip == (pass == 0)) optv[noptv++] = i;
        }
}
static float scroll[6], hl[6], menu_anim;
static int exit_code = -1;

/* Statistiche: System/stats.tsv, "core<TAB>rom<TAB>partite<TAB>secondi<TAB>ultima volta".
   Il tempo conta solo mentre si gioca, non con il menu aperto. */
static double played;

static void stats_update(void) {
    char p[1200], tmp[1210], line[512];
    snprintf(p, sizeof p, "%s/System/stats.tsv", root);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *in = fopen(p, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    bool found = false;
    while (in && fgets(line, sizeof line, in)) {
        char c[64], n[128];
        long plays = 0, secs = 0, last = 0;
        if (sscanf(line, "%63[^\t]\t%127[^\t]\t%ld\t%ld\t%ld", c, n, &plays, &secs, &last) == 5 &&
            !strcmp(c, core_id) && !strcmp(n, rom_name)) {
            fprintf(out, "%s\t%s\t%ld\t%ld\t%ld\n", c, n, plays + 1, secs + (long)played, (long)time(NULL));
            found = true;
        } else {
            fputs(line, out);
        }
    }
    if (!found) fprintf(out, "%s\t%s\t1\t%ld\t%ld\n", core_id, rom_name, (long)played, (long)time(NULL));
    if (in) fclose(in);
    fclose(out);
    rename(tmp, p);
}

static void write_resume(void) {
    char p[1200], t[1300];
    snprintf(p, sizeof p, "%s/System/resume.txt", root);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    if (!f) { ui_log("impossibile scrivere %s", t); return; }
    fprintf(f, "libretro\t%s\t%s\t%s\tresume\n", core_path, rom_path, title);
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(t, p) != 0) { ui_log("impossibile scrivere %s", p); remove(t); }
}

/* Prestazioni: ogni 10 s di gioco nel log fotogrammi al secondo, tempi dell'emulatore e del
   disegno (media e massimo), audio rimasto senza campioni, frequenza e temperatura della CPU */
static struct { int frames, starved; double emu, emu_max, draw, draw_max; Uint64 t0; } perf;

static int read_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}

static void perf_frame(Uint64 a, Uint64 b, Uint64 c, Uint32 queued, Uint32 frame_bytes, double fps) {
    if (!ui_debug_on(1)) return;   /* registro "Prestazioni" o "Completo" */
    double f = (double)SDL_GetPerformanceFrequency();
    double emu = (b - a) * 1000.0 / f, draw = (c - b) * 1000.0 / f;
    /* dopo una pausa (menu, sospensione) si ricomincia a contare */
    static Uint64 last;
    if (last && (a - last) / f > 0.5) memset(&perf, 0, sizeof perf);
    last = c;
    if (!perf.t0) perf.t0 = a;
    perf.frames++;
    perf.emu += emu;
    perf.draw += draw;
    if (emu > perf.emu_max) perf.emu_max = emu;
    if (draw > perf.draw_max) perf.draw_max = draw;
    if (queued < frame_bytes) perf.starved++;   /* la scheda audio ha quasi finito i campioni */
    double secs = (c - perf.t0) / f;
    if (secs < 10) return;
    int mhz = read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") / 1000;
    int temp = read_int("/sys/class/thermal/thermal_zone0/temp");
    ui_log("prestazioni: %.1f fps (gioco a %.1f), emulatore %.1f ms (max %.1f), disegno %.1f ms (max %.1f), "
           "audio scarso %d volte, CPU %d MHz (profilo %d, FN %d), %d °C",
           perf.frames / secs, fps, perf.emu / perf.frames, perf.emu_max, perf.draw / perf.frames, perf.draw_max,
           perf.starved, mhz, UI.set.cpu_profile, UI.in.fn, temp > 1000 ? temp / 1000 : temp);
    memset(&perf, 0, sizeof perf);
}

static bool suspend_ask;
static int ask_sel = 1;

static void power_off_from_game(void) {
    if (state_save(true)) write_resume();
    exit_code = EXIT_POWEROFF;
}

/* voci del menu senza effetto per questo gioco: in grigio */
static bool row_disabled(int i) {
    return page == PAGE_MAIN && ((i == MI_SHADER && !glsh_param_count()) || (i == MI_OPTIONS && !noptv));
}

static int page_rows(void) {
    if (page == PAGE_OPTIONS) return noptv ? noptv : 1;
    if (page == PAGE_SHADER) return glsh_param_count() ? glsh_param_count() : 1;
    if (page == PAGE_CONTROLS) return NREMAP + 1;   /* + ripristina */
    if (page == PAGE_SAVES) return SAVE_ROWS;
    if (page == PAGE_HOTKEYS) return NHOTKEYS;
    return MI_COUNT;
}

static int rot_index(void) {
    for (int i = 0; i < 3; i++)
        if (rot_values[i] == gcfg.rotate) return i;
    return 0;
}

static void main_change(int item, int dir) {
    switch (item) {
    case MI_ASPECT: gcfg.aspect = (gcfg.aspect + dir + 3) % 3; break;
    case MI_FILTER: {
        if (!nfilters) nfilters = filters_list(root, UI.set.lang, filters, 24);
        if (!nfilters) return;
        int cur = 0;
        for (int i = 0; i < nfilters; i++)
            if (!strcmp(filters[i].id, gcfg.filter)) cur = i;
        snprintf(gcfg.filter, sizeof gcfg.filter, "%s", filters[(cur + dir + nfilters) % nfilters].id);
        filter_apply();
        break;
    }
    case MI_BEZEL: gcfg.bezel = !gcfg.bezel; break;
    case MI_ROTATE: gcfg.rotate = rot_values[(rot_index() + dir + 3) % 3]; break;
    case MI_CPU: gcfg.cpu = (gcfg.cpu + dir + 5) % 5; apply_cpu(); break;
    default: return;
    }
    gamecfg_save();
    ui_sound(SND_MOVE);
}

static void remap_change(int row, int dir) {
    if (row == NREMAP) return;
    int id = remap_ids[row], cur = 0;
    for (int i = 0; i < NCONSOLE; i++)
        if (console_btns[i] == gcfg.map[id]) cur = i;
    gcfg.map[id] = console_btns[(cur + dir + NCONSOLE) % NCONSOLE];
    gamecfg_save();
    ui_sound(SND_MOVE);
}

static void open_page(int p) {
    if (p == PAGE_OPTIONS) build_option_view();
    page = p;
    sel[p] = 0;
    scroll[p] = hl[p] = 0;
    ui_sound(SND_OK);
}

/* azioni che fanno perdere qualcosa: si chiede conferma, con Annulla già selezionato */
enum { ASK_NONE, ASK_RESTART, ASK_CONTROLS, ASK_OVERWRITE };
static int ask_kind, ask_kind_sel, ask_row;

static void ask_start(int kind) {
    ask_kind = kind;
    ask_kind_sel = 0;
    ui_sound(SND_OK);
}

/* ----------------------------------------- pagina Salvataggi */

static SDL_Texture *sv_tex;
static int sv_row = -1;

static void sv_forget(void) {
    if (sv_tex) SDL_DestroyTexture(sv_tex);
    sv_tex = NULL;
    sv_row = -1;
}

static bool save_exists(int row, time_t *when) {
    char p[1400];
    struct stat st;
    game_file(p, sizeof p, save_ext[row]);
    if (stat(p, &st) != 0) return false;
    if (when) *when = st.st_mtime;
    return true;
}

static void saves_write(int row) {
    gcfg.slot = row;
    gamecfg_save();
    ui_toast(T(state_save(false) ? "game.state_saved" : "game.save_failed"));
    sv_forget();
    ui_sound(SND_OK);
}

static void saves_load(int row) {
    char p[1400];
    if (!save_exists(row, NULL)) { ui_toast(T("game.no_state")); ui_sound(SND_ERROR); return; }
    if (row < NSLOTS) {
        gcfg.slot = row;
        gamecfg_save();
        state_save(true);   /* la partita in corso resta recuperabile dal salvataggio automatico */
    }
    game_file(p, sizeof p, save_ext[row]);
    if (state_load_file(p)) { ui_toast(T("game.state_loaded")); menu_open = false; ui_sound(SND_OK); }
    else { ui_toast(T("saves.load_failed")); ui_sound(SND_ERROR); }
    sv_forget();
}

static void saves_preview(int x, int y, int w, int h, int row) {
    theme_t *t = &UI.th;
    if (row != sv_row) {
        sv_forget();
        sv_row = row;
        char p[1400], png[1450];
        game_file(p, sizeof p, save_ext[row]);
        snprintf(png, sizeof png, "%s.png", p);
        sv_tex = IMG_LoadTexture(UI.ren, png);
    }
    ui_rrect(x, y, w, h, M(t->panel_radius), rgba(0, 0, 0, 120));
    if (!sv_tex) {
        ui_text(UI.f_small, T("saves.no_preview"), x + w / 2, y + h / 2 - TTF_FontHeight(UI.f_small) / 2, t->text_dim, ALIGN_CENTER);
        return;
    }
    int tw, th, m = M(0.01f);
    SDL_QueryTexture(sv_tex, NULL, NULL, &tw, &th);
    unsigned total = (rotation + (unsigned)gcfg.rotate) & 3;
    bool odd = total & 1;
    float vw = (float)(odd ? th : tw), vh = (float)(odd ? tw : th), s = fminf((w - 2 * m) / vw, (h - 2 * m) / vh);
    int dw = (int)(vw * s), dh = (int)(vh * s), x0 = x + (w - dw) / 2, y0 = y + (h - dh) / 2;
    int rw = odd ? dh : dw, rh = odd ? dw : dh;
    SDL_Rect d = { x0 + (dw - rw) / 2, y0 + (dh - rh) / 2, rw, rh };
    SDL_RenderCopyEx(UI.ren, sv_tex, NULL, &d, -90.0 * total, NULL, SDL_FLIP_NONE);
}

static void menu_input(void) {
    input_t *in = &UI.in;
    int n = page_rows(), *s = &sel[page];
    if (in->pressed[BTN_UP]) { *s = (*s + n - 1) % n; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { *s = (*s + 1) % n; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_L]) { *s = *s - 8 < 0 ? 0 : *s - 8; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_R]) { *s = *s + 8 >= n ? n - 1 : *s + 8; ui_sound(SND_MOVE); }
    int dir = in->pressed[BTN_RIGHT] ? 1 : in->pressed[BTN_LEFT] ? -1 : 0;
    bool ok = in->pressed[BTN_A];

    if (page != PAGE_MAIN) {
        if (in->pressed[BTN_B]) { page = PAGE_MAIN; ui_sound(SND_BACK); return; }
        if (in->pressed[BTN_MENU]) { menu_open = false; ui_sound(SND_BACK); return; }
        if (page == PAGE_OPTIONS && noptv && (dir || ok)) { copt_cycle(&copts[optv[*s]], dir ? dir : 1); ui_sound(SND_MOVE); }
        if (page == PAGE_SHADER && glsh_param_count() && dir) {
            glsh_param_t *q = glsh_param(*s);
            float v = q->value + dir * q->step;
            q->value = v < q->min ? q->min : v > q->max ? q->max : v;
            gamecfg_save();
            ui_sound(SND_MOVE);
        }
        if (page == PAGE_SHADER && glsh_param_count() && ok) {
            glsh_param(*s)->value = glsh_param(*s)->def;   /* A: valore predefinito */
            gamecfg_save();
            ui_sound(SND_OK);
        }
        if (page == PAGE_CONTROLS) {
            if (dir || (ok && *s < NREMAP)) remap_change(*s, dir ? dir : 1);
            else if (ok && *s == NREMAP) ask_start(ASK_CONTROLS);
        }
        if (page == PAGE_SAVES) {
            if (ok) saves_load(*s);
            else if (in->pressed[BTN_X] && *s < NSLOTS) {
                if (save_exists(*s, NULL)) { ask_row = *s; ask_start(ASK_OVERWRITE); }
                else saves_write(*s);
            }
        }
        return;
    }

    if (dir) main_change(*s, dir);
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) { menu_open = false; ui_sound(SND_BACK); return; }
    if (!ok) return;
    if (row_disabled(*s)) { ui_sound(SND_ERROR); return; }
    switch (*s) {
    case MI_RESUME: menu_open = false; break;
    case MI_SAVE: ui_toast(T(state_save(false) ? "game.state_saved" : "game.save_failed")); break;
    case MI_LOAD:
        if (state_load(false)) { ui_toast(T("game.state_loaded")); menu_open = false; }
        else ui_toast(T("game.no_state"));
        break;
    case MI_ASPECT: case MI_FILTER: case MI_BEZEL: case MI_ROTATE: case MI_CPU: main_change(*s, 1); return;
    case MI_OPTIONS: open_page(PAGE_OPTIONS); return;
    case MI_CONTROLS: open_page(PAGE_CONTROLS); return;
    case MI_HOTKEYS: open_page(PAGE_HOTKEYS); return;
    case MI_SAVES: open_page(PAGE_SAVES); sel[PAGE_SAVES] = gcfg.slot; hl[PAGE_SAVES] = 0; sv_forget(); return;
    case MI_SHADER: open_page(PAGE_SHADER); return;
    case MI_RESET: ask_start(ASK_RESTART); break;
    case MI_EXIT: exit_code = EXIT_MENU; break;
    }
    ui_sound(SND_OK);
}

/* testo e valore della riga i della pagina corrente; *sub = apre una pagina */
static void row_text(int i, char *label, size_t ln, char *value, size_t vn, bool *sub) {
    *sub = false;
    value[0] = 0;
    if (page == PAGE_MAIN) {
        snprintf(label, ln, "%s", T(main_keys[i]));
        if (i == MI_ASPECT) snprintf(value, vn, "%s", T(aspect_names[gcfg.aspect % 3]));
        if (i == MI_FILTER) snprintf(value, vn, "%s", flt.name);
        if (i == MI_BEZEL) snprintf(value, vn, "%s", T(gcfg.bezel ? "common.yes" : "common.no"));
        if (i == MI_ROTATE) snprintf(value, vn, "%s", T(rotate_names[rot_index()]));
        if (i == MI_CPU) snprintf(value, vn, "%s", T(cpu_names[gcfg.cpu % 5]));
        *sub = i == MI_OPTIONS || i == MI_CONTROLS || i == MI_SHADER || i == MI_SAVES || i == MI_HOTKEYS;
    } else if (page == PAGE_SAVES) {
        time_t when;
        if (i < NSLOTS) {
            snprintf(label, ln, T("saves.slot"), i + 1);
            if (i == gcfg.slot) { size_t l = strlen(label); snprintf(label + l, ln - l, "  •"); }
        } else snprintf(label, ln, "%s", T(i == SAVE_AUTO ? "saves.auto" : "saves.prev"));
        if (save_exists(i, &when)) {
            struct tm tmv;
            localtime_r(&when, &tmv);
            strftime(value, vn, "%d/%m %H:%M", &tmv);
        } else snprintf(value, vn, "%s", T("saves.empty"));
    } else if (page == PAGE_HOTKEYS) {
        snprintf(label, ln, "Menu + %s", hotkeys_list[i].combo[0] == '@' ? T(hotkeys_list[i].combo + 1) : hotkeys_list[i].combo);
        snprintf(value, vn, "%s", T(hotkeys_list[i].action));
    } else if (page == PAGE_SHADER) {
        if (!glsh_param_count()) { snprintf(label, ln, "%s", T(glsh_loaded() ? "game.no_params" : "game.no_shader")); return; }
        glsh_param_t *q = glsh_param(i);
        snprintf(label, ln, "%s", q->desc[0] ? q->desc : q->name);
        snprintf(value, vn, "%g", q->value);
    } else if (page == PAGE_OPTIONS) {
        if (!noptv) { snprintf(label, ln, "%s", T("game.no_options")); return; }
        coreopt_t *o = &copts[optv[i]];
        /* FBNeo scrive "[Dipswitch] Lives": più leggibile "DIP · Lives" */
        const char *d = o->desc[0] ? o->desc : o->key;
        if (!strncmp(d, "[Dipswitch] ", 12)) snprintf(label, ln, "DIP · %s", d + 12);
        else snprintf(label, ln, "%s", d);
        snprintf(value, vn, "%s", o->value);
    } else if (i == NREMAP) {
        snprintf(label, ln, "%s", T("game.controls_reset"));
    } else {
        int id = remap_ids[i];
        if (input_desc[id][0]) snprintf(label, ln, "%s  ·  %s", retro_names[id], input_desc[id]);
        else snprintf(label, ln, "%s", retro_names[id]);
        snprintf(value, vn, "%s", console_name(gcfg.map[id]));
    }
}

static bool row_changeable(int i) {
    if (page == PAGE_SAVES || page == PAGE_HOTKEYS) return false;
    if (page == PAGE_MAIN) return i == MI_ASPECT || i == MI_FILTER || i == MI_BEZEL || i == MI_ROTATE || i == MI_CPU;
    if (page == PAGE_OPTIONS) return noptv > 0;
    if (page == PAGE_SHADER) return glsh_param_count() > 0;
    return i < NREMAP;
}

static void menu_draw(void) {
    theme_t *t = &UI.th;
    menu_anim = ui_approach(menu_anim, 1);
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, (Uint8)(150 * menu_anim)));
    int hb = M(t->hintbar_h), my = M(0.035f);
    int pw = page == PAGE_MAIN ? M(0.80f) : UI.w - 2 * M(t->margin);
    if (pw > UI.w - 2 * M(t->margin)) pw = UI.w - 2 * M(t->margin);
    int ph = UI.h - hb - 2 * my;
    int px = M(t->margin) - (int)((1 - menu_anim) * pw), py = my, r = M(t->panel_radius) * 2;
    ui_shadow(px, py + M(0.01f), pw, ph, r, M(0.04f), rgba(0, 0, 0, 110));
    ui_rrect(px, py, pw, ph, r, t->surface);

    /* intestazione: titolo del gioco, e nelle sottopagine il nome della pagina */
    char buf[300], label[200], value[120];
    int pad = M(0.04f), y = py + pad;
    ui_text_fit(UI.f_title, title, pw - 2 * pad, buf, sizeof buf);
    ui_text(UI.f_title, buf, px + pad, y, t->text, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_title);
    if (page != PAGE_MAIN) {
        ui_text(UI.f_small, T(page == PAGE_OPTIONS ? "game.options" : page == PAGE_SHADER ? "game.shader_params" : page == PAGE_SAVES ? "saves.title" : page == PAGE_HOTKEYS ? "hotkey.title" : "game.controls"),
                px + pad, y, t->accent, ALIGN_LEFT);
        y += TTF_FontHeight(UI.f_small);
    }
    y += M(0.02f);

    /* nota in fondo al pannello */
    const char *note = page == PAGE_OPTIONS ? T("game.options_note") : page == PAGE_SHADER ? T("game.shader_note")
                     : page == PAGE_CONTROLS ? T("game.controls_note") : page == PAGE_SAVES ? T("saves.note")
                     : page == PAGE_HOTKEYS ? T("hotkey.note") : T("game.per_game_note");
    int note_h = TTF_FontHeight(UI.f_small) + M(0.02f);
    ui_text_fit(UI.f_small, note, pw - 2 * pad, buf, sizeof buf);
    ui_text(UI.f_small, buf, px + pad, py + ph - pad / 2 - note_h + M(0.01f), t->text_dim, ALIGN_LEFT);

    /* righe con scorrimento */
    int rh = M(t->list_row_h), n = page_rows(), *s = &sel[page];
    int list_w = page == PAGE_SAVES ? pw * 58 / 100 : pw;   /* a destra l'anteprima */
    int area_h = py + ph - pad / 2 - note_h - y;
    area_h -= area_h % rh;
    float ry = (float)*s * rh;
    float target = scroll[page];
    if (ry - target > area_h - rh) target = ry - (area_h - rh);
    if (ry < target) target = ry;
    scroll[page] = ui_approach(scroll[page], target);
    hl[page] = ui_approach(hl[page], ry);
    TTF_Font *f = UI.f_normal;
    int fh = TTF_FontHeight(f);
    ui_clip_push(px, y, list_w, area_h);
    ui_rrect(px + M(0.02f), y + (int)(hl[page] - scroll[page]), list_w - M(0.04f), rh, M(t->panel_radius), t->accent);
    for (int i = 0; i < n; i++) {
        int ry_i = y + (int)(i * rh - scroll[page]);
        if (ry_i + rh < y || ry_i > y + area_h) continue;
        bool is_sel = i == *s, sub;
        row_text(i, label, sizeof label, value, sizeof value, &sub);
        color_t c = is_sel ? t->accent_text : t->text;
        if (row_disabled(i)) { c = with_alpha(is_sel ? t->accent_text : t->text_dim, 120); sub = false; }
        int ty = ry_i + (rh - fh) / 2, right = px + list_w - M(0.05f);
        int vw = 0;
        if (value[0]) {
            snprintf(buf, sizeof buf, is_sel && row_changeable(i) ? "‹ %s ›" : "%s", value);
            char fitv[160];
            ui_text_fit(f, buf, page == PAGE_SAVES ? list_w / 2 : list_w / 3, fitv, sizeof fitv);
            vw = ui_text(f, fitv, right, ty, is_sel ? c : t->text_dim, ALIGN_RIGHT) + M(0.03f);
        } else if (sub) {
            vw = ui_text(f, "›", right, ty, is_sel ? c : t->text_dim, ALIGN_RIGHT) + M(0.03f);
        }
        ui_text_fit(f, label, right - vw - (px + M(0.05f)), buf, sizeof buf);
        ui_text(f, buf, px + M(0.05f), ty, c, ALIGN_LEFT);
    }
    ui_clip_pop();
    if (page == PAGE_SAVES) saves_preview(px + list_w + M(0.01f), y, pw - list_w - pad, area_h, *s);

    ui_rect(0, UI.h - hb, UI.w, hb, with_alpha(t->bg, (int)(235 * menu_anim)));
    if (page == PAGE_MAIN) {
        const char *h[] = { "B", T("game.back_to_game"), "A", T("common.ok") };
        ui_hints(h, 4);
    } else if (page == PAGE_SAVES) {
        const char *h[] = { "B", T("common.back"), "A", T("saves.load"), "X", T("saves.save") };
        ui_hints(h, *s < NSLOTS ? 6 : 4);
    } else if (page == PAGE_HOTKEYS) {
        const char *h[] = { "B", T("common.back") };
        ui_hints(h, 2);
    } else {
        const char *h[] = { "B", T("common.back"), "<>", T("common.change") };
        ui_hints(h, 4);
    }
}

/* ----------------------------------------------------- tasti rapidi */
/* Con Menu tenuto premuto: R salva, L carica, R2 avanti veloce, X screenshot,
   su/giù volume, sinistra/destra luminosità. Menu premuto e rilasciato da solo apre
   il menu. Restituisce true quando il menu va aperto. */

enum { OSD_NONE, OSD_VOLUME, OSD_BRIGHT };
static int osd_kind;
static double osd_until;
static bool menu_combo;

static void osd_draw(void) {
    if (!osd_kind || UI.t > osd_until) return;
    theme_t *t = &UI.th;
    float a = (float)fmin(1.0, (osd_until - UI.t) / 0.3);
    int w = M(0.62f), h = M(0.1f), x = (UI.w - w) / 2, y = M(0.05f), pad = M(0.03f);
    ui_rrect(x, y, w, h, h / 2, with_alpha(rgba(0, 0, 0, 255), (int)(180 * a)));
    const char *label = T(osd_kind == OSD_VOLUME ? "settings.volume" : "settings.brightness");
    int v = osd_kind == OSD_VOLUME ? UI.set.volume : UI.set.brightness, max = osd_kind == OSD_VOLUME ? 20 : 10;
    int lw = ui_text(UI.f_small, label, x + pad, y + (h - TTF_FontHeight(UI.f_small)) / 2, with_alpha(rgba(255, 255, 255, 255), (int)(255 * a)), ALIGN_LEFT);
    int bx = x + pad + lw + M(0.03f), bw = x + w - pad - bx, bh = M(0.014f);
    ui_rrect(bx, y + (h - bh) / 2, bw, bh, bh / 2, with_alpha(rgba(255, 255, 255, 255), (int)(70 * a)));
    ui_rrect(bx, y + (h - bh) / 2, bw * v / max, bh, bh / 2, with_alpha(t->accent, (int)(255 * a)));
}

static void screenshot(void) {
    char dir[1100], p[1300], stamp[32];
    snprintf(dir, sizeof dir, "%s/Screenshots", root);
    mkdir(dir, 0755);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    snprintf(p, sizeof p, "%s/%s-%s.png", dir, rom_name, stamp);
    ui_screenshot(p);
}

static bool hotkeys(void) {
    static bool was_down;
    input_t *in = &UI.in;
    bool down = in->down[BTN_MENU], released = was_down && !down;
    was_down = down;
    if (in->pressed[BTN_MENU]) menu_combo = false;
    if (down) {
        bool used = true;
        if (in->pressed[BTN_R]) ui_toast(T(state_save(false) ? "game.state_saved" : "game.save_failed"));
        else if (in->pressed[BTN_L]) ui_toast(T(state_load(false) ? "game.state_loaded" : "game.no_state"));
        else if (in->pressed[BTN_R2]) {
            fast_forward = !fast_forward;
            if (fast_forward && UI.audio) SDL_ClearQueuedAudio(UI.audio);
            ui_toast(T(fast_forward ? "hotkey.ff_on" : "hotkey.ff_off"));
        } else if (in->pressed[BTN_X]) {
            screenshot();
            ui_toast(T("hotkey.screenshot"));
        } else if (in->pressed[BTN_UP] || in->pressed[BTN_DOWN]) {
            int v = UI.set.volume + (in->pressed[BTN_UP] ? 1 : -1);
            UI.set.volume = v < 0 ? 0 : v > 20 ? 20 : v;
            osd_kind = OSD_VOLUME;
            osd_until = UI.t + 1.4;
        } else if (in->pressed[BTN_LEFT] || in->pressed[BTN_RIGHT]) {
            int v = UI.set.brightness + (in->pressed[BTN_RIGHT] ? 1 : -1);
            UI.set.brightness = v < 1 ? 1 : v > 10 ? 10 : v;
            osd_kind = OSD_BRIGHT;
            osd_until = UI.t + 1.4;
        } else used = false;
        if (used) {
            menu_combo = true;
            settings_save(&UI.set, root);
        }
        return false;
    }
    /* rilasciato: apre il menu solo se non è servito a una scorciatoia */
    if (released && !menu_combo) {
        fast_forward = false;
        return true;
    }
    return false;
}

/* ------------------------------------------------------- batteria */
/* Sotto il 10% un avviso; sotto il 4% la partita viene salvata (stato automatico e
   ripresa all'accensione), così non si perde nulla se la console si spegne da sola. */
static void battery_watch(void) {
    static double next;
    static bool warned, saved;
    if (UI.t < next) return;
    next = UI.t + 5;
    bool charging;
    int pct = ui_battery(&charging);
    if (charging) { warned = saved = false; return; }
    if (pct < 0) return;   /* livello sconosciuto: niente avvisi né salvataggi per batteria scarica */
    if (pct <= 10 && !warned) {
        char m[96];
        snprintf(m, sizeof m, T("battery.low"), pct);
        ui_toast(m);
        warned = true;
    }
    if (pct <= 4 && !saved && state_save(true)) {
        write_resume();
        ui_toast(T("battery.saved"));
        saved = true;
    }
}

static void ask_do(int kind) {
    if (kind == ASK_OVERWRITE) {
        saves_write(ask_row);
    } else if (kind == ASK_RESTART) {
        core.reset();
        menu_open = false;
    } else {
        memcpy(gcfg.map, default_map, sizeof gcfg.map);
        gamecfg_save();
        ui_toast(T("game.controls_restored"));
        ui_sound(SND_OK);
    }
}

/* tasti F1 e F2 della Brick Pro: l'azione si sceglie in Impostazioni > Comandi */
static void extra_key(int action) {
    switch (action) {
    case XK_SAVE: ui_toast(T(state_save(false) ? "game.state_saved" : "game.save_failed")); break;
    case XK_LOAD: ui_toast(T(state_load(false) ? "game.state_loaded" : "game.no_state")); break;
    case XK_SHOT: screenshot(); ui_toast(T("hotkey.screenshot")); break;
    default: break;
    }
}

/* ------------------------------------------------------------ main */

static void show_error(const char *msg) {
    ui_log("errore: %s", msg);
    for (int i = 0; i < (UI.automation ? 2 : 100000) && !UI.in.quit; i++) {
        ui_poll();
        if (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B] || UI.in.pressed[BTN_MENU]) break;
        ui_begin();
        ui_text(UI.f_title, T("common.launch_error"), UI.w / 2, UI.h / 3, UI.th.text, ALIGN_CENTER);
        ui_text(UI.f_normal, msg, UI.w / 2, UI.h / 3 + M(0.09f), UI.th.text_dim, ALIGN_CENTER);
        ui_text(UI.f_small, T("game.error_hint"), UI.w / 2, UI.h / 3 + M(0.18f), UI.th.text_dim, ALIGN_CENTER);
        ui_end();
        SDL_Delay(16);
    }
}

/* FBNeo non rifiuta i giochi che non può avviare: risponde di averli caricati e disegna una
   sua schermata d'errore su un'area 640x480. La si riconosce da quella misura insieme a un
   errore del core (ROM mancanti, gioco non funzionante) o all'assenza di opzioni (gioco ignoto). */
static bool core_refused_game(void) {
    return !strcmp(core_id, "fbneo") && av.geometry.base_width == 640 && av.geometry.base_height == 480
        && (core_last_error[0] || ncopts == 0);
}

/* Il launcher legge questo file al ritorno e spiega cosa impedisce di avviare il gioco. */
static bool write_failure(const char *stage) {
    char p[1100];
    snprintf(p, sizeof p, "%s/System/last-failure.txt", root);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s\t%s\t%s\n", stage, rom_path, core_last_error) > 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

int main(int argc, char **argv) {
    const char *auto_script = NULL;
    bool resume = false;
    snprintf(root, sizeof root, "sdcard");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) snprintf(core_path, sizeof core_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) snprintf(rom_path, sizeof rom_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) snprintf(title, sizeof title, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_script = argv[++i];
        else if (!strcmp(argv[i], "--resume")) resume = true;
    }
    const char *base = strrchr(rom_path, '/');
    snprintf(rom_name, sizeof rom_name, "%s", base ? base + 1 : rom_path);
    char *dot = strrchr(rom_name, '.');
    if (dot) *dot = 0;
    if (!title[0]) snprintf(title, sizeof title, "%s", rom_name);
    const char *cb = strrchr(core_path, '/');
    snprintf(core_id, sizeof core_id, "%s", cb ? cb + 1 : core_path);
    char *us = strstr(core_id, "_libretro");
    if (us) *us = 0;
    snprintf(sys_dir, sizeof sys_dir, "%s/Bios", root);
    snprintf(save_dir, sizeof save_dir, "%s/Saves", root);
    mkdir(sys_dir, 0755);
    mkdir(save_dir, 0755);
    snprintf(save_dir, sizeof save_dir, "%s/Saves/%s", root, core_id);
    mkdir(save_dir, 0755);

    settings_t early;
    settings_load(&early, root);
    snprintf(lang_code, sizeof lang_code, "%s", early.lang);
    /* OpenGL (ES sulla console) come renderer: serve agli shader dei filtri */
#ifdef __APPLE__
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#else
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#endif
    /* Il core va caricato prima della finestra: la frequenza audio la decide lui. */
    bool core_ok = load_core(), ok = core_ok && load_game() && !core_refused_game();
    if (!ui_init("Arcade", root, auto_script, ok ? (int)lround(av.timing.sample_rate) : 48000, false)) return 1;
    ui_log("gioco: %s (%s)", title, rom_path);
    if (!ok) {
        ui_log("gioco non avviato: %s", core_last_error);
        if (!write_failure(core_ok ? "game" : "core")) show_error(T("game.error_body"));
        ui_shutdown_code(EXIT_MENU);
        return EXIT_MENU;
    }
    gamecfg_load();
    emu_cpu = emulator_cpu();
    apply_cpu();
    filter_apply();
    audio_on = UI.audio && !UI.automation;
    if (resume && state_load(true)) ui_toast(T("game.resumed"));

    double fps = av.timing.fps > 0 ? av.timing.fps : 60;
    Uint32 frame_bytes = (Uint32)(av.timing.sample_rate / fps) * 4;
    rate_target = frame_bytes * 2.5;
    UI.vol_bar = true;
    Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
    while (exit_code < 0) {
        ui_poll();
        if (UI.in.vol_changed) { osd_kind = OSD_VOLUME; osd_until = UI.t + 1.4; }
        if (UI.in.quit) { power_off_from_game(); break; }   /* chiusura finestra = tasto di accensione */
        /* tasti extra della Brick Pro: HOME torna al launcher, F1 salva, F2 carica */
        if (UI.in.home && UI.set.key_home == XH_LAUNCHER) { exit_code = EXIT_MENU; break; }
        /* tasto di accensione (sistema su SD): breve chiede se sospendere (il gioco riprende da
           dove era), tenuto premuto salva la partita e spegne */
        if (UI.in.power_long) { power_off_from_game(); break; }
        if (suspend_ask) {
            int r = ui_confirm_input(&ask_sel);
            if (r >= 0) suspend_ask = false;
            if (r == 1) { ui_suspend(); next = SDL_GetPerformanceCounter(); continue; }
        } else if (UI.in.power) {
            suspend_ask = true;
            ask_sel = 1;
            if (UI.audio) SDL_ClearQueuedAudio(UI.audio);
            ui_sound(SND_OK);
        }
        if (UI.in.f1 && !menu_open && !suspend_ask) extra_key(UI.set.key_f1);
        if (UI.in.f2 && !menu_open && !suspend_ask) extra_key(UI.set.key_f2);
        if (suspend_ask) {
            /* il gioco resta fermo finché non si risponde */
        } else if (ask_kind) {
            int r = ui_confirm_input(&ask_kind_sel);
            if (r >= 0) { int k = ask_kind; ask_kind = 0; if (r == 1) ask_do(k); }
        } else if (!menu_open && ((UI.in.home && UI.set.key_home == XH_MENU) || hotkeys())) {
            menu_open = true;
            menu_anim = 0;
            page = PAGE_MAIN;
            sel[PAGE_MAIN] = MI_RESUME;
            scroll[PAGE_MAIN] = hl[PAGE_MAIN] = 0;
            if (UI.audio) SDL_ClearQueuedAudio(UI.audio);
            ui_sound(SND_OK);
        } else if (menu_open) {
            menu_input();
        }
        battery_watch();
        Uint64 pf0 = SDL_GetPerformanceCounter();
        if (!menu_open && !suspend_ask) {
            for (int k = 0; k < (fast_forward ? 3 : 1); k++) core.run();
            played += UI.dt;
            /* diagnosi (sistema su SD): dopo 5 s di gioco si salva ciò che è stato disegnato */
            static bool shot_done;
            if (!shot_done && played > 5 && (getenv("ARCADE_DEBUG_SHOT") || ui_debug_on(2))) {
                char p[1200];
                snprintf(p, sizeof p, "%s/Logs/gioco.png", root);
                ui_screenshot(p);
                ui_log("diagnosi: istantanea del gioco in %s", p);
                shot_done = true;
            }
        }

        Uint64 pf1 = SDL_GetPerformanceCounter();
        ui_begin();
        draw_game();
        if (menu_open) menu_draw();
        if (ask_kind == ASK_OVERWRITE) ui_confirm_draw(T("confirm.overwrite_title"), T("confirm.overwrite_body"), T("common.cancel"), T("saves.save"), ask_kind_sel);
        if (ask_kind == ASK_RESTART) ui_confirm_draw(T("confirm.restart_title"), T("confirm.restart_body"), T("common.cancel"), T("game.reset"), ask_kind_sel);
        if (ask_kind == ASK_CONTROLS) ui_confirm_draw(T("confirm.controls_title"), T("confirm.controls_body"), T("common.cancel"), T("confirm.restore"), ask_kind_sel);
        if (suspend_ask) ui_confirm_draw(T("dialog.suspend_title"), T("dialog.suspend_body"), T("common.cancel"), T("dialog.suspend"), ask_sel);
        osd_draw();
        ui_end();
        if (!menu_open && !suspend_ask && !fast_forward) perf_frame(pf0, pf1, SDL_GetPerformanceCounter(), audio_on ? SDL_GetQueuedAudioSize(UI.audio) : 0, frame_bytes, fps);

        /* ritmo: l'audio in coda fa da orologio; senza audio si usa il timer */
        if (UI.automation) continue;
        if (audio_on && !menu_open && !suspend_ask && !fast_forward) {
            /* se la scheda audio non consuma (driver bloccato), dopo 250 ms si continua
               senza audio con il timer: meglio un gioco muto che un gioco fermo */
            Uint32 t0 = SDL_GetTicks();
            while (SDL_GetQueuedAudioSize(UI.audio) > frame_bytes * 3) {
                if (SDL_GetTicks() - t0 > 250) {
                    ui_log("audio fermo: la scheda non consuma i campioni, continuo senza audio");
                    ui_toast(T("game.audio_stalled"));
                    audio_on = false;
                    SDL_ClearQueuedAudio(UI.audio);
                    next = SDL_GetPerformanceCounter();
                    break;
                }
                SDL_Delay(1);
            }
        } else {
            next += (Uint64)(freq / fps);
            Uint64 now = SDL_GetPerformanceCounter();
            if (next > now) SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else next = now;
        }
    }
    ui_log("uscita (%d), giocato %.0f s", exit_code, played);
    ui_busy(T(exit_code == EXIT_POWEROFF ? "common.shutting_down" : "common.leaving"), 0.35);
    stats_update();
    core.unload_game();
    core.deinit();
    free(rom_data);
    ui_shutdown_code(exit_code);
    return exit_code;
}

/* i tasti di ui.h (BTN_A, BTN_LEFT...) non devono essere coperti da <linux/input.h> */
#ifdef BTN_TRIGGER_HAPPY
#error "linux/input.h incluso: coprirebbe i tasti di ui.h"
#endif
