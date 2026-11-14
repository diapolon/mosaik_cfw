/* Lettore musicale: app di Mosaik, pacchetto dello Store (Arcade/Apps/music).

   I brani della cartella Music (anche nelle sottocartelle, una per album, tranne Music/Menu che è
   la musica del menu di Mosaik): MP3, OGG, FLAC, WAV, in ordine naturale. Nell'elenco A suona il
   brano scelto e apre la sua schermata (copertina dal file o dalla cartella, titolo e artista dai
   tag, spettro, controlli); lì A mette in pausa, B
   torna all'elenco. Ovunque L/R brano precedente e successivo, sinistra/destra tenuti indietro e
   avanti di 10 secondi, X ripeti (no, tutti, uno), Y casuale, START cambia aspetto (cartella
   skins), SELECT spegne lo schermo mentre la musica continua (un tasto qualsiasi lo riaccende),
   MENU esce. Il volume è quello della console (tasti laterali).
   Uscendo, la musica si ferma e il menu di Mosaik resta in silenzio fino alla prossima accensione
   (System/musica-app.txt, issue #8).

   Sul Mac: build/music --root sdcard --app package/apps/music [--auto "..."] */
#include "appkit.h"
#include "imgdims.h"
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "../common/json.h"
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

static bool scan_cut;   /* qualcosa è rimasto fuori dall'elenco (nome troppo lungo, memoria) */
static void scan(const char *dir, const char *rel, int depth) {
    DIR *d = depth < 4 ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1600], r[600];
        struct stat sb;
        int lp = snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int lr = rel[0] ? snprintf(r, sizeof r, "%s/%s", rel, e->d_name) : snprintf(r, sizeof r, "%s", e->d_name);
        if (lp >= (int)sizeof p || lr >= (int)sizeof r || lr >= (int)sizeof tracks[0].rel) { scan_cut = true; continue; }
        if (stat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            if (depth == 0 && !strcasecmp(e->d_name, "Menu")) continue;   /* musica del menu di Mosaik */
            scan(p, r, depth + 1);
            continue;
        }
        if (!is_audio(e->d_name)) continue;
        if (ntracks == cap_tracks) {
            int nc = cap_tracks ? cap_tracks * 2 : 128;
            track_t *nt = realloc(tracks, (size_t)nc * sizeof *nt);
            if (!nt) { scan_cut = true; break; }
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
static void postmix(void *udata, Uint8 *stream, int len);
static void cover_load(int i);
static void silence_mark(void);
static int spec_rate = 48000, spec_ch = 2;

static bool mixer_open(void) {
    if (mixer_on) return true;
    /* quali codec ci sono davvero: un brano di un formato mancante non parte e lo dice il log */
    static bool told;
    int got = Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC);
    if (!told) { told = true; ui_log("musica: codec mp3 %s, ogg %s, flac %s", got & MIX_INIT_MP3 ? "sì" : "no", got & MIX_INIT_OGG ? "sì" : "no", got & MIX_INIT_FLAC ? "sì" : "no"); }
    ui_audio_takeover(play_fx);   /* l'uscita audio passa al mixer: i suoni dei tasti anche */
    if (Mix_OpenAudioDevice(UI.audio_rate ? UI.audio_rate : 48000, AUDIO_S16SYS, 2, 2048, NULL, 0) < 0) {
        ui_log("musica: audio non disponibile (%s)", Mix_GetError());
        ui_audio_takeover(NULL);
        return false;
    }
    Mix_AllocateChannels(1);
    Mix_HookMusicFinished(on_finished);
    Uint16 fmt;
    if (!Mix_QuerySpec(&spec_rate, &fmt, &spec_ch) || fmt != AUDIO_S16SYS) spec_ch = 0;   /* spettro solo in S16 */
    if (spec_ch) Mix_SetPostMix(postmix, NULL);
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
    if (!mus) {   /* niente brano in corso: stato fermo, non quello del brano di prima (M61-005) */
        ui_log("musica: %s non leggibile (%s)", tracks[i].rel, Mix_GetError());
        cur = -1;
        paused = false;
        duration = 0;
        cover_load(-1);
        return false;
    }
    cur = i;
    paused = false;
    duration = Mix_MusicDuration(mus);
    volume_apply();
    if (Mix_PlayMusic(mus, 0) < 0) {   /* codec o uscita che rifiutano: non è in riproduzione */
        ui_log("musica: %s non parte (%s)", tracks[i].rel, Mix_GetError());
        Mix_FreeMusic(mus);
        mus = NULL;
        cur = -1;
        duration = 0;
        cover_load(-1);
        return false;
    }
    ui_log("musica: %s", tracks[i].rel);
    plat_amp_wake();   /* dopo 4 s di silenzio l'altoparlante può essere spento a riposo (M61-003) */
    silence_mark();
    cover_load(i);
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
    cur = -1;   /* nessuno si legge: fermo */
}

static double position(void) { return mus ? Mix_GetMusicPosition(mus) : 0; }

/* Il menu di Mosaik non deve far ripartire la musica dopo il Lettore (issue #8): appena un brano
   parte si lascia il silenzio per questa accensione, così vale anche se l'app viene chiusa a
   forza o la playlist finisce (verifica Codex M61-004). /run/musica-silenzio è in RAM e sparisce
   spegnendo (launcher dalla 0.9.62); System/musica-app.txt lo leggono i launcher 0.9.60 e 0.9.61. */
static void silence_mark(void) {
    static bool done;
    if (done) return;
    done = true;
    FILE *f = fopen("/run/musica-silenzio", "w");
    if (f) fclose(f);
    char p[1200];
    snprintf(p, sizeof p, "%s/System/musica-app.txt", app_root);
    if ((f = fopen(p, "w"))) {
        fputs("silenzio\n", f);
        if (fclose(f) != 0) remove(p);
    }
}

/* Dopo una sospensione della console (gamewatch ferma l'app, arcade-suspend, poi la riprende)
   l'uscita audio può non rispondere più: il mixer si riapre e il brano riparte da dove era,
   in pausa se lo era (verifica audit). La sospensione si riconosce dal tempo: CLOCK_BOOTTIME la
   conta, CLOCK_MONOTONIC no. */
