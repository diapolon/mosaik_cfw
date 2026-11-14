#include "music.h"
#include "../common/ui.h"
#include "../common/platform.h"
#include <SDL_mixer.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>

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

/* Passi dell'avvio della musica, con un cane da guardia (14 novembre 2026: sulla Brick Pro il
   launcher restava fermo su "caricamento" quando la musica del menu partiva all'accensione).
   I passi dell'apertura dell'audio vanno nel registro; se un passo dura più di WD_SECONDS il thread di guardia scrive in
   Logs/musica-blocco.txt il passo, lo stato dei thread del launcher e della scheda audio (da /proc),
   lascia System/musica-bloccata.txt e chiude il launcher: system-init lo riavvia e, con quel file,
   all'accensione la musica non parte più (si può sempre farla partire da Impostazioni). */
#define WD_SECONDS 12
static const char *_Atomic wd_step;
static _Atomic Uint32 wd_since;
static SDL_Thread *wd_thread;

static void cat_file(FILE *out, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(out, "  (%s non leggibile)\n", path); return; }
    char line[512];
    int n = 0;
    while (n++ < 60 && fgets(line, sizeof line, f)) fprintf(out, "  %s", line);
    fclose(f);
}

static void wd_dump(const char *step, Uint32 ms) {
    char p[1200];
    snprintf(p, sizeof p, "%s/Logs/musica-blocco.txt", UI.root);
    FILE *o = fopen(p, "a");
    if (!o) return;
    time_t now = time(NULL);
    fprintf(o, "=== %s", ctime(&now));
    fprintf(o, "passo fermo: %s, da %.1f s\n", step, ms / 1000.0);
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char q[300], name[64] = "?", wchan[128] = "?";
        FILE *f;
        snprintf(q, sizeof q, "/proc/self/task/%s/comm", e->d_name);
        if ((f = fopen(q, "r"))) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
        snprintf(q, sizeof q, "/proc/self/task/%s/wchan", e->d_name);
        if ((f = fopen(q, "r"))) { if (fgets(wchan, sizeof wchan, f)) wchan[strcspn(wchan, "\n")] = 0; fclose(f); }
        fprintf(o, "thread %s %s, wchan %s\n", e->d_name, name, wchan);
        snprintf(q, sizeof q, "/proc/self/task/%s/stack", e->d_name);
        cat_file(o, q);
    }
    if (d) closedir(d);
    static const char *snd[] = { "/proc/asound/cards", "/proc/asound/card0/pcm0p/sub0/status",
                                 "/proc/asound/card0/pcm0p/sub0/hw_params", "/proc/asound/card0/pcm0p/sub0/sw_params" };
    for (size_t i = 0; i < sizeof snd / sizeof snd[0]; i++) { fprintf(o, "%s:\n", snd[i]); cat_file(o, snd[i]); }
    fflush(o);
    fsync(fileno(o));
    fclose(o);
}

static int wd_run(void *arg) {
    (void)arg;
    for (;;) {
        SDL_Delay(250);
        const char *st = atomic_load(&wd_step);
        if (!st) return 0;   /* la musica è partita (o ha rinunciato) */
        Uint32 ms = SDL_GetTicks() - atomic_load(&wd_since);
        if (ms < WD_SECONDS * 1000u) continue;
        ui_log("musica: ferma da %.0f s nel passo \"%s\": dettagli in Logs/musica-blocco.txt, il launcher riparte senza musica", ms / 1000.0, st);
        wd_dump(st, ms);
        char p[1200];
        snprintf(p, sizeof p, "%s/System/musica-bloccata.txt", UI.root);
        FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "%s\n", st); fflush(f); fsync(fileno(f)); fclose(f); }
        sync();
        _exit(3);
    }
}

