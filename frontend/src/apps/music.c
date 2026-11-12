/* Lettore musicale: app di Mosaik, pacchetto dello Store (Arcade/Apps/music).

   I brani della cartella Music (anche nelle sottocartelle, una per album): MP3, OGG, FLAC, WAV,
   in ordine naturale. A suona il brano scelto o mette in pausa quello in corso, L/R brano
   precedente e successivo, sinistra/destra tenuti indietro e avanti di 10 secondi, X ripeti
   (no, tutti, uno), Y casuale, SELECT spegne lo schermo mentre la musica continua (un tasto
   qualsiasi lo riaccende), B o MENU escono. Il volume è quello della console (tasti laterali).

   Sul Mac: build/music --root sdcard --app package/apps/music [--auto "..."] */
#include "appkit.h"
#include <SDL_mixer.h>
#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "../common/platform.h"

typedef struct { char rel[512]; char title[160]; char album[96]; } track_t;
static track_t *tracks;
static int ntracks, cap_tracks, sel, cur = -1;
static float scroll;
static Mix_Music *mus;
static _Atomic bool finished;
static bool mixer_on, paused, shuffle, screen_off, quit;
static int repeat;   /* 0 no, 1 tutti, 2 uno */
static double duration;

/* ------------------------------------------------------------ brani */

static bool is_audio(const char *n) {
    const char *dot = strrchr(n, '.');
    static const char *ext[] = { "mp3", "ogg", "flac", "wav" };
    if (!dot || n[0] == '.') return false;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot + 1, ext[i])) return true;
    return false;
}

static int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)(ea - a) - (int)(eb - b);
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c) return c;
            a = ea;
            b = eb;
            continue;
        }
        int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (c) return c;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int track_cmp(const void *a, const void *b) { return natcmp(((const track_t *)a)->rel, ((const track_t *)b)->rel); }

static void scan(const char *dir, const char *rel, int depth) {
    DIR *d = depth < 4 ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1600], r[600];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        else snprintf(r, sizeof r, "%s", e->d_name);
        if (stat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { scan(p, r, depth + 1); continue; }
        if (!is_audio(e->d_name)) continue;
        if (ntracks == cap_tracks) {
            int nc = cap_tracks ? cap_tracks * 2 : 128;
            track_t *nt = realloc(tracks, (size_t)nc * sizeof *nt);
            if (!nt) break;
            tracks = nt;
            cap_tracks = nc;
        }
        track_t *t = &tracks[ntracks++];
        snprintf(t->rel, sizeof t->rel, "%s", r);
        snprintf(t->title, sizeof t->title, "%s", e->d_name);
        char *dot = strrchr(t->title, '.');
        if (dot) *dot = 0;
        snprintf(t->album, sizeof t->album, "%s", rel);   /* la sottocartella, se c'è */
    }
    if (d) closedir(d);
}

/* ------------------------------------------------------------ riproduzione */

/* suoni dell'interfaccia sul canale 0 del mixer (SDL_mixer non copia il PCM) */
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

static bool mixer_open(void) {
    if (mixer_on) return true;
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC);
    ui_audio_takeover(play_fx);   /* l'uscita audio passa al mixer: i suoni dei tasti anche */
    if (Mix_OpenAudioDevice(UI.audio_rate ? UI.audio_rate : 48000, AUDIO_S16SYS, 2, 2048, NULL, 0) < 0) {
        ui_log("musica: audio non disponibile (%s)", Mix_GetError());
        ui_audio_takeover(NULL);
        return false;
    }
    Mix_AllocateChannels(1);
    Mix_HookMusicFinished(on_finished);
    mixer_on = true;
    return true;
}

static void volume_apply(void) {
    static int last = -1;
    int v = (int)(MIX_MAX_VOLUME * ui_gain());
    if (v != last) { Mix_VolumeMusic(v); last = v; }
}

static bool play(int i) {
    if (i < 0 || i >= ntracks || !mixer_open()) return false;
    if (mus) { Mix_HookMusicFinished(NULL); Mix_HaltMusic(); Mix_FreeMusic(mus); mus = NULL; Mix_HookMusicFinished(on_finished); }
    finished = false;
    char p[1700];
    snprintf(p, sizeof p, "%s/Music/%s", app_root, tracks[i].rel);
    mus = Mix_LoadMUS(p);
    if (!mus) { ui_log("musica: %s non leggibile (%s)", tracks[i].rel, Mix_GetError()); return false; }
    cur = i;
    paused = false;
    duration = Mix_MusicDuration(mus);
    volume_apply();
    Mix_PlayMusic(mus, 0);
    ui_log("musica: %s", tracks[i].rel);
    return true;
}