static void resume_check(void) {
#ifdef CLOCK_BOOTTIME
    struct timespec bt, mt;
    static double last = -1;
    clock_gettime(CLOCK_BOOTTIME, &bt);
    clock_gettime(CLOCK_MONOTONIC, &mt);
    double gap = (bt.tv_sec - mt.tv_sec) + (bt.tv_nsec - mt.tv_nsec) / 1e9;
    bool slept = last >= 0 && gap - last > 1.0;
    last = gap;
    if (!slept || !mixer_on) return;
    int idx = cur;
    bool was_paused = paused;
    double pos = position();
    ui_log("musica: ripresa dalla sospensione, riapro l'audio (brano %d, %.0f s)", idx, pos);
    Mix_HookMusicFinished(NULL);
    Mix_HaltMusic();
    if (mus) { Mix_FreeMusic(mus); mus = NULL; }
    Mix_CloseAudio();
    mixer_on = false;
    if (idx >= 0 && play(idx)) {
        if (pos > 0 && Mix_SetMusicPosition(pos) < 0) ui_log("musica: ripresa da %.0f s non riuscita (%s)", pos, Mix_GetError());
        if (was_paused) { Mix_PauseMusic(); paused = true; }
    }
#endif
}

static void seek(double delta) {
    if (!mus) return;
    double p = position() + delta;
    if (duration > 0 && p > duration - 1) p = duration - 1;
    if (p < 0) p = 0;   /* dopo il limite di fine: con brani più corti di un secondo resta 0 */
    static bool told;
    if (Mix_SetMusicPosition(p) < 0 && !told) { told = true; ui_log("musica: spostamento non riuscito (%s)", Mix_GetError()); }
}

/* ------------------------------------------------------------ copertina e informazioni */

static SDL_Texture *cover_tex;    /* copertina del brano in corso, NULL se non c'è */
static SDL_Texture *cover_soft;   /* la stessa, piccolissima: sfondo sfocato dell'aspetto Copertina */
static char tag_title[200], tag_artist[200];
static bool bg_dirty = true;

static Uint32 be32(const Uint8 *p) { return (Uint32)p[0] << 24 | (Uint32)p[1] << 16 | (Uint32)p[2] << 8 | p[3]; }
static Uint32 syncsafe(const Uint8 *p) {
    return (Uint32)(p[0] & 127) << 21 | (Uint32)(p[1] & 127) << 14 | (Uint32)(p[2] & 127) << 7 | (p[3] & 127);
}

#define PIC_MAX (8u << 20)

/* immagine dentro il file: frame APIC di ID3v2.3/2.4 (MP3) o blocco PICTURE (FLAC). Restituisce i
   dati in un buffer nuovo, NULL se non c'è; tra più immagini vince la copertina frontale (tipo 3).
   Tag "unsynchronised", frame compressi o cifrati: si lasciano stare. */
static Uint8 *embedded_picture(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Uint8 h[10], *out = NULL;
    int best = -1;
    *len = 0;
    if (fread(h, 1, 10, f) != 10) { fclose(f); return NULL; }
    if (!memcmp(h, "ID3", 3) && (h[3] == 3 || h[3] == 4) && !(h[5] & 0x80)) {
        Uint32 size = syncsafe(h + 6), pos = 0;
        Uint8 *tag = size && size <= PIC_MAX ? malloc(size) : NULL;
        if (tag && fread(tag, 1, size, f) == size) {
            if (h[5] & 0x40 && size >= 4) pos = h[3] == 4 ? syncsafe(tag) : be32(tag) + 4;   /* intestazione estesa */
            while ((Uint64)pos + 10 <= size && tag[pos]) {
                Uint32 fs = h[3] == 4 ? syncsafe(tag + pos + 4) : be32(tag + pos + 4);
                Uint16 flags = (Uint16)(tag[pos + 8] << 8 | tag[pos + 9]);
                const Uint8 *fd = tag + pos + 10;
                if (fs > size - pos - 10) break;
                bool skip = h[3] == 4 ? (flags & 0x000E) != 0 : (flags & 0x00C0) != 0;
                if (!memcmp(tag + pos, "APIC", 4) && !skip) {
                    Uint32 o = h[3] == 4 && (flags & 0x0001) ? 4 : 0, n = fs;
                    int enc = o < n ? fd[o] : 0;
                    o++;
                    while (o < n && fd[o]) o++;   /* tipo MIME */
                    o++;
                    int ptype = o < n ? fd[o] : 0;
                    o++;
                    if (enc == 1 || enc == 2) { while (o + 1 < n && (fd[o] || fd[o + 1])) o += 2; o += 2; }
                    else { while (o < n && fd[o]) o++; o++; }   /* descrizione */
                    if (o < n && (best < 0 || (ptype == 3 && best != 3))) {
                        Uint8 *d = malloc(n - o);
                        if (d) { memcpy(d, fd + o, n - o); free(out); out = d; *len = n - o; best = ptype; }
                    }
                }
                pos += 10 + fs;
            }
        }
        free(tag);
    } else if (!memcmp(h, "fLaC", 4)) {
        Uint8 b[4];
        long at = 4;
        for (int k = 0; k < 64 && fseek(f, at, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4; k++) {
            Uint32 bl = (Uint32)b[1] << 16 | (Uint32)b[2] << 8 | b[3];
            if ((b[0] & 127) == 6 && bl >= 32 && bl <= PIC_MAX) {
                Uint8 *blk = malloc(bl);
                if (blk && fread(blk, 1, bl, f) == bl) {
                    Uint32 ptype = be32(blk), o = 4, ml = be32(blk + o);
                    o += 4;
                    if ((Uint64)o + ml + 4 <= bl) {
                        o += ml;
                        Uint32 dl = be32(blk + o);
                        o += 4;
                        if ((Uint64)o + dl + 20 <= bl) {
                            o += dl + 16;
                            Uint32 n = be32(blk + o);
                            o += 4;
                            if (n && (Uint64)o + n <= bl && (best < 0 || (ptype == 3 && best != 3))) {
                                Uint8 *d = malloc(n);
                                if (d) { memcpy(d, blk + o, n); free(out); out = d; *len = n; best = (int)ptype; }
                            }
                        }
                    }
                }
                free(blk);
            }
            if (b[0] & 0x80) break;
            at += 4 + (long)bl;
        }
    }
    fclose(f);
    return out;
}

/* Una copertina si decodifica solo con misure note e ragionevoli: 16 milioni di pixel sono circa
   64 MB decodificati, e una copertina vera è sotto i 3000x3000 (verifica Codex M61-006) */
#define COVER_MAX_PIXELS 16000000L
static bool cover_size_ok(bool known, long w, long h) {
    bool ok = known && w > 0 && h > 0 && w <= 16384 && h <= 16384 && w * h <= COVER_MAX_PIXELS;
    if (known && !ok) ui_log("musica: copertina troppo grande (%ldx%ld), non la mostro", w, h);
    return ok;
}
static bool cover_size_ok_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char head[64];
    size_t hn = fread(head, 1, sizeof head, f);
    long w = 0, h = 0;
    bool known = hn >= 4 && head[0] == 0xFF && head[1] == 0xD8 ? jpeg_dims_file(f, &w, &h) : img_dims(head, hn, &w, &h);
    fclose(f);
    return cover_size_ok(known, w, h);
}