/* passo in corso; NULL quando l'avvio della musica è finito */
static void wd(const char *step) {
    if (step) {
        if (!mixer_on) ui_log("musica, passo: %s", step);   /* nel registro solo all'apertura dell'audio */
        atomic_store(&wd_since, SDL_GetTicks());
        atomic_store(&wd_step, step);
        if (!wd_thread) wd_thread = SDL_CreateThread(wd_run, "musica-guardia", NULL);
#ifndef TRIMUI_DEVICE
        const char *hang = getenv("ARCADE_MUSICA_BLOCCA");   /* prove sul Mac: si ferma qui */
        if (hang && *hang && strstr(step, hang)) for (;;) SDL_Delay(1000);
#endif
    } else {
        atomic_store(&wd_step, (const char *)NULL);
        if (wd_thread) { SDL_WaitThread(wd_thread, NULL); wd_thread = NULL; }
    }
}

/* posizione del flusso audio nella scheda (/proc/asound): se tra due letture non cambia, il
   chip non sta consumando e chiudere il flusso si bloccherebbe */
static long hw_ptr(void) {
    FILE *f = fopen("/proc/asound/card0/pcm0p/sub0/status", "r");
    if (!f) return -1;
    char line[128];
    long v = -1;
    bool running = false;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "state:", 6)) running = strstr(line, "RUNNING") != NULL;
        if (!strncmp(line, "hw_ptr", 6)) { char *c = strchr(line, ':'); if (c) v = strtol(c + 1, NULL, 10); }
    }
    fclose(f);
    return running ? v : -1;   /* un flusso non ancora partito non è fermo */
}

static bool mixer_open(void) {
    if (mixer_on) return true;
    wd("Mix_Init");
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC | MIX_INIT_MOD | MIX_INIT_OPUS);
    /* All'accensione system-init lascia spente le uscite audio (arcade-suspend mute) finché il
       launcher non è pronto, e il flusso dei menu, già aperto, non avanza: chiuderlo per aprire
       quello della musica bloccava il launcher su "caricamento" (14 novembre 2026). Si
       riaccendono le uscite e si aspetta che il flusso si muova; se resta fermo, niente musica. */
    plat_audio_release();
    long p0 = hw_ptr();
    if (p0 >= 0) {
        long p1 = p0;
        for (int i = 0; i < 12 && p1 == p0; i++) { SDL_Delay(50); p1 = hw_ptr(); }
        ui_log("musica: flusso dei menu %s (hw_ptr %ld -> %ld)", p1 != p0 ? "in movimento" : "FERMO", p0, p1);
        if (p1 == p0) {   /* chiuderlo bloccherebbe il launcher: per ora niente musica */
            ui_log("musica: non parte finché il flusso dei menu è fermo");
            return false;
        }
    }
    wd("chiusura dell'audio dei menu (SDL_CloseAudioDevice)");
    ui_audio_takeover(play_fx);
    wd("apertura dell'audio della musica (Mix_OpenAudioDevice)");
    if (Mix_OpenAudioDevice(UI.audio_rate, AUDIO_S16SYS, 2, 1024, NULL, 0) < 0) {
        ui_log("musica: audio non disponibile (%s)", Mix_GetError());
        wd("riapertura dell'audio dei menu");
        ui_audio_takeover(NULL);
        return false;
    }
    wd("Mix_AllocateChannels");
    Mix_AllocateChannels(2);
    Mix_HookMusicFinished(on_finished);
    mixer_on = true;
    return true;
}

/* un brano preciso da un punto preciso (quello che suonava nel Lettore musicale) */
static bool music_play_from_steps(const char *path, double pos) {
    if (!mixer_open()) return false;
    wd("arresto del brano precedente");
    if (mus) { Mix_HaltMusic(); Mix_FreeMusic(mus); mus = NULL; }
    finished = false;
    wd("Mix_LoadMUS");
    mus = Mix_LoadMUS(path);
    if (!mus) { ui_log("musica: %s non leggibile (%s)", path, Mix_GetError()); return false; }
    const char *base = strrchr(path, '/');
    snprintf(title, sizeof title, "%s", base ? base + 1 : path);
    char *dot = strrchr(title, '.');
    if (dot) *dot = 0;
    last_vol = (int)(MIX_MAX_VOLUME * UI.set.music_vol / 10.0f * ui_gain());
    Mix_VolumeMusic(last_vol);
    wd("Mix_FadeInMusicPos");
    if (Mix_FadeInMusicPos(mus, 0, 800, pos > 0 ? pos : 0) < 0) {
        ui_log("musica: %s non parte (%s)", path, Mix_GetError());
        Mix_FreeMusic(mus);
        mus = NULL;
        title[0] = 0;
        return false;
    }
    ui_log("musica: continuo dal Lettore musicale %s da %.0f s", title, pos);
    return true;
}