/* brano dopo quello in corso, secondo ripeti e casuale; -1 alla fine */
static int next_index(int dir, bool user) {
    if (!ntracks) return -1;
    if (repeat == 2 && !user) return cur;
    if (shuffle && ntracks > 1) {
        int n;
        do n = rand() % ntracks; while (n == cur);
        return n;
    }
    int n = cur + dir;
    if (n >= ntracks) return repeat == 1 || user ? 0 : -1;
    if (n < 0) return ntracks - 1;
    return n;
}

static void skip(int dir, bool user) {
    for (int tries = 0; tries < ntracks; tries++) {
        int n = next_index(dir, user);
        if (n < 0) { cur = -1; return; }
        if (play(n)) { sel = n; return; }
        cur = n;   /* illeggibile: si passa al successivo */
    }
}

static double position(void) { return mus ? Mix_GetMusicPosition(mus) : 0; }

static void seek(double delta) {
    if (!mus) return;
    double p = position() + delta;
    if (p < 0) p = 0;
    if (duration > 0 && p > duration - 1) p = duration - 1;
    Mix_SetMusicPosition(p);
}

/* ------------------------------------------------------------ disegno */

static void mmss(double s, char *out, size_t n) {
    if (s < 0) s = 0;
    int t = (int)s;
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    ui_topbar(FT("music.title"));
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small, *ft = UI.f_title;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), fth = TTF_FontHeight(ft);
    /* in fondo: il brano in corso con la barra */
    int ph = fth + fsh + M(0.05f) + M(0.03f), py = UI.h - ui_keybar_h() - ph - M(0.01f);
    char fit[300], a[16], b[16];
    ui_rrect(mx - M(0.012f), py, W + M(0.024f), ph, M(t->panel_radius), with_alpha(t->text_dim, 30));
    if (cur >= 0) {
        ui_text_fit(ft, tracks[cur].title, W - M(0.04f), fit, sizeof fit);
        ui_text(ft, fit, mx + M(0.01f), py + M(0.012f), t->text, ALIGN_LEFT);
        double pos = position();
        mmss(pos, a, sizeof a);
        mmss(duration, b, sizeof b);
        int by = py + M(0.012f) + fth + M(0.02f), bh = M(0.012f), tw = ui_text_w(fs, "00:00") + M(0.02f);
        int bx = mx + M(0.01f) + tw, bw = W - 2 * tw - M(0.02f);
        ui_text(fs, a, mx + M(0.01f), by + bh / 2 - fsh / 2, t->text_dim, ALIGN_LEFT);
        ui_text(fs, duration > 0 ? b : "--:--", mx + W - M(0.01f), by + bh / 2 - fsh / 2, t->text_dim, ALIGN_RIGHT);
        ui_rrect(bx, by, bw, bh, bh / 2, with_alpha(t->text_dim, 60));
        if (duration > 0) ui_rrect(bx, by, (int)(bw * (pos < duration ? pos / duration : 1)), bh, bh / 2, t->accent);
        if (paused) ui_text(fs, FT("music.paused"), mx + W - M(0.01f), py + M(0.012f) + (fth - fsh) / 2, t->accent, ALIGN_RIGHT);
    } else ui_text(fn, FT(ntracks ? "music.choose" : "music.none"), mx + M(0.01f), py + (ph - fnh) / 2, t->text_dim, ALIGN_LEFT);
    /* modi: ripeti e casuale, sopra il brano */
    static const char *rk[3] = { "music.repeat_off", "music.repeat_all", "music.repeat_one" };
    char modes[200];
    snprintf(modes, sizeof modes, "%s  ·  %s", FT(rk[repeat]), FT(shuffle ? "music.shuffle_on" : "music.shuffle_off"));
    int top = M(t->topbar_h) + M(0.02f);
    ui_text(fs, modes, mx, top, t->text_dim, ALIGN_LEFT);
    char count[64];
    snprintf(count, sizeof count, FT("music.count"), ntracks);
    ui_text(fs, count, mx + W, top, t->text_dim, ALIGN_RIGHT);
    /* elenco */
    int ly = top + fsh + M(0.02f), rh = fnh + M(0.024f), vis = (py - M(0.015f) - ly) / rh;
    if (!ntracks) {
        char lines[3][160];
        int n = ui_wrap(fn, FT("music.empty"), W, lines, 3);
        for (int k = 0; k < n; k++) ui_text(fn, lines[k], UI.w / 2, ly + rh + k * fnh, t->text_dim, ALIGN_CENTER);
    } else {
        int first = (int)scroll;
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        if (first < 0) first = 0;
        scroll = (float)first;
        for (int i = first; i < ntracks && i < first + vis; i++) {
            int ry = ly + (i - first) * rh;
            bool on = i == sel, playing = i == cur;
            if (on) ui_rrect(mx - M(0.012f), ry, W + M(0.024f), rh, M(t->panel_radius), with_alpha(t->accent, 230));
            color_t tc = on ? t->accent_text : playing ? t->accent : t->text, dc = on ? t->accent_text : t->text_dim;
            int is = fnh * 2 / 3, ix = mx + M(0.005f), iy = ry + (rh - is) / 2;
            if (playing && !paused) ui_triangle(ix, iy, ix, iy + is, ix + is, iy + is / 2, tc);   /* in riproduzione */
            else if (playing) { ui_rect(ix, iy, is / 3, is, tc); ui_rect(ix + is * 2 / 3, iy, is / 3, is, tc); }
            int aw = tracks[i].album[0] ? ui_text_w(fs, tracks[i].album) : 0;
            if (aw > W / 3) aw = W / 3;
            if (aw) { ui_text_fit(fs, tracks[i].album, W / 3, fit, sizeof fit); ui_text(fs, fit, mx + W, ry + (rh - fsh) / 2, dc, ALIGN_RIGHT); }
            ui_text_fit(fn, tracks[i].title, W - is - M(0.04f) - aw - M(0.02f), fit, sizeof fit);
            ui_text(fn, fit, ix + is + M(0.02f), ry + (rh - fnh) / 2, tc, ALIGN_LEFT);
        }
    }
    const char *h[] = { "B", FT("music.quit"), NULL, "SELECT", FT("music.screen_off"), "", "X", FT("music.repeat"), "",
                        "Y", FT("music.shuffle"), "", "A", FT(sel == cur && !paused ? "music.pause" : "music.play"), NULL };
    ui_keybar(h, 15);
}