/* immagine accanto al brano: cover, folder, front, album, albumart (.jpg, .jpeg, .png) o lo stesso
   nome del brano */
static SDL_Surface *folder_picture(const char *track) {
    static const char *names[] = { "cover", "folder", "front", "album", "albumart" };
    char dir[1700], base[512];
    snprintf(dir, sizeof dir, "%s", track);
    char *sl = strrchr(dir, '/');
    if (!sl) return NULL;
    *sl = 0;
    snprintf(base, sizeof base, "%s", sl + 1);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    SDL_Surface *s = NULL;
    while (d && !s && (e = readdir(d))) {
        const char *x = strrchr(e->d_name, '.');
        if (!x || e->d_name[0] == '.' || (strcasecmp(x, ".jpg") && strcasecmp(x, ".jpeg") && strcasecmp(x, ".png"))) continue;
        size_t l = (size_t)(x - e->d_name);
        bool ok = strlen(base) == l && !strncasecmp(e->d_name, base, l);
        for (size_t i = 0; !ok && i < sizeof names / sizeof names[0]; i++)
            ok = strlen(names[i]) == l && !strncasecmp(e->d_name, names[i], l);
        if (!ok) continue;
        char p[2000];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (cover_size_ok_file(p)) s = IMG_Load(p);
    }
    if (d) closedir(d);
    return s;
}

/* copia sempre più piccola (metà per volta, filtro lineare) fino a larghezza <= maxw: una
   sfocatura che costa poco. NULL se la texture è già abbastanza piccola. */
static SDL_Texture *shrink(SDL_Texture *src, int maxw) {
    int w, h;
    if (!src || SDL_QueryTexture(src, NULL, NULL, &w, &h) != 0 || w <= maxw) return NULL;
    SDL_Texture *prev = SDL_GetRenderTarget(UI.ren), *cur = src;
    SDL_Rect clip;
    SDL_bool clipped = SDL_RenderIsClipEnabled(UI.ren);
    SDL_RenderGetClipRect(UI.ren, &clip);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    while (w > maxw) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        SDL_Texture *next = SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!next) break;
        SDL_SetTextureScaleMode(next, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(next, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(UI.ren, next);
        SDL_RenderSetClipRect(UI.ren, NULL);
        SDL_RenderCopy(UI.ren, cur, NULL, NULL);
        if (cur != src) SDL_DestroyTexture(cur);
        cur = next;
    }
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(UI.ren, prev);
    SDL_RenderSetClipRect(UI.ren, clipped ? &clip : NULL);
    if (cur == src) return NULL;
    SDL_SetTextureBlendMode(cur, SDL_BLENDMODE_BLEND);
    return cur;
}

/* le copertine grandi (anche 3000x3000) si riducono a 512 punti prima di diventare texture */
static SDL_Texture *cover_texture(SDL_Surface *s) {
    if (!s) return NULL;
    SDL_Surface *use = s, *conv = NULL, *small = NULL;
    int big = s->w > s->h ? s->w : s->h;
    if (big > 512 && (conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0))) {
        int w = (int)((long)s->w * 512 / big), h = (int)((long)s->h * 512 / big);
        small = SDL_CreateRGBSurfaceWithFormat(0, w > 0 ? w : 1, h > 0 ? h : 1, 32, SDL_PIXELFORMAT_ARGB8888);
        if (small && SDL_SoftStretchLinear(conv, NULL, small, NULL) == 0) use = small;
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(UI.ren, use);
    if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    if (small) SDL_FreeSurface(small);
    if (conv) SDL_FreeSurface(conv);
    return t;
}

static void cover_load(int i) {
    if (cover_tex) SDL_DestroyTexture(cover_tex);
    if (cover_soft) SDL_DestroyTexture(cover_soft);
    cover_tex = cover_soft = NULL;
    tag_title[0] = tag_artist[0] = 0;
    bg_dirty = true;
    if (i < 0 || i >= ntracks) return;
#if defined(SDL_MIXER_VERSION_ATLEAST) && SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
    if (mus) {
        const char *t = Mix_GetMusicTitleTag(mus), *a = Mix_GetMusicArtistTag(mus);
        snprintf(tag_title, sizeof tag_title, "%s", t ? t : "");
        snprintf(tag_artist, sizeof tag_artist, "%s", a ? a : "");
    }
#endif
    char p[1700];
    size_t n = 0;
    snprintf(p, sizeof p, "%s/Music/%s", app_root, tracks[i].rel);
    Uint8 *pic = embedded_picture(p, &n);
    long iw = 0, ih = 0;
    bool known = pic && img_dims(pic, n, &iw, &ih);
    SDL_Surface *s = pic && n < 0x7fffffff && cover_size_ok(known, iw, ih) ? IMG_Load_RW(SDL_RWFromConstMem(pic, (int)n), 1) : NULL;
    free(pic);
    if (!s) s = folder_picture(p);
    cover_tex = cover_texture(s);
    if (s) SDL_FreeSurface(s);
    cover_soft = shrink(cover_tex, 16);
}

static const char *cur_title(void) { return cur < 0 ? "" : tag_title[0] ? tag_title : tracks[cur].title; }
static const char *cur_artist(void) { return cur < 0 ? "" : tag_artist[0] ? tag_artist : tracks[cur].album; }

/* ------------------------------------------------------------ spettro */

/* Il mixer passa ogni blocco suonato a postmix (thread dell'audio): si tiene l'ultimo pezzo in
   mono, e a ogni disegno una FFT da 1024 punti lo divide in bande a scala logaritmica. */
#define FFT_N 1024
#define RING 4096
#define NBARS 48
static _Atomic short ring[RING];   /* campioni mono: li scrive il thread dell'audio, li legge il disegno */
static _Atomic unsigned ring_w;
static float bars[NBARS];

static void postmix(void *udata, Uint8 *stream, int len) {
    (void)udata;
    const Sint16 *s = (const Sint16 *)stream;
    int ch = spec_ch > 0 ? spec_ch : 2, n = len / (2 * ch);
    unsigned w = ring_w;
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int c = 0; c < ch; c++) v += s[i * ch + c];
        atomic_store_explicit(&ring[w++ & (RING - 1)], (short)(v / ch), memory_order_relaxed);
    }
    ring_w = w;
}

static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float vr = re[b] * cr - im[b] * ci, vi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr; im[a] += vi;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