static bool music_play_from(const char *path, double pos) {
    bool ok = music_play_from_steps(path, pos);
    wd(NULL);
    return ok;
}

void music_init(const char *root) {
    char dir[1100];
    /* nota del Lettore musicale appena chiuso (apps/music.c, issue #8): continuare il suo brano
       o restare in silenzio; vale una volta sola */
    char hp[1100], line[1400] = "";
    snprintf(hp, sizeof hp, "%s/System/musica-app.txt", root);
    FILE *hf = fopen(hp, "r");
    if (hf) {
        if (!fgets(line, sizeof line, hf)) line[0] = 0;
        fclose(hf);
        unlink(hp);
        line[strcspn(line, "\r\n")] = 0;
    }
    bool silent = !strcmp(line, "silenzio");
    bool resumed = false;
    if (!strncmp(line, "riprendi\t", 9) && UI.set.music_vol > 0) {
        char *path = line + 9, *tab = strrchr(path, '\t');
        if (tab && tab != path) {
            *tab = 0;
            resumed = music_play_from(path, atof(tab + 1));
        }
    }
    if (silent) ui_log("musica: il Lettore musicale era in pausa, menu in silenzio");
    /* la musica del menu sta in Music/Menu: il resto di Music è del Lettore musicale (prima il menu
       suonava anche i brani copiati per l'app, issue #8); la cartella si crea per farla trovare */
    snprintf(dir, sizeof dir, "%s/Music", root);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Music/Menu", root);
    mkdir(dir, 0755);
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
    ui_log("musica: %d brani in Music/Menu", ntracks);
    /* all'avvio prima la musica si era bloccata (cane da guardia): all'accensione non parte */
    char bp[1100], why[200] = "";
    snprintf(bp, sizeof bp, "%s/System/musica-bloccata.txt", root);
    FILE *bf = fopen(bp, "r");
    if (bf) {
        if (fgets(why, sizeof why, bf)) why[strcspn(why, "\r\n")] = 0;
        fclose(bf);
        ui_log("musica: si era bloccata (%s), all'accensione resta spenta", why);
    }
    if (ntracks && UI.set.music_vol > 0 && !silent && !resumed && !bf) music_next();
}

static void music_next_steps(void) {
    if (!ntracks || !mixer_open()) return;
    for (int tries = 0; tries < ntracks; tries++) {
        cur = (cur + 1) % ntracks;
        wd("arresto del brano precedente");
        if (mus) { Mix_HaltMusic(); Mix_FreeMusic(mus); mus = NULL; }
        finished = false;   /* l'arresto qui sopra chiama on_finished: non è una fine brano */
        wd("Mix_LoadMUS");
        mus = Mix_LoadMUS(tracks[cur]);
        if (!mus) { ui_log("musica: %s non leggibile (%s)", tracks[cur], Mix_GetError()); continue; }
        const char *base = strrchr(tracks[cur], '/');
        snprintf(title, sizeof title, "%s", base ? base + 1 : tracks[cur]);
        char *dot = strrchr(title, '.');
        if (dot) *dot = 0;
        last_vol = (int)(MIX_MAX_VOLUME * UI.set.music_vol / 10.0f * ui_gain());
        wd("Mix_VolumeMusic");
        Mix_VolumeMusic(last_vol);
        wd("Mix_FadeInMusic");
        Mix_FadeInMusic(mus, 0, 800);
        ui_log("musica: %s", title);
        return;
    }
    title[0] = 0;
}

void music_next(void) {
    music_next_steps();
    wd(NULL);
    if (title[0]) {   /* è partita: il blocco di un avvio precedente non conta più */
        char p[1200];
        snprintf(p, sizeof p, "%s/System/musica-bloccata.txt", UI.root);
        unlink(p);
    }
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