/* ------------------------------------------------------------ comandi */

static double hold_t;

static void input(void) {
    input_t *in = &UI.in;
    if (screen_off) {   /* un tasto qualsiasi riaccende, senza fare altro */
        bool any = false;
        for (int b = 0; b < BTN_COUNT; b++) any |= in->pressed[b];
        if (any) { screen_off = false; plat_screen_off(false); }
        return;
    }
    int old = sel;
    if (in->pressed[BTN_UP] && ntracks) sel = (sel + ntracks - 1) % ntracks;
    if (in->pressed[BTN_DOWN] && ntracks) sel = (sel + 1) % ntracks;
    if (sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) { quit = true; return; }
    if (in->pressed[BTN_A] && ntracks) {
        if (sel == cur && mus) {
            if (paused) Mix_ResumeMusic(); else Mix_PauseMusic();
            paused = !paused;
        } else if (!play(sel)) { ui_toast(FT("music.bad_file")); ui_sound(SND_ERROR); }
    }
    if (in->pressed[BTN_R] && ntracks) skip(+1, true);
    if (in->pressed[BTN_L] && ntracks) {   /* dopo 3 secondi L ricomincia il brano, come i lettori */
        if (mus && position() > 3) Mix_SetMusicPosition(0); else skip(-1, true);
    }
    /* sinistra/destra: un salto di 10 s, poi ancora ogni 0,4 s se tenuti */
    int d = in->down[BTN_RIGHT] ? 1 : in->down[BTN_LEFT] ? -1 : 0;
    if (in->pressed[BTN_RIGHT] || in->pressed[BTN_LEFT]) { seek(d * 10.0); hold_t = UI.t + 0.6; }
    else if (d && UI.t > hold_t) { seek(d * 10.0); hold_t = UI.t + 0.4; }
    if (in->pressed[BTN_X]) { repeat = (repeat + 1) % 3; ui_sound(SND_OK); }
    if (in->pressed[BTN_Y]) { shuffle = !shuffle; ui_sound(SND_OK); }
    if (in->pressed[BTN_SELECT] && cur >= 0 && !paused) { screen_off = true; plat_screen_off(true); }
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "music")) return 1;
    srand((unsigned)time(NULL));
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Music", app_root);
    scan(dir, "", 0);
    if (ntracks > 1) qsort(tracks, (size_t)ntracks, sizeof *tracks, track_cmp);
    ui_log("musica: %d brani", ntracks);
    while (!quit) {
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        input();
        if (mixer_on) volume_apply();
        if (finished) { finished = false; skip(+1, false); }
        if (screen_off) { SDL_Delay(50); continue; }   /* schermo spento: niente disegno, poco processore */
        ui_begin();
        draw();
        ui_end();
    }
    plat_screen_off(false);
    if (mixer_on) {
        Mix_HookMusicFinished(NULL);
        Mix_HaltMusic();
        if (mus) Mix_FreeMusic(mus);
        Mix_CloseAudio();
    }
    ui_shutdown();
    return 0;
}