static void spectrum_update(void) {
    float target[NBARS] = { 0 };
    if (mixer_on && cur >= 0 && !paused) {
        static float re[FFT_N], im[FFT_N];
        unsigned w = ring_w;
        float g = ui_gain();   /* sul Mac il volume scala i campioni: le barre non devono cambiare */
        if (g < 0.02f) g = 0.02f;
        for (int i = 0; i < FFT_N; i++) {
            float hann = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
            re[i] = atomic_load_explicit(&ring[(w - FFT_N + i) & (RING - 1)], memory_order_relaxed) / 32768.0f * hann / g;
            im[i] = 0;
        }
        fft(re, im, FFT_N);
        float lo = 50, hi = spec_rate / 2.0f < 16000 ? spec_rate / 2.0f : 16000;
        for (int b = 0; b < NBARS; b++) {
            float f0 = lo * powf(hi / lo, (float)b / NBARS), f1 = lo * powf(hi / lo, (float)(b + 1) / NBARS);
            int k0 = (int)(f0 * FFT_N / spec_rate), k1 = (int)(f1 * FFT_N / spec_rate);
            if (k0 < 1) k0 = 1;
            if (k1 <= k0) k1 = k0 + 1;
            float m = 0;
            for (int k = k0; k < k1 && k < FFT_N / 2; k++) {
                float v = re[k] * re[k] + im[k] * im[k];
                if (v > m) m = v;
            }
            float db = 10.0f * log10f(m * 16.0f / ((float)FFT_N * FFT_N) + 1e-10f);
            float v = (db + 58.0f) / 50.0f + 0.12f * b / NBARS;   /* gli acuti hanno meno energia */
            target[b] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
    }
    float up = UI.dt * 22 > 1 ? 1 : (float)UI.dt * 22, down = (float)UI.dt * 1.4f;
    for (int b = 0; b < NBARS; b++) {
        if (target[b] > bars[b]) bars[b] += (target[b] - bars[b]) * up;
        else bars[b] = bars[b] - down > target[b] ? bars[b] - down : target[b];
    }
}

/* ------------------------------------------------------------ aspetti */

/* Gli aspetti sono cartelle in skins/ accanto all'app, ognuna con skin.json:
     "order": posizione nell'elenco, "name" e "texts": {"it": ...} il nome per lingua,
     "layout": "cover" (copertina, titolo, controlli e volume), "spectrum" (icona, spettro,
               controlli in un riquadro), "title" (titolo grande, spettro, tempi),
     "background": "theme" (lo sfondo del tema di Mosaik), "blobs" (sfumatura con tre aloni),
                   "cover" (la copertina sfocata), "image" (il file "image" della cartella),
     "glass": riquadro di vetro smerigliato (sfondo sfocato dietro, bordo chiaro),
     "colors": background, background2, panel, border, text, text_dim, accent, accent_text,
               bars, blob1, blob2, blob3 in #RRGGBB o #RRGGBBAA; quelli che mancano vengono dal tema.
   START passa al successivo; la scelta resta in System/musica-aspetto.txt. */
enum { LAY_COVER, LAY_SPECTRUM, LAY_TITLE };
enum { BG_THEME, BG_BLOBS, BG_COVER, BG_IMAGE };
typedef struct {
    char id[48], name[64], image[1300];
    int order, layout, bgkind;
    bool glass;
    color_t bg, bg2, panel, border, text, dim, accent, accent_text, bars, blob[3];
} skin_t;
#define MAX_SKINS 16
static skin_t skins[MAX_SKINS];
static int nskins, cur_skin;

static color_t col(jval *c, const char *key, color_t def) {
    const char *s = json_str(c, key, NULL);
    if (!s || s[0] != '#') return def;
    size_t n = strlen(s + 1);
    unsigned v = (unsigned)strtoul(s + 1, NULL, 16);
    if (n == 6) return rgba(v >> 16, (v >> 8) & 255, v & 255, 255);
    if (n == 8) return rgba(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return def;
}

static void skin_from_theme(skin_t *s) {
    theme_t *t = &UI.th;
    s->layout = LAY_COVER;
    s->bgkind = BG_THEME;
    s->glass = false;
    s->bg = t->bg;
    s->bg2 = t->bg2;
    s->panel = with_alpha(t->text_dim, 30);
    s->border = with_alpha(t->text_dim, 60);
    s->text = t->text;
    s->dim = t->text_dim;
    s->accent = t->accent;
    s->accent_text = t->accent_text;
    s->bars = t->accent;
    for (int i = 0; i < 3; i++) s->blob[i] = with_alpha(t->accent, 60);
}

static int skin_cmp(const void *a, const void *b) {
    const skin_t *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

static void skin_path(char *p, size_t n) { snprintf(p, n, "%s/System/musica-aspetto.txt", app_root); }

static void skins_load(void) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/skins", app_dir);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && nskins < MAX_SKINS && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof skins[0].id) continue;
        char p[1300];
        snprintf(p, sizeof p, "%s/%s/skin.json", dir, e->d_name);
        jval *j = json_load(p);
        if (!j) continue;
        skin_t *s = &skins[nskins++];
        memset(s, 0, sizeof *s);
        skin_from_theme(s);
        snprintf(s->id, sizeof s->id, "%s", e->d_name);
        s->order = json_int(j, "order", 99);
        char key[48];
        snprintf(key, sizeof key, "texts.%s", UI.set.lang);
        snprintf(s->name, sizeof s->name, "%s", json_str(j, key, json_str(j, "texts.en", json_str(j, "name", s->id))));
        const char *lay = json_str(j, "layout", "cover"), *bg = json_str(j, "background", "theme");
        s->layout = !strcmp(lay, "spectrum") ? LAY_SPECTRUM : !strcmp(lay, "title") ? LAY_TITLE : LAY_COVER;
        s->bgkind = !strcmp(bg, "blobs") ? BG_BLOBS : !strcmp(bg, "cover") ? BG_COVER : !strcmp(bg, "image") ? BG_IMAGE : BG_THEME;
        const char *img = json_str(j, "image", NULL);
        if (img && !strstr(img, "..")) snprintf(s->image, sizeof s->image, "%s/%s/%s", dir, e->d_name, img);
        s->glass = json_bool(j, "glass", s->bgkind != BG_THEME);
        jval *c = json_get(j, "colors");
        if (c) {
            s->bg = col(c, "background", s->bg);
            s->bg2 = col(c, "background2", s->bg);
            s->panel = col(c, "panel", s->panel);
            s->border = col(c, "border", s->border);
            s->text = col(c, "text", s->text);
            s->dim = col(c, "text_dim", s->dim);
            s->accent = col(c, "accent", s->accent);
            s->accent_text = col(c, "accent_text", s->accent_text);
            s->bars = col(c, "bars", s->accent);
            s->blob[0] = col(c, "blob1", s->blob[0]);
            s->blob[1] = col(c, "blob2", s->blob[0]);
            s->blob[2] = col(c, "blob3", s->blob[1]);
        }
        json_free(j);
    }
    if (d) closedir(d);
    if (!nskins) {   /* nessun aspetto installato: quello del tema */
        skin_from_theme(&skins[0]);
        snprintf(skins[0].id, sizeof skins[0].id, "tema");
        snprintf(skins[0].name, sizeof skins[0].name, "Mosaik");
        nskins = 1;
    }
    qsort(skins, (size_t)nskins, sizeof *skins, skin_cmp);
    char p[1200], id[64] = "";
    skin_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(id, sizeof id, f)) id[strcspn(id, "\r\n")] = 0;
        fclose(f);
    }
    for (int i = 0; i < nskins; i++)
        if (!strcmp(skins[i].id, id)) cur_skin = i;
    ui_log("musica: %d aspetti, in uso %s", nskins, skins[cur_skin].id);
}

