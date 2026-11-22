/* Core libretro dimostrativo per le schermate del menu di gioco (toolchain/schermate.py): non legge
 * la ROM, disegna una scena sua in pixel art (cielo, sole a cerchi, città, un omino su una
 * piattaforma) e la anima appena. Scritto per Mosaik, nessun gioco vero. Salva e carica lo stato
 * (pochi byte), così le pagine dei salvataggi hanno qualcosa da mostrare.
 *   cc -shared -fPIC -O2 -o demo_libretro.dylib toolchain/schermate-core.c -I frontend/src/runner */
#include <stdint.h>
#include <string.h>
#include "libretro.h"

#define W 320
#define H 224
static uint32_t fb[W * H];
static unsigned frame;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_cb;
static retro_input_poll_t poll_cb;
static retro_environment_t env_cb;

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    bool no_rom = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}
RETRO_API void retro_get_system_info(struct retro_system_info *i) {
    memset(i, 0, sizeof *i);
    i->library_name = "Mosaik demo";
    i->library_version = "1";
    i->valid_extensions = "zip|nes|sfc|md|gb|gba";
    i->need_fullpath = true;
}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *a) {
    memset(a, 0, sizeof *a);
    a->geometry.base_width = a->geometry.max_width = W;
    a->geometry.base_height = a->geometry.max_height = H;
    a->geometry.aspect_ratio = 4.0f / 3.0f;
    a->timing.fps = 60;
    a->timing.sample_rate = 44100;
}
RETRO_API void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
RETRO_API void retro_reset(void) { frame = 0; }

static uint32_t rgb(int r, int g, int b) { return (uint32_t)(r << 16 | g << 8 | b); }
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H) fb[j * W + i] = c;
}
static void draw(void) {
    for (int y = 0; y < H; y++) {   /* cielo a bande */
        int t = y * 8 / H;
        rect(0, y, W, 1, rgb(30 + t * 14, 18 + t * 8, 60 + t * 6));
    }
    for (int k = 0; k < 50; k++) rect((k * 97) % W, (k * 53) % (H / 2), 1, 1, rgb(230, 230, 255));
    int cx = 230, cy = 70;   /* sole a cerchi */
    for (int r = 40, k = 0; r > 0; r -= 8, k++)
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++)
                if (x * x + y * y <= r * r) rect(cx + x, cy + y, 1, 1, rgb(250, 150 + k * 20, 60 + k * 40));
    for (int x = 0, k = 0; x < W; k++) {   /* città lontana e vicina */
        int w = 10 + (k * 7) % 14, h = 30 + (k * 13) % 40;
        rect(x, H - 40 - h, w, h + 40, rgb(60, 40, 80));
        x += w;
    }
    for (int x = 0, k = 0; x < W; k++) {
        int w = 14 + (k * 5) % 18, h = 10 + (k * 11) % 26;
        rect(x, H - 24 - h, w, h + 24, rgb(28, 18, 40));
        x += w;
    }
    rect(60, 150, 120, 8, rgb(240, 192, 92));   /* piattaforma e omino che saltella */
    int jy = (int)(frame / 6 % 8);
    jy = jy < 4 ? jy : 8 - jy;
    rect(110, 128 - jy * 2, 12, 14, rgb(80, 200, 220));
    rect(112, 120 - jy * 2, 8, 8, rgb(250, 220, 180));
    for (int k = 0; k < 5; k++) rect(20 + k * 60, 30, 6, 6, rgb(255, 230, 120));   /* monete */
}
RETRO_API void retro_run(void) {
    poll_cb();
    frame++;
    draw();
    video_cb(fb, W, H, W * sizeof(uint32_t));
    static int16_t silence[735 * 2];
    audio_cb(silence, 735);
}
RETRO_API bool retro_load_game(const struct retro_game_info *g) {
    (void)g;
    static const struct retro_input_descriptor d[] = {   /* nomi dei tasti per la pagina Comandi */
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Jump" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Throw" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Run" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Map" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Previous item" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Next item" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Pause" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" },
        { 0, 0, 0, 0, NULL } };
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)d);
    enum retro_pixel_format f = RETRO_PIXEL_FORMAT_XRGB8888;
    return env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
}
RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API size_t retro_serialize_size(void) { return sizeof frame; }
RETRO_API bool retro_serialize(void *d, size_t n) { if (n < sizeof frame) return false; memcpy(d, &frame, sizeof frame); return true; }
RETRO_API bool retro_unserialize(const void *d, size_t n) { if (n < sizeof frame) return false; memcpy(&frame, d, sizeof frame); return true; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
