#include "music.h"
#include "../common/ui.h"
#include <SDL_mixer.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static char (*tracks)[512];
static int ntracks, cur = -1, last_vol = -1;
static Mix_Music *mus;
static _Atomic bool finished;
static bool mixer_on;
static char title[128];

/* suoni dell'interfaccia: una copia del PCM per il canale 0 (SDL_mixer non la copia) */
static Mix_Chunk *fx;
static Uint8 *fx_buf;

static void play_fx(const Sint16 *stereo, int frames) {
    Mix_HaltChannel(0);
    if (fx) Mix_FreeChunk(fx);
    free(fx_buf);
    fx = NULL;
    fx_buf = malloc((size_t)frames * 4);
    if (!fx_buf) return;
    memcpy(fx_buf, stereo, (size_t)frames * 4);
    fx = Mix_QuickLoad_RAW(fx_buf, (Uint32)frames * 4);
    if (fx) Mix_PlayChannel(0, fx, 0);
}

static void on_finished(void) { finished = true; }

static bool is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    static const char *ext[] = { "mp3", "ogg", "flac", "wav" };
    if (!dot || name[0] == '.') return false;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot + 1, ext[i])) return true;
    return false;
}

static bool mixer_open(void) {
    if (mixer_on) return true;
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC | MIX_INIT_MOD | MIX_INIT_OPUS);
    ui_audio_takeover(play_fx);
    if (Mix_OpenAudioDevice(UI.audio_rate, AUDIO_S16SYS, 2, 1024, NULL, 0) < 0) {
        ui_log("musica: audio non disponibile (%s)", Mix_GetError());
        ui_audio_takeover(NULL);
        return false;
    }
    Mix_AllocateChannels(2);
    Mix_HookMusicFinished(on_finished);
    mixer_on = true;
    return true;
}

void music_init(const char *root) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Music", root);
    DIR *d = opendir(dir);
    if (!d) return;
    int cap = 64;
    tracks = malloc(sizeof *tracks * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_audio(e->d_name)) continue;
        if (ntracks == cap) tracks = realloc(tracks, sizeof *tracks * (size_t)(cap *= 2));
        snprintf(tracks[ntracks++], 512, "%s/%s", dir, e->d_name);
    }
    closedir(d);
    /* ordine casuale, diverso a ogni avvio */
    srand((unsigned)time(NULL));
    for (int i = ntracks - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char t[512];
        memcpy(t, tracks[i], 512); memcpy(tracks[i], tracks[j], 512); memcpy(tracks[j], t, 512);
    }
    ui_log("musica: %d brani", ntracks);
    if (ntracks && UI.set.music_vol > 0) music_next();
}

void music_next(void) {
    if (!ntracks || !mixer_open()) return;
    for (int tries = 0; tries < ntracks; tries++) {
        cur = (cur + 1) % ntracks;
        if (mus) { Mix_HaltMusic(); Mix_FreeMusic(mus); mus = NULL; }
        finished = false;   /* l'arresto qui sopra chiama on_finished: non è una fine brano */
        mus = Mix_LoadMUS(tracks[cur]);
        if (!mus) { ui_log("musica: %s non leggibile (%s)", tracks[cur], Mix_GetError()); continue; }
        const char *base = strrchr(tracks[cur], '/');
        snprintf(title, sizeof title, "%s", base ? base + 1 : tracks[cur]);
        char *dot = strrchr(title, '.');
        if (dot) *dot = 0;
        last_vol = (int)(MIX_MAX_VOLUME * UI.set.music_vol / 10.0f * ui_gain());
        Mix_VolumeMusic(last_vol);
        Mix_FadeInMusic(mus, 0, 800);
        ui_log("musica: %s", title);
        return;
    }
    title[0] = 0;
}

void music_update(void) {
    if (!mixer_on) return;
    if (finished && UI.set.music_vol > 0) { music_next(); return; }
    int vol = (int)(MIX_MAX_VOLUME * UI.set.music_vol / 10.0f * ui_gain());
    if (vol != last_vol) { Mix_VolumeMusic(vol); last_vol = vol; }
}

void music_stop(void) {
    if (mixer_on && mus) Mix_FadeOutMusic(400);
    title[0] = 0;
}

void music_quit(void) {
    if (!mixer_on) return;
    Mix_HookMusicFinished(NULL);
    Mix_HaltMusic();
    Mix_HaltChannel(-1);
    if (mus) Mix_FreeMusic(mus);
    if (fx) Mix_FreeChunk(fx);
    free(fx_buf);
    Mix_CloseAudio();
    Mix_Quit();
    mixer_on = false;
    ui_audio_takeover(NULL);
}

int music_count(void) { return ntracks; }
const char *music_title(void) { return title; }