static void skin_save(void) {
    char p[1200];
    skin_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%s\n", skins[cur_skin].id);
    if (fclose(f) != 0) remove(p);
}

/* i colori dell'aspetto valgono per tutto il disegno, barre di Mosaik comprese */
static theme_t saved_th;
static void skin_push(const skin_t *s) {
    saved_th = UI.th;
    if (s->bgkind == BG_THEME && !s->glass) return;
    UI.th.bg = s->bg;
    UI.th.bg2 = s->bg2;
    UI.th.text = s->text;
    UI.th.text_dim = s->dim;
    UI.th.accent = s->accent;
    UI.th.accent_text = s->accent_text;
    UI.th.line = s->border;
    UI.th.surface = s->panel;
}
static void skin_pop(void) { UI.th = saved_th; }

/* ------------------------------------------------------------ sfondo e vetro */

static SDL_Texture *bg_tex, *bg_soft;

static void bg_build(const skin_t *s) {
    if (!bg_dirty && bg_tex) return;
    bg_dirty = false;
    if (!bg_tex) {
        bg_tex = SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, UI.w, UI.h);
        if (!bg_tex) return;
        SDL_SetTextureScaleMode(bg_tex, SDL_ScaleModeLinear);
    }
    SDL_Texture *prev = SDL_GetRenderTarget(UI.ren);
    SDL_Rect clip;
    SDL_bool clipped = SDL_RenderIsClipEnabled(UI.ren);
    SDL_RenderGetClipRect(UI.ren, &clip);
    SDL_SetRenderTarget(UI.ren, bg_tex);
    SDL_RenderSetClipRect(UI.ren, NULL);
    SDL_SetRenderDrawColor(UI.ren, s->bg.r, s->bg.g, s->bg.b, 255);
    SDL_RenderClear(UI.ren);
    ui_vgradient(0, 0, UI.w, UI.h, s->bg, s->bg2);
    SDL_Texture *img = s->bgkind == BG_IMAGE && s->image[0] ? ui_image(s->image) : NULL;
    if (s->bgkind == BG_COVER && cover_soft) {
        ui_image_fill(cover_soft, 0, 0, UI.w, UI.h, 0);
        ui_rect(0, 0, UI.w, UI.h, with_alpha(s->bg, 150));
    } else if (img) ui_image_fill(img, 0, 0, UI.w, UI.h, 0);
    else {   /* tre aloni morbidi: danno al vetro qualcosa da sfocare */
        static const float at[3][3] = { { 0.16f, 0.22f, 0.34f }, { 0.86f, 0.74f, 0.40f }, { 0.58f, 0.02f, 0.24f } };
        for (int i = 0; i < 3; i++) {
            int r = (int)(UI.h * at[i][2]), cx = (int)(UI.w * at[i][0]), cy = (int)(UI.h * at[i][1]);
            ui_shadow(cx - r / 2, cy - r / 2, r, r, r / 2, r, s->blob[i]);
        }
    }
    SDL_SetRenderTarget(UI.ren, prev);
    SDL_RenderSetClipRect(UI.ren, clipped ? &clip : NULL);
    if (bg_soft) SDL_DestroyTexture(bg_soft);
    bg_soft = shrink(bg_tex, UI.w / 12);
}

/* lo sfondo sfocato dentro un rettangolo arrotondato (un ventaglio di triangoli) */
static void frosted(int x, int y, int w, int h, int r) {
    if (!bg_soft || r * 2 > w || r * 2 > h) return;
    enum { SEG = 6 };
    SDL_Vertex v[1 + 4 * (SEG + 1)];
    int idx[3 * 4 * (SEG + 1)], n = 0, ni = 0;
    static const float start[4] = { -90, 0, 90, 180 };
    float ccx[4] = { (float)(x + w - r), (float)(x + w - r), (float)(x + r), (float)(x + r) };
    float ccy[4] = { (float)(y + r), (float)(y + h - r), (float)(y + h - r), (float)(y + r) };
    SDL_Color white = { 255, 255, 255, 255 };
    v[n++] = (SDL_Vertex){ { x + w / 2.0f, y + h / 2.0f }, white, { (x + w / 2.0f) / UI.w, (y + h / 2.0f) / UI.h } };
    for (int c = 0; c < 4; c++)
        for (int k = 0; k <= SEG; k++) {
            float a = (start[c] + 90.0f * k / SEG) * (float)M_PI / 180.0f;
            float px = ccx[c] + r * cosf(a), py = ccy[c] + r * sinf(a);
            v[n++] = (SDL_Vertex){ { px, py }, white, { px / UI.w, py / UI.h } };
        }
    for (int i = 1; i < n; i++) {
        idx[ni++] = 0;
        idx[ni++] = i;
        idx[ni++] = i + 1 < n ? i + 1 : 1;
    }
    SDL_RenderGeometry(UI.ren, bg_soft, v, n, idx, ni);
}

static int thin(void) { int t = M(0.0025f); return t > 1 ? t : 1; }

static void panel(int x, int y, int w, int h, int r, const skin_t *s) {
    if (s->glass) {
        ui_shadow(x, y + M(0.012f), w, h, r, M(0.035f), rgba(0, 0, 0, 120));
        frosted(x, y, w, h, r);
    }
    ui_rrect(x, y, w, h, r, s->panel);
    if (s->glass) ui_rborder(x, y, w, h, r, thin(), s->border);
}

/* ------------------------------------------------------------ icone */

static void line(float x1, float y1, float x2, float y2, float th, color_t c) {
    float dx = x2 - x1, dy = y2 - y1, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.5f) return;
    float nx = -dy / l * th / 2, ny = dx / l * th / 2;
    ui_triangle((int)(x1 + nx), (int)(y1 + ny), (int)(x2 + nx), (int)(y2 + ny), (int)(x2 - nx), (int)(y2 - ny), c);
    ui_triangle((int)(x1 + nx), (int)(y1 + ny), (int)(x2 - nx), (int)(y2 - ny), (int)(x1 - nx), (int)(y1 - ny), c);
}

static void tri(float cx, float cy, float s, int dir, color_t c) {   /* s = altezza, dir +1 a destra */
    float hw = s * 0.45f;
    ui_triangle((int)(cx - dir * hw), (int)(cy - s / 2), (int)(cx - dir * hw), (int)(cy + s / 2), (int)(cx + dir * hw), (int)cy, c);
}

static void ic_play(int cx, int cy, int s, color_t c) { tri(cx + s * 0.08f, (float)cy, (float)s, 1, c); }
static void ic_pause(int cx, int cy, int s, color_t c) {
    int bw = s * 28 / 100, gap = s * 24 / 100;
    ui_rrect(cx - bw - gap / 2, cy - s / 2, bw, s, bw / 3, c);
    ui_rrect(cx + gap / 2, cy - s / 2, bw, s, bw / 3, c);
}
static void ic_skip(int cx, int cy, int s, int dir, color_t c) {
    float t = s * 0.8f;
    tri(cx - dir * s * 0.08f, (float)cy, t, dir, c);
    int bw = s / 6 > 1 ? s / 6 : 2;
    ui_rect(dir > 0 ? cx + (int)(s * 0.32f) : cx - (int)(s * 0.32f) - bw, cy - (int)(t / 2), bw, (int)t, c);
}
static void ic_seek(int cx, int cy, int s, int dir, color_t c) {
    float t = s * 0.7f;
    tri(cx - dir * s * 0.2f, (float)cy, t, dir, c);
    tri(cx + dir * s * 0.2f, (float)cy, t, dir, c);
}
static void ic_note(int cx, int cy, int s, color_t c) {   /* due crome legate */
    int hr = s * 15 / 100, st = s / 14 > 1 ? s / 14 : 2;
    int lx = cx - s * 22 / 100, ly = cy + s * 30 / 100, rx = cx + s * 24 / 100, ry = cy + s * 22 / 100;
    ui_circle(lx, ly, hr, c);
    ui_circle(rx, ry, hr, c);
    int ltop = cy - s * 32 / 100, rtop = cy - s * 40 / 100;
    ui_rect(lx + hr - st, ltop, st, ly - ltop, c);
    ui_rect(rx + hr - st, rtop, st, ry - rtop, c);
    line((float)(lx + hr - st / 2), (float)ltop + s * 0.06f, (float)(rx + hr - st / 2), (float)rtop + s * 0.06f, s * 0.13f, c);
}
static void ic_repeat(int cx, int cy, int s, int mode, color_t c) {
    int w = s, h = s * 6 / 10, th = thin() + 1;
    ui_rborder(cx - w / 2, cy - h / 2, w, h, h / 2, th, c);
    tri(cx + s * 0.08f, (float)(cy - h / 2 + th / 2), s * 0.38f, 1, c);
    if (mode == 2) ui_text(UI.f_small, "1", cx, cy - TTF_FontHeight(UI.f_small) / 2, c, ALIGN_CENTER);
}
static void ic_shuffle(int cx, int cy, int s, color_t c) {
    float h = s * 0.32f, l = cx - s * 0.5f, r = cx + s * 0.32f, th = (float)(thin() + 1);
    line(l, cy + h, r, cy - h, th, c);
    line(l, cy - h, r, cy + h, th, c);
    tri(r + s * 0.06f, cy - h, s * 0.3f, 1, c);
    tri(r + s * 0.06f, cy + h, s * 0.3f, 1, c);
}
static void ic_speaker(int cx, int cy, int s, color_t c) {
    int bw = s / 4, bh = s * 4 / 10;
    ui_rect(cx - s / 2, cy - bh / 2, bw, bh, c);
    ui_triangle(cx - s / 2 + bw, cy - bh / 2, cx - s / 2 + bw, cy + bh / 2, cx + s / 8, cy - s / 2, c);
    ui_triangle(cx - s / 2 + bw, cy + bh / 2, cx + s / 8, cy + s / 2, cx + s / 8, cy - s / 2, c);
    ui_rect(cx + s / 4, cy - s / 5, thin() + 1, s * 2 / 5, c);
}

/* ------------------------------------------------------------ parti della schermata */

enum { C_PREV, C_REW, C_PLAY, C_FF, C_NEXT };
static double flash_t[5] = { -9, -9, -9, -9, -9 };

static void mmss(double s, char *out, size_t n) {
    if (s < 0) s = 0;
    int t = (int)s;
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void ctext(TTF_Font *f, const char *s, int cx, int y, int maxw, color_t c) {
    char fit[300];
    ui_text_fit(f, s, maxw, fit, sizeof fit);
    ui_text(f, fit, cx, y, c, ALIGN_CENTER);
}

/* barra del brano con il pallino; i tempi sotto, ai due lati */
static void progress(int x, int y, int w, bool times, const skin_t *s) {
    double pos = position(), frac = duration > 0 ? (pos < duration ? pos / duration : 1) : 0;
    int bh = M(0.006f) > 2 ? M(0.006f) : 2, fx = x + (int)(w * frac);
    ui_rrect(x, y - bh / 2, w, bh, bh / 2, with_alpha(s->dim, 70));
    if (frac > 0) ui_rrect(x, y - bh / 2, fx - x, bh, bh / 2, s->accent);
    if (cur >= 0) ui_circle(fx, y, bh * 2, s->accent);
    if (times) {
        char a[16], b[16];
        mmss(pos, a, sizeof a);
        mmss(duration, b, sizeof b);
        int ty = y + bh * 2 + M(0.008f);
        ui_text(UI.f_small, cur >= 0 ? a : "-:--", x, ty, s->dim, ALIGN_LEFT);
        ui_text(UI.f_small, cur >= 0 && duration > 0 ? b : "-:--", x + w, ty, s->dim, ALIGN_RIGHT);
    }
}

static void level_bar(int x, int y, int w, float frac, const skin_t *s) {
    int bh = M(0.005f) > 2 ? M(0.005f) : 2;
    ui_rrect(x, y - bh / 2, w, bh, bh / 2, with_alpha(s->dim, 60));
    if (frac > 0) ui_rrect(x, y - bh / 2, (int)(w * (frac > 1 ? 1 : frac)), bh, bh / 2, s->dim);
}

/* indietro, -10 s, suona/pausa, +10 s, avanti: i tasti L, sinistra, A, destra, R */
static void transport(int x, int y, int w, int s, float play_scale, const skin_t *sk) {
    for (int i = 0; i < 5; i++) {
        int cx = x + w * (2 * i + 1) / 10;
        bool lit = UI.t - flash_t[i] < 0.2;
        if (i == C_PLAY) {
            int R = (int)(s * play_scale);
            ui_circle(cx, y, R, with_alpha(sk->text, lit ? 80 : 40));
            ui_ring(cx, y, R, thin(), with_alpha(sk->text, 110));
            if (cur >= 0 && !paused) ic_pause(cx, y, s * 6 / 10, sk->text);
            else ic_play(cx, y, s * 6 / 10, sk->text);
            continue;
        }
        if (lit) ui_circle(cx, y, s * 8 / 10, with_alpha(sk->text, 45));
        int is = s * 7 / 10;
        if (i == C_PREV) ic_skip(cx, y, is, -1, sk->text);
        else if (i == C_NEXT) ic_skip(cx, y, is, 1, sk->text);
        else ic_seek(cx, y, is, i == C_REW ? -1 : 1, sk->text);
    }
}

static void bars_draw(int x, int y, int w, int h, const skin_t *s) {
    int n = NBARS, bw = w * 10 / (n * 16), gap = bw * 6 / 10;   /* barre e spazi in proporzione 5:3 */
    if (bw < 2) bw = 2;
    if (gap < 1) gap = 1;
    int mid = y + h / 2;
    x += (w - (n * bw + (n - 1) * gap)) / 2;
    for (int b = 0; b < n; b++) {
        int bh = (int)(bars[b] * h);
        if (bh < bw) bh = bw;
        ui_rrect(x + b * (bw + gap), mid - bh / 2, bw, bh, bw / 2, s->bars);
    }
}

static void modes(int cx1, int cy1, int cx2, int cy2, int is, const skin_t *s) {
    ic_repeat(cx1, cy1, is, repeat, repeat ? s->accent : with_alpha(s->dim, 120));
    ic_shuffle(cx2, cy2, is, shuffle ? s->accent : with_alpha(s->dim, 120));
}

static void cover_or_note(int cx, int cy, int side, const skin_t *s) {
    if (cover_tex) {
        ui_shadow(cx - side / 2, cy - side / 2 + M(0.008f), side, side, M(0.02f), M(0.02f), rgba(0, 0, 0, 90));
        ui_image_fill(cover_tex, cx - side / 2, cy - side / 2, side, side, M(0.02f));
    } else {
        ui_rrect(cx - side / 2, cy - side / 2, side, side, M(0.02f), with_alpha(s->text, 22));
        ic_note(cx, cy, side * 6 / 10, with_alpha(s->text, 200));
    }
}

/* ------------------------------------------------------------ schermata del brano */

static void player_draw(const skin_t *s) {
    theme_t *t = &UI.th;
    ui_topbar(FT("music.title"));
    int top = M(t->topbar_h), bot = UI.h - ui_keybar_h(), avail = bot - top;
    int ch = avail * 93 / 100, cw = ch * 9 / 10, mx = M(t->margin);
    if (cw > UI.w - 2 * mx) cw = UI.w - 2 * mx;
    int x0 = (UI.w - cw) / 2, y0 = top + (avail - ch) / 2, r = M(0.04f);
    panel(x0, y0, cw, ch, r, s);
    int pad = cw * 8 / 100, x = x0 + pad, w = cw - 2 * pad, cx = x0 + cw / 2;
    int fsh = TTF_FontHeight(UI.f_small), fnh = TTF_FontHeight(UI.f_normal), fth = TTF_FontHeight(UI.f_title);
    int is = fsh, ts = ch * 6 / 100;   /* icone piccole, controlli */
#define Y(f) (y0 + (int)(ch * (f)))
    const char *title = cur >= 0 ? cur_title() : FT(ntracks ? "music.choose" : "music.none"), *artist = cur_artist();
    if (s->layout == LAY_COVER) {
        modes(x + is / 2, Y(0.075f), x + w - is / 2, Y(0.075f), is, s);
        int side = ch * 36 / 100;
        cover_or_note(cx, Y(0.08f) + side / 2, side, s);
        ctext(UI.f_title, title, cx, Y(0.52f) - fth / 2, w, s->text);
        if (artist[0]) ctext(UI.f_normal, artist, cx, Y(0.595f) - fnh / 2, w, s->dim);
        progress(x, Y(0.68f), w, true, s);
        transport(x, Y(0.835f), w, ts, 1.05f, s);
        ic_speaker(x + is / 2, Y(0.945f), is, s->dim);
        level_bar(x + is + M(0.015f), Y(0.945f), w / 2, UI.set.volume / 20.0f, s);
    } else if (s->layout == LAY_SPECTRUM) {
        modes(x + w - is * 5 / 2, Y(0.07f), x + w - is / 2, Y(0.07f), is, s);
        int side = ch * 28 / 100;
        if (cover_tex) cover_or_note(cx, Y(0.08f) + side / 2, side, s);
        else ic_note(cx, Y(0.08f) + side / 2, side * 8 / 10, with_alpha(s->text, 210));
        ctext(UI.f_normal, title, cx, Y(0.425f) - fnh / 2, w, s->text);
        if (artist[0]) ctext(UI.f_small, artist, cx, Y(0.48f) - fsh / 2, w, s->dim);
        bars_draw(x, Y(0.52f), w, ch * 16 / 100, s);
        progress(x, Y(0.725f), w, false, s);
        int py = Y(0.775f), ph = ch * 15 / 100;
        ui_rrect(x - pad / 3, py, w + pad * 2 / 3, ph, M(0.025f), rgba(0, 0, 0, s->glass ? 50 : 20));
        if (s->glass) ui_rborder(x - pad / 3, py, w + pad * 2 / 3, ph, M(0.025f), thin(), with_alpha(s->border, s->border.a / 2));
        transport(x, py + ph / 2, w, ts, 0.95f, s);
        level_bar(x, Y(0.96f), w * 4 / 10, UI.set.volume / 20.0f, s);
    } else {
        modes(x + is / 2, Y(0.07f), x + w - is / 2, Y(0.07f), is, s);
        ctext(UI.f_title, title, cx, Y(0.18f) - fth / 2, w, s->text);
        if (artist[0]) ctext(UI.f_normal, artist, cx, Y(0.265f) - fnh / 2, w, s->dim);
        bars_draw(x, Y(0.33f), w, ch * 22 / 100, s);
        progress(x, Y(0.625f), w, true, s);
        transport(x, Y(0.8f), w, ts, 1.3f, s);
        int hw = cw / 8, hh = M(0.006f) > 3 ? M(0.006f) : 3;
        ui_rrect(cx - hw / 2, Y(0.94f), hw, hh, hh / 2, with_alpha(s->dim, 110));
    }
#undef Y
    const char *h[] = { "B", FT("music.list"), NULL, "START", FT("music.skin"), "", "SELECT", FT("music.screen_off"), "",
                        "A", FT(cur >= 0 && !paused ? "music.pause" : "music.play"), NULL };
    ui_keybar(h, 12);
}

/* ------------------------------------------------------------ elenco */

static void list_draw(void) {
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
        ui_text_fit(ft, cur_title(), W - M(0.04f), fit, sizeof fit);
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
    char modes_s[200];
    snprintf(modes_s, sizeof modes_s, "%s  ·  %s", FT(rk[repeat]), FT(shuffle ? "music.shuffle_on" : "music.shuffle_off"));
    int top = M(t->topbar_h) + M(0.02f);
    ui_text(fs, modes_s, mx, top, t->text_dim, ALIGN_LEFT);
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
    const char *h[] = { "B", FT("music.quit"), NULL, "START", FT("music.skin"), "", "X", FT("music.repeat"), "",
                        "Y", FT("music.shuffle"), "", "A", FT(sel == cur && mus ? "music.open" : "music.play"), NULL };
    ui_keybar(h, 15);
}

static bool player_view;

static void draw(void) {
    const skin_t *s = &skins[cur_skin];
    if (s->bgkind != BG_THEME) {
        bg_build(s);
        if (bg_tex) SDL_RenderCopy(UI.ren, bg_tex, NULL, NULL);
    }
    skin_push(s);
    if (player_view) player_draw(s); else list_draw();
    skin_pop();
}

/* ------------------------------------------------------------ comandi */

static double hold_t;

static void input(void) {
    input_t *in = &UI.in;
    if (screen_off) {   /* un tasto qualsiasi riaccende, senza fare altro */
        bool any = false;
        for (int b = 0; b < BTN_COUNT; b++) any |= in->pressed[b];
        if (any) { screen_off = false; plat_screen_off(false); plat_apply(&UI.set); }
        return;
    }
    if (in->pressed[BTN_MENU]) { quit = true; return; }
    if (in->pressed[BTN_B]) {   /* dal brano si torna all'elenco, dall'elenco si esce */
        if (player_view) { player_view = false; ui_sound(SND_BACK); } else quit = true;
        return;
    }
    if (!player_view) {
        int old = sel;
        if (in->pressed[BTN_UP] && ntracks) sel = (sel + ntracks - 1) % ntracks;
        if (in->pressed[BTN_DOWN] && ntracks) sel = (sel + 1) % ntracks;
        if (sel != old) ui_sound(SND_MOVE);
        if (in->pressed[BTN_A] && ntracks) {
            if (sel == cur && mus) player_view = true;
            else if (play(sel)) player_view = true;
            else { ui_toast(FT("music.bad_file")); ui_sound(SND_ERROR); }
        }
    } else if (in->pressed[BTN_A]) {
        flash_t[C_PLAY] = UI.t;
        if (cur >= 0 && mus) {
            if (paused) { Mix_ResumeMusic(); plat_amp_wake(); } else Mix_PauseMusic();
            paused = !paused;
        } else if (ntracks && !play(sel)) { ui_toast(FT("music.bad_file")); ui_sound(SND_ERROR); }
    }
    if (in->pressed[BTN_START] && nskins > 1) {
        cur_skin = (cur_skin + 1) % nskins;
        bg_dirty = true;
        skin_save();
        char msg[160];
        snprintf(msg, sizeof msg, FT("music.skin_now"), skins[cur_skin].name);
        ui_toast(msg);
        ui_sound(SND_OK);
    }
    if (in->pressed[BTN_R] && ntracks) { flash_t[C_NEXT] = UI.t; skip(+1, true); }
    if (in->pressed[BTN_L] && ntracks) {   /* dopo 3 secondi L ricomincia il brano, come i lettori */
        flash_t[C_PREV] = UI.t;
        if (mus && position() > 3) Mix_SetMusicPosition(0); else skip(-1, true);
    }
    /* sinistra/destra: un salto di 10 s, poi ancora ogni 0,4 s se tenuti */
    int d = in->down[BTN_RIGHT] ? 1 : in->down[BTN_LEFT] ? -1 : 0;
    bool jump = false;
    if (in->pressed[BTN_RIGHT] || in->pressed[BTN_LEFT]) { jump = true; hold_t = UI.t + 0.6; }
    else if (d && UI.t > hold_t) { jump = true; hold_t = UI.t + 0.4; }
    if (jump && d) { seek(d * 10.0); flash_t[d > 0 ? C_FF : C_REW] = UI.t; }
    if (in->pressed[BTN_X]) { repeat = (repeat + 1) % 3; ui_sound(SND_OK); }
    if (in->pressed[BTN_Y]) { shuffle = !shuffle; ui_sound(SND_OK); }
    if (in->pressed[BTN_SELECT] && cur >= 0 && !paused) { screen_off = true; plat_screen_off(true); plat_apply(&UI.set); }
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "music")) return 1;
    srand((unsigned)time(NULL));
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Music", app_root);
    scan(dir, "", 0);
    if (ntracks > 1) qsort(tracks, (size_t)ntracks, sizeof *tracks, track_cmp);
    ui_log("musica: %d brani%s", ntracks, scan_cut ? " (elenco incompleto)" : "");
    if (scan_cut) ui_toast(FT("music.list_incomplete"));
    skins_load();
    while (!quit) {
        UI.amp_keep = cur >= 0 && !paused;   /* in riproduzione l'altoparlante resta acceso */
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        resume_check();
        input();
        if (mixer_on) volume_apply();
        if (finished) { finished = false; skip(+1, false); }
        if (player_view) spectrum_update();
        /* schermo spento: niente disegno, poco processore; la retroilluminazione e il volume dei
           tasti si applicano qui, perché di solito lo fa la fine del disegno (audit APP-001) */
        if (screen_off) { plat_apply(&UI.set); SDL_Delay(50); continue; }
        ui_begin();
        draw();
        ui_end();
    }
    app_exit_begin();
    plat_screen_off(false);
    if (mixer_on) {
        Mix_HookMusicFinished(NULL);
        Mix_HaltMusic();
        if (mus) Mix_FreeMusic(mus);
        Mix_CloseAudio();
    }
    app_quit();
    return 0;
}
