/* Lettore video: app di Mosaik, pacchetto dello Store (Arcade/Apps/video).

   I video della cartella Video (creata se manca, anche nelle sottocartelle): MP4, MKV, WEBM, AVI,
   MOV, M4V, in ordine naturale, con durata e risoluzione lette in sottofondo. Nell'elenco A apre
   il video (se era rimasto a metà chiede se riprendere o ricominciare), B o MENU escono.
   Nel video: A pausa, sinistra/destra -10/+10 s (tenuti, ancora ogni 0,4 s), L/R -1/+1 minuto,
   SELECT mostra o toglie la barra con titolo e tempi (compare anche a ogni comando e sparisce dopo
   3 s), B o MENU tornano all'elenco. Il volume è quello della console (tasti laterali).

   Decodifica con FFmpeg (librerie LGPL condivise in lib/ accanto al programma, da
   toolchain/build-ffmpeg.sh) in un thread: lettura del file, video e audio. Il video passa in una
   coda di 4 fotogrammi e si mostra con una texture IYUV scalata dal renderer a tutto schermo,
   proporzioni conservate (anche i pixel non quadrati e i video girati dei telefoni). L'audio va,
   convertito in 16 bit stereo, in un anello di 2 secondi letto dalla richiamata di SDL: è
   l'orologio di riferimento; un video senza audio segue l'orologio del sistema. Fotogrammi in
   ritardo si saltano; se se ne saltano tanti per qualche secondo si alleggerisce la decodifica
   (niente filtro antiblocchi) e un avviso lo dice. Oltre 1920x1200 il video non si apre.

   Il punto raggiunto di ogni video resta in System/video-posizioni.txt (secondi, durata, percorso
   dentro Video; i più recenti in cima, al massimo 300); un video finito si segna come visto.

   Sul Mac: build/video --root sdcard --app package/apps/video [--auto "..."] */
#include "appkit.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../common/platform.h"

#define MAX_PIXELS (1920 * 1200)
#define FQ 4                   /* fotogrammi pronti in coda */
/* un video lasciato a meno di 15 s dalla fine (il 5% per i video sotto i 5 minuti) conta come visto */
static double finished_margin(double dur) { return dur >= 300 ? 15 : dur * 0.05; }

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

/* ------------------------------------------------------------ elenco */

typedef struct {
    char rel[512], name[200], folder[160];
    _Atomic int probed;   /* 1 quando dur, w e h sono pronti (thread delle durate) */
    double dur;
    int w, h;
} vid_t;
static vid_t *vids;
static int nvids, sel;
static float scroll;
static bool scan_cut, quit;

static bool is_video(const char *n) {
    const char *dot = strrchr(n, '.');
    static const char *ext[] = { "mp4", "mkv", "webm", "avi", "mov", "m4v" };
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

static int vid_cmp(const void *a, const void *b) { return natcmp(((const vid_t *)a)->rel, ((const vid_t *)b)->rel); }

static int cap_vids;
static void scan(const char *dir, const char *rel, int depth) {
    DIR *d = depth < 4 ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1600], r[600];
        struct stat sb;
        int lp = snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int lr = rel[0] ? snprintf(r, sizeof r, "%s/%s", rel, e->d_name) : snprintf(r, sizeof r, "%s", e->d_name);
        if (lp >= (int)sizeof p || lr >= (int)sizeof r || lr >= (int)sizeof vids[0].rel) { scan_cut = true; continue; }
        if (stat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { scan(p, r, depth + 1); continue; }
        if (!S_ISREG(sb.st_mode) || !is_video(e->d_name)) continue;   /* solo file veri: una FIFO bloccherebbe */
        if (nvids == cap_vids) {
            int nc = cap_vids ? cap_vids * 2 : 64;
            vid_t *nv = realloc(vids, (size_t)nc * sizeof *nv);
            if (!nv) { scan_cut = true; break; }
            vids = nv;
            cap_vids = nc;
        }
        vid_t *v = &vids[nvids++];
        memset(v, 0, sizeof *v);
        snprintf(v->rel, sizeof v->rel, "%s", r);
        snprintf(v->name, sizeof v->name, "%s", e->d_name);
        char *dot = strrchr(v->name, '.');
        if (dot) *dot = 0;
        snprintf(v->folder, sizeof v->folder, "%s", rel);
    }
    if (d) closedir(d);
}

static void vid_path(int i, char *out, size_t n) { snprintf(out, n, "%s/Video/%s", app_root, vids[i].rel); }

/* durata e misure di ogni video, in sottofondo: basta l'intestazione del contenitore */
static _Atomic bool probe_stop;
static int probe_interrupt(void *u) { (void)u; return probe_stop; }
static int probe_thread(void *u) {
    (void)u;
    for (int i = 0; i < nvids && !probe_stop; i++) {
        char p[1700];
        vid_path(i, p, sizeof p);
        AVFormatContext *fc = avformat_alloc_context();
        if (fc) fc->interrupt_callback = (AVIOInterruptCB){ probe_interrupt, NULL };
        if (fc && avformat_open_input(&fc, p, NULL, NULL) == 0) {
            if (fc->duration > 0) vids[i].dur = fc->duration / (double)AV_TIME_BASE;
            for (unsigned s = 0; s < fc->nb_streams; s++) {   /* senza durata del contenitore: la più lunga degli stream */
                AVStream *st = fc->streams[s];
                double d = st->duration > 0 ? st->duration * av_q2d(st->time_base) : 0;
                if (fc->duration <= 0 && d > vids[i].dur) vids[i].dur = d;
            }
            for (unsigned s = 0; s < fc->nb_streams; s++) {
                AVCodecParameters *cp = fc->streams[s]->codecpar;
                if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->width > 0 && !(fc->streams[s]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                    vids[i].w = cp->width;
                    vids[i].h = cp->height;
                    break;
                }
            }
            avformat_close_input(&fc);
        }
        vids[i].probed = 1;
    }
    return 0;
}

/* ------------------------------------------------------------ posizioni */

typedef struct { char rel[512]; double pos, dur; } mark_t;   /* pos < 0: visto fino alla fine */
#define MAX_MARKS 300
static mark_t marks[MAX_MARKS];
static int nmarks;

static void marks_path(char *p, size_t n) { snprintf(p, n, "%s/System/video-posizioni.txt", app_root); }

static void marks_load(void) {
    char p[1200], line[700];
    marks_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && nmarks < MAX_MARKS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || !t2[1]) continue;
        mark_t *m = &marks[nmarks++];
        m->pos = strncmp(line, "fine\t", 5) ? atof(line) : -1;
        m->dur = atof(t1 + 1);
        snprintf(m->rel, sizeof m->rel, "%s", t2 + 1);
    }
    if (f) fclose(f);
}

static void marks_save(void) {
    char p[1200], tmp[1300];
    marks_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.nuovo", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < nmarks; i++) {
        if (marks[i].pos < 0) fprintf(f, "fine\t%.0f\t%s\n", marks[i].dur, marks[i].rel);
        else fprintf(f, "%.1f\t%.0f\t%s\n", marks[i].pos, marks[i].dur, marks[i].rel);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, p) != 0) { remove(tmp); ui_log("video: posizioni non salvate"); }
}

static mark_t *mark_find(const char *rel) {
    for (int i = 0; i < nmarks; i++)
        if (!strcmp(marks[i].rel, rel)) return &marks[i];
    return NULL;
}

/* il video appena lasciato va in cima */
static void mark_set(const char *rel, double pos, double dur) {
    mark_t m, *old = mark_find(rel);
    int at = old ? (int)(old - marks) : nmarks < MAX_MARKS ? nmarks++ : MAX_MARKS - 1;
    memmove(&marks[1], &marks[0], (size_t)at * sizeof m);
    snprintf(m.rel, sizeof m.rel, "%s", rel);
    m.pos = pos;
    m.dur = dur;
    marks[0] = m;
    marks_save();
}

/* ------------------------------------------------------------ guardia */

/* Limite di tempo per le operazioni che con un driver audio bloccato o una scheda che non risponde
   non tornano più (passaggio dell'audio all'app, apertura e chiusura dell'uscita, ripresa dopo la
   sospensione, apertura di un video, fine dei thread tornando all'elenco): allo scadere il segnale
   chiude l'app e gamewatch torna al menu, invece di un'app ferma (verifica Codex NEW-001, come il
   Lettore musicale). A contatore: le chiamate annidate non lo spengono prima del tempo. Durante
   l'uscita vale il limite di app_exit_begin e la guardia non si tocca più. */
static int guard_depth;
static bool exiting;
static void guard_begin(void) { if (!exiting && guard_depth++ == 0) alarm(8); }
static void guard_end(void) { if (!exiting && guard_depth > 0 && --guard_depth == 0) alarm(0); }
static void exit_begin(void) { exiting = true; app_exit_begin(); }

/* ------------------------------------------------------------ audio */

/* L'uscita audio è dell'app per tutto il tempo (anche i suoni dei tasti passano da qui): la
   richiamata mescola l'audio del video, dall'anello, e i suoni dell'interfaccia. Tutto quello che
   segue è protetto da amtx (anche nella richiamata), che resta valido se l'uscita si riapre.

   Orologio. Di solito è l'audio: play_pos è il tempo del prossimo campione da suonare; se l'audio
   comincia più tardi del video la richiamata suona silenzio e fa avanzare play_pos finché non lo
   raggiunge. Quando l'audio è finito davvero (a_ended, dalla lettura alla fine del file) o non
   arriva più (lettura ferma con le code video piene) l'orologio passa a quello monotono
   (a_wall), ancorato all'ultimo tempo dell'audio; da lì la richiamata suona i dati dell'anello
   solo al loro tempo. Una semplice attesa di dati (anello vuoto per poco) ferma l'orologio. */
static SDL_AudioDeviceID adev;
static SDL_mutex *amtx;
static int arate = 48000, abps = 48000 * 4, alat_samples = 1024;
static Uint8 *aring;
static int aring_cap, ar_r, ar_n;
static double ar_end_pts;            /* tempo del video alla fine dei dati nell'anello */
static double play_pos;              /* tempo del prossimo campione da mandare all'uscita */
static double clk_pts, clk_time;     /* orologio: tempo del video suonato all'istante clk_time */
static bool clk_run;                 /* l'audio sta scorrendo (non a corto di dati) */
static bool a_hold = true;           /* niente audio del video finché non c'è il primo fotogramma */
static bool a_pause, a_ended, a_wall;
/* Uscita audio persa per questo video (non si è riaperta dopo la sospensione): il decoder audio
   resta al thread di lettura, che però butta i campioni invece di aspettare una richiamata che non
   c'è più, e l'orologio resta quello monotono anche dopo i salti (verifica Codex NEW-006). Si
   azzera solo aprendo un altro video. Atomica: la leggono il thread di lettura e l'interfaccia. */
static _Atomic bool a_mute;
static double wall_pts, wall_time;   /* orologio monotono: tempo del video all'istante wall_time */
static _Atomic int a_gain = 256;
static Sint16 fxbuf[48000];          /* suoni dei tasti (stereo) */
static int fx_n, fx_pos;

static double ring_front(void) { return ar_end_pts - (double)ar_n / abps; }

static void ring_drop(int bytes) {
    bytes -= bytes % 4;
    if (bytes > ar_n) bytes = ar_n;
    ar_r = (ar_r + bytes) % aring_cap;
    ar_n -= bytes;
}

static int ring_play(Sint16 *out, int len) {   /* byte suonati */
    int n = ar_n < len ? ar_n : len, g = a_gain;
    n -= n % 4;
    for (int i = 0; i < n / 2; i++) {
        int k = (ar_r + i * 2) % aring_cap;
        Sint16 s = (Sint16)(aring[k] | aring[(k + 1) % aring_cap] << 8);
        out[i] = (Sint16)(s * g / 256);
    }
    ring_drop(n);
    return n;
}

static void audio_cb(void *u, Uint8 *stream, int len) {
    (void)u;
    memset(stream, 0, (size_t)len);
    Sint16 *out = (Sint16 *)stream;
    SDL_LockMutex(amtx);
    int ns = len / 2;
    double lat = (double)alat_samples / arate, dur = (double)len / abps;
    if (a_hold || a_pause) clk_run = false;
    else if (a_wall) {   /* orologio monotono: l'audio che c'è si suona al suo tempo */
        double pos = wall_pts + (now_s() - wall_time) + lat;
        if (ar_n > 0 && ring_front() < pos - 0.03) ring_drop((int)((pos - ring_front()) * abps));
        if (ar_n > 0 && ring_front() <= pos + 0.03) ring_play(out, len);
    } else if (ar_n > 0 && ring_front() > play_pos + 0.04) {   /* l'audio comincia più tardi */
        play_pos += dur;
        clk_pts = play_pos - lat;
        clk_time = now_s();
        clk_run = true;
    } else if (ar_n > 0) {
        double front = ring_front();
        int n = ring_play(out, len);
        play_pos = front + (double)n / abps;
        clk_pts = play_pos - lat;
        clk_time = now_s();
        clk_run = n == len;
    } else clk_run = false;
    for (int i = 0; i < ns && fx_pos < fx_n; i++) {
        int v = out[i] + fxbuf[fx_pos++];
        out[i] = (Sint16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    SDL_UnlockMutex(amtx);
}

static void play_fx(const Sint16 *stereo, int frames) {   /* già con il volume applicato */
    if (!adev) return;
    int n = frames * 2 < (int)(sizeof fxbuf / sizeof fxbuf[0]) ? frames * 2 : (int)(sizeof fxbuf / sizeof fxbuf[0]);
    SDL_LockMutex(amtx);
    memcpy(fxbuf, stereo, (size_t)n * 2);
    fx_n = n;
    fx_pos = 0;
    SDL_UnlockMutex(amtx);
}

static void audio_close_now(void) {
    if (adev) SDL_CloseAudioDevice(adev);
    adev = 0;
}

static bool audio_open(bool keep_ring) {
    if (adev) return true;
    if (getenv("ARCADE_NO_AUDIO")) return false;   /* audio non funzionante (system-init) */
    guard_begin();
    SDL_AudioSpec want = { .freq = UI.audio_rate ? UI.audio_rate : 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024, .callback = audio_cb }, have;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    guard_end();
    if (!adev) { ui_log("video: audio non disponibile (%s)", SDL_GetError()); return false; }
    SDL_LockMutex(amtx);
    arate = have.freq;
    abps = arate * 4;
    alat_samples = have.samples;
    if (!aring) {
        aring_cap = abps * 2;
        aring = malloc((size_t)aring_cap);
    }
    if (aring && !keep_ring) ar_r = ar_n = 0;
    SDL_UnlockMutex(amtx);
    if (!aring) { audio_close_now(); return false; }
    SDL_PauseAudioDevice(adev, 0);
    return true;
}

static void audio_close(void) {
    if (!adev) return;
    guard_begin();
    SDL_CloseAudioDevice(adev);
    guard_end();
    adev = 0;
}

/* ------------------------------------------------------------ riproduzione */

static AVFormatContext *fmt;
static AVCodecContext *vdec, *adec;
static struct SwrContext *swr;
static struct SwsContext *sws;
static int vidx = -1, aidx = -1;
static double vtb, atb, fdur = 1.0 / 30;   /* basi dei tempi e durata di un fotogramma */
static SDL_Thread *dthr, *vthr;
static SDL_mutex *mtx;
static SDL_cond *cnd;
static _Atomic bool d_stop, go_light;
/* Due thread: "lettura" legge il file, decodifica l'audio nell'anello e mette i pacchetti video
   in una coda; "video" li decodifica nella coda dei fotogrammi. Così un file con audio e video
   poco mescolati (AVI vecchi) non blocca la lettura mentre il video aspetta di essere mostrato.
   Ogni salto alza vserial: pacchetti e fotogrammi di prima si buttano. Tutto sotto mtx. */
static bool seek_req;
static double seek_to, drop_before;
static int vserial, eof_serial = -1;       /* eof_serial: il video è finito per questo salto */
#define PQ 512
#define PQ_BYTES (24 << 20)
static AVPacket *pq[PQ];                   /* NULL = fine del file */
static int pq_serial[PQ], pq_r, pq_n, pq_bytes;
static AVFrame *fq[FQ];
static double fq_pts[FQ];
static int fq_r, fq_n;
static int vframes_ok, vframes_bad;

static bool playing, paused, waiting, has_audio, bar_pinned, light_told;
/* l'orologio può essere l'audio: c'è il suo decoder e c'è l'uscita */
static bool audio_clock(void) { return has_audio && !a_mute; }
static int cur = -1, rot;                  /* rotazione in gradi, in senso orario */
static double cur_pts, duration, wait_since, bar_until, paused_at_clk;
static double audio_end = -1;              /* fine dell'audio, quando è nota (per i salti oltre) */
static double starve_since;                /* da quando l'audio manca con le code piene */
static SDL_Texture *vtex;
static int tex_w, tex_h;
static double sar = 1;
static int shown, dropped;                 /* conteggi della finestra di 3 s */
static double win_start;

static void fq_clear(void) {
    while (fq_n) {
        av_frame_free(&fq[fq_r]);
        fq_r = (fq_r + 1) % FQ;
        fq_n--;
    }
    fq_r = 0;
}

static void pq_clear(void) {
    while (pq_n) {
        av_packet_free(&pq[pq_r]);
        pq_r = (pq_r + 1) % PQ;
        pq_n--;
    }
    pq_r = pq_bytes = 0;
}

static bool seek_pending(void) {
    SDL_LockMutex(mtx);
    bool sk = seek_req;
    SDL_UnlockMutex(mtx);
    return sk;
}

/* audio decodificato: convertito in 16 bit stereo e messo nell'anello (thread di lettura) */
static void push_audio(AVFrame *fr) {
    double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * atb : -1;
    double fd = fr->sample_rate > 0 ? (double)fr->nb_samples / fr->sample_rate : 0;
    SDL_LockMutex(mtx);
    double db = drop_before;
    SDL_UnlockMutex(mtx);
    if (a_mute || (pts >= 0 && pts + fd < db)) return;   /* senza uscita, o prima del punto chiesto */
    int max = swr_get_out_samples(swr, fr->nb_samples);
    static Uint8 *buf;
    static int bcap;
    if (max * 4 > bcap) {
        Uint8 *nb = realloc(buf, (size_t)max * 4);
        if (!nb) return;
        buf = nb;
        bcap = max * 4;
    }
    Uint8 *o[1] = { buf };
    int got = swr_convert(swr, o, max, (const uint8_t **)fr->extended_data, fr->nb_samples);
    if (got <= 0) return;
    int bytes = got * 4, done = 0;
    while (done < bytes && !d_stop && !a_mute && !seek_pending()) {
        SDL_LockMutex(amtx);
        if (pts >= 0 && ar_n == 0) ar_end_pts = pts;   /* l'anello vuoto riparte dal tempo del pezzo */
        /* Anello pieno mentre si aspetta il primo fotogramma (audio molto prima del video nel
           file): nessuno lo sta suonando, aspettare fermerebbe la lettura prima del video. Si
           tiene l'audio più recente e si butta il più vecchio (verifica Codex NEW-003). */
        if (a_hold && aring_cap - ar_n < bytes - done) ring_drop(bytes - done - (aring_cap - ar_n));
        int space = aring_cap - ar_n, n = bytes - done < space ? bytes - done : space;
        n -= n % 4;
        int w = (ar_r + ar_n) % aring_cap;
        for (int i = 0; i < n; i++) aring[(w + i) % aring_cap] = buf[done + i];
        ar_n += n;
        ar_end_pts += (double)n / abps;
        SDL_UnlockMutex(amtx);
        done += n;
        if (done < bytes) SDL_Delay(10);   /* anello pieno: si aspetta che suoni */
    }
}

/* un fotogramma in coda (convertito in YUV 4:2:0 se serve), se è ancora del salto in corso */
static void push_video(AVFrame *fr, int serial, double *last_pts) {
    double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * vtb : *last_pts + fdur;
    *last_pts = pts;
    SDL_LockMutex(mtx);
    double db = drop_before;
    bool stale = serial != vserial;
    SDL_UnlockMutex(mtx);
    if (stale || pts + fdur * 0.5 < db) return;
    AVFrame *f = av_frame_alloc();
    if (!f) return;
    if (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_YUVJ420P) av_frame_move_ref(f, fr);
    else {
        sws = sws_getCachedContext(sws, fr->width, fr->height, fr->format, fr->width, fr->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
        f->format = AV_PIX_FMT_YUV420P;
        f->width = fr->width;
        f->height = fr->height;
        if (!sws || av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return; }
        sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, f->data, f->linesize);
    }
    SDL_LockMutex(mtx);
    while (fq_n == FQ && serial == vserial && !d_stop) SDL_CondWaitTimeout(cnd, mtx, 100);
    if (serial != vserial || d_stop) { SDL_UnlockMutex(mtx); av_frame_free(&f); return; }
    int w = (fq_r + fq_n) % FQ;
    fq[w] = f;
    fq_pts[w] = pts;
    fq_n++;
    vframes_ok++;
    SDL_UnlockMutex(mtx);
}

static int video_thread(void *u) {
    (void)u;
    AVFrame *fr = av_frame_alloc();
    int my_serial = -1;
    bool light = false;
    double last_pts = 0;
    while (fr && !d_stop) {
        SDL_LockMutex(mtx);
        while (!pq_n && !d_stop) SDL_CondWaitTimeout(cnd, mtx, 100);
        if (d_stop) { SDL_UnlockMutex(mtx); break; }
        AVPacket *pkt = pq[pq_r];
        int serial = pq_serial[pq_r];
        pq_r = (pq_r + 1) % PQ;
        pq_n--;
        if (pkt) pq_bytes -= pkt->size;
        bool stale = serial != vserial;
        SDL_CondBroadcast(cnd);
        SDL_UnlockMutex(mtx);
        if (stale) { av_packet_free(&pkt); continue; }
        if (serial != my_serial) {   /* primo pacchetto dopo un salto: il decoder riparte pulito */
            avcodec_flush_buffers(vdec);
            my_serial = serial;
        }
        if (go_light != light) {
            light = go_light;
            vdec->skip_loop_filter = light ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
            ui_log("video: decodifica %s", light ? "alleggerita (niente filtro antiblocchi)" : "normale");
        }
        if (avcodec_send_packet(vdec, pkt) < 0 && pkt) { SDL_LockMutex(mtx); vframes_bad++; SDL_UnlockMutex(mtx); }
        for (;;) {
            int e = avcodec_receive_frame(vdec, fr);
            if (e < 0) {
                if (e != AVERROR(EAGAIN) && e != AVERROR_EOF) { SDL_LockMutex(mtx); vframes_bad++; SDL_UnlockMutex(mtx); }
                break;
            }
            push_video(fr, serial, &last_pts);
            av_frame_unref(fr);
        }
        if (!pkt) {   /* fine del file: tutto decodificato */
            SDL_LockMutex(mtx);
            if (serial == vserial) eof_serial = serial;
            SDL_UnlockMutex(mtx);
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&fr);
    return 0;
}

static void audio_decode(AVCodecContext *c, const AVPacket *pkt, AVFrame *fr) {
    if (avcodec_send_packet(c, pkt) < 0) return;
    while (!d_stop && avcodec_receive_frame(c, fr) >= 0) {
        push_audio(fr);
        av_frame_unref(fr);
    }
}

/* pacchetto video in coda (NULL: fine del file); false se c'è un salto o la chiusura */
static bool pq_push(AVPacket *pkt, int serial) {
    SDL_LockMutex(mtx);
    while ((pq_n == PQ || pq_bytes > PQ_BYTES) && !seek_req && !d_stop) SDL_CondWaitTimeout(cnd, mtx, 100);
    bool ok = !seek_req && !d_stop;
    if (ok) {
        int w = (pq_r + pq_n) % PQ;
        pq[w] = pkt;
        pq_serial[w] = serial;
        pq_n++;
        if (pkt) pq_bytes += pkt->size;
        SDL_CondBroadcast(cnd);
    }
    SDL_UnlockMutex(mtx);
    return ok;
}

static int read_thread(void *u) {
    (void)u;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int serial = 0;
    bool at_end = false;
    while (pkt && fr && !d_stop) {
        SDL_LockMutex(mtx);
        if (seek_req) {
            double t = seek_to;
            seek_req = false;
            serial = vserial;
            pq_clear();
            SDL_UnlockMutex(mtx);
            int64_t ts = (int64_t)(t * AV_TIME_BASE);
            if (avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, 0) < 0 && t > 0) ui_log("video: salto a %.0f s non riuscito", t);
            if (adec) avcodec_flush_buffers(adec);
            if (swr) swr_init(swr);
            SDL_LockMutex(amtx);
            ar_r = ar_n = 0;
            ar_end_pts = clk_pts = play_pos = t;
            clk_run = a_ended = false;
            SDL_UnlockMutex(amtx);
            at_end = false;
            continue;
        }
        if (at_end) {   /* fine del file: si aspetta un salto o la chiusura */
            SDL_CondWaitTimeout(cnd, mtx, 100);
            SDL_UnlockMutex(mtx);
            continue;
        }
        SDL_UnlockMutex(mtx);
        int r = av_read_frame(fmt, pkt);
        if (r == AVERROR(EAGAIN)) { SDL_Delay(5); continue; }
        if (r < 0) {   /* fine (o errore di lettura): si svuotano i decoder */
            if (r != AVERROR_EOF) ui_log("video: lettura interrotta (%s)", av_err2str(r));
            if (adec) audio_decode(adec, NULL, fr);
            if (pq_push(NULL, serial)) {
                at_end = true;
                SDL_LockMutex(amtx);
                a_ended = true;   /* non arriverà altro audio fino al prossimo salto (NEW-002) */
                SDL_UnlockMutex(amtx);
            }
            continue;
        }
        if (pkt->stream_index == vidx) {
            AVPacket *q = av_packet_alloc();
            if (q) {
                av_packet_move_ref(q, pkt);
                if (!pq_push(q, serial)) av_packet_free(&q);
            }
        } else if (pkt->stream_index == aidx && adec) audio_decode(adec, pkt, fr);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    return 0;
}

/* FFmpeg chiede a ogni lettura se deve smettere: per la chiusura (d_stop) e, mentre si apre un
   video nel thread dell'interfaccia, dopo una scadenza (verifica Codex NEW-001). Una lettura ferma
   dentro il sistema non torna comunque: per quella resta la guardia. */
static _Atomic double io_deadline;   /* 0: nessuna */
static int io_interrupt(void *u) {
    (void)u;
    double dl = io_deadline;
    return d_stop || (dl > 0 && now_s() > dl);
}

static void player_close(void) {
    guard_begin();   /* i thread finiscono, o l'app si chiude: i contesti si liberano solo dopo */
    d_stop = true;
    SDL_LockMutex(mtx);
    SDL_CondBroadcast(cnd);
    SDL_UnlockMutex(mtx);
    if (dthr) SDL_WaitThread(dthr, NULL);
    if (vthr) SDL_WaitThread(vthr, NULL);
    dthr = vthr = NULL;
    SDL_LockMutex(mtx);
    fq_clear();
    pq_clear();
    SDL_UnlockMutex(mtx);
    SDL_LockMutex(amtx);
    ar_r = ar_n = 0;
    a_hold = true;
    SDL_UnlockMutex(amtx);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    swr_free(&swr);
    sws_freeContext(sws);
    sws = NULL;
    avformat_close_input(&fmt);
    if (vtex) SDL_DestroyTexture(vtex);
    vtex = NULL;
    playing = false;
    io_deadline = 0;
    guard_end();
}

static const char *open_err;   /* chiave del messaggio per l'ultimo video che non si apre */
static char open_detail[64];

static bool player_open_guarded(int i, double start);
static bool player_open(int i, double start) {
    guard_begin();
    bool ok = player_open_guarded(i, start);
    guard_end();
    return ok;
}

static bool player_open_guarded(int i, double start) {
    char p[1700];
    vid_path(i, p, sizeof p);
    open_err = "video.bad_file";
    open_detail[0] = 0;
    d_stop = false;
    /* apertura e ricerca degli stream nel thread dell'interfaccia: al massimo 6 s */
    io_deadline = now_s() + 6;
    fmt = avformat_alloc_context();
    if (!fmt) return false;
    fmt->interrupt_callback = (AVIOInterruptCB){ io_interrupt, NULL };
    if (avformat_open_input(&fmt, p, NULL, NULL) < 0) { ui_log("video: %s non si apre", vids[i].rel); io_deadline = 0; return false; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { ui_log("video: %s senza stream leggibili", vids[i].rel); player_close(); return false; }
    io_deadline = 0;
    vidx = aidx = -1;
    for (unsigned s = 0; s < fmt->nb_streams; s++) {
        AVStream *st = fmt->streams[s];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) && vidx < 0) vidx = (int)s;
    }
    if (vidx < 0) { open_err = "video.no_video"; ui_log("video: %s senza video", vids[i].rel); player_close(); return false; }
    for (unsigned s = 0; s < fmt->nb_streams; s++) fmt->streams[s]->discard = AVDISCARD_ALL;
    AVStream *vs = fmt->streams[vidx];
    vs->discard = AVDISCARD_DEFAULT;
    const AVCodec *vc = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!vc) {
        open_err = "video.unsupported";
        snprintf(open_detail, sizeof open_detail, "%s", avcodec_get_name(vs->codecpar->codec_id));
        ui_log("video: %s, video %s non supportato", vids[i].rel, open_detail);
        player_close();
        return false;
    }
    int w = vs->codecpar->width, h = vs->codecpar->height;
    if ((long)w * h > MAX_PIXELS) {
        open_err = "video.too_big";
        snprintf(open_detail, sizeof open_detail, "%dx%d", w, h);
        ui_log("video: %s troppo grande (%dx%d)", vids[i].rel, w, h);
        player_close();
        return false;
    }
    vdec = avcodec_alloc_context3(vc);
    if (!vdec || avcodec_parameters_to_context(vdec, vs->codecpar) < 0) { player_close(); return false; }
    vdec->thread_count = 4;
    vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    vdec->pkt_timebase = vs->time_base;
    if (avcodec_open2(vdec, vc, NULL) < 0) { ui_log("video: decoder %s non parte", vc->name); player_close(); return false; }
    vtb = av_q2d(vs->time_base);
    AVRational fr = av_guess_frame_rate(fmt, vs, NULL);
    fdur = fr.num > 0 && fr.den > 0 ? av_q2d(av_inv_q(fr)) : 1.0 / 30;
    if (fdur <= 0 || fdur > 1) fdur = 1.0 / 30;
    AVRational sr = av_guess_sample_aspect_ratio(fmt, vs, NULL);
    sar = sr.num > 0 && sr.den > 0 ? av_q2d(sr) : 1;
    rot = 0;
    const AVPacketSideData *sd = av_packet_side_data_get(vs->codecpar->coded_side_data, vs->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd && sd->size >= 9 * 4) {
        double a = -av_display_rotation_get((const int32_t *)sd->data);
        if (!isnan(a)) rot = ((int)lround(a / 90) * 90 % 360 + 360) % 360;
    }
    /* audio: il migliore legato al video; senza decoder o senza uscita si va avanti muti */
    const AVCodec *ac = NULL;
    aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vidx, &ac, 0);
    has_audio = false;
    if (aidx >= 0 && !ac) {
        char m[160];
        snprintf(m, sizeof m, FT("video.no_audio_codec"), avcodec_get_name(fmt->streams[aidx]->codecpar->codec_id));
        ui_toast(m);
        ui_log("video: audio %s non supportato", avcodec_get_name(fmt->streams[aidx]->codecpar->codec_id));
        aidx = -1;
    }
    if (aidx >= 0 && audio_open(false)) {
        AVStream *as = fmt->streams[aidx];
        adec = avcodec_alloc_context3(ac);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (adec) adec->pkt_timebase = as->time_base;
        if (adec && avcodec_parameters_to_context(adec, as->codecpar) >= 0 && avcodec_open2(adec, ac, NULL) >= 0 &&
            swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, arate, &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, NULL) >= 0 &&
            swr_init(swr) >= 0) {
            as->discard = AVDISCARD_DEFAULT;
            atb = av_q2d(as->time_base);
            has_audio = true;
        } else {
            ui_log("video: audio %s non parte", ac->name);
            avcodec_free_context(&adec);
            swr_free(&swr);
            aidx = -1;
        }
    } else aidx = -1;
    /* durata: dal contenitore, poi dal più lungo degli stream, poi dal probing solo se già
       pubblicato (verifica Codex NEW-004: il thread delle durate può scriverla in questo momento) */
    /* VIDEO_PROVA_SENZA_DURATA=1: solo per le prove, si ignora la durata del contenitore */
    bool no_cont = getenv("VIDEO_PROVA_SENZA_DURATA") != NULL;
    duration = fmt->duration > 0 && !no_cont ? fmt->duration / (double)AV_TIME_BASE : 0;
    if (duration <= 0) {   /* il più lungo di tutti gli stream (verifica Codex NEW-007), poi il probing */
        for (unsigned s = 0; s < fmt->nb_streams; s++) {
            AVStream *st = fmt->streams[s];
            double d = st->duration > 0 ? st->duration * av_q2d(st->time_base) : 0;
            if (d > duration) duration = d;
        }
        if (!no_cont && atomic_load_explicit(&vids[i].probed, memory_order_acquire) && vids[i].dur > duration) duration = vids[i].dur;
    }
    ui_log("video: %s, %s %dx%d %.2f fps%s%s, audio %s, %.0f s, da %.0f s", vids[i].rel, vc->name, w, h, 1 / fdur,
           rot ? ", girato" : "", sar != 1 ? ", pixel non quadrati" : "", has_audio ? ac->name : "no", duration, start);
    /* stato iniziale: si parte con un salto, anche da zero (i decoder partono puliti) */
    go_light = false;
    light_told = false;
    seek_req = true;
    seek_to = start;
    drop_before = start;
    vserial++;
    eof_serial = -1;
    fq_r = fq_n = 0;
    pq_r = pq_n = pq_bytes = 0;
    vframes_ok = vframes_bad = 0;
    SDL_LockMutex(amtx);
    ar_r = ar_n = 0;
    ar_end_pts = clk_pts = play_pos = wall_pts = start;
    clk_run = a_ended = a_wall = false;
    a_hold = true;
    a_pause = false;
    a_mute = false;
    SDL_UnlockMutex(amtx);
    audio_end = -1;
    starve_since = 0;
    cur = i;
    cur_pts = start;
    paused = false;
    waiting = true;
    wait_since = now_s();
    shown = dropped = 0;
    win_start = now_s();
    playing = true;
    dthr = SDL_CreateThread(read_thread, "lettura", NULL);
    vthr = dthr ? SDL_CreateThread(video_thread, "video", NULL) : NULL;
    if (!vthr) { player_close(); return false; }
    return true;
}

/* tempo del video adesso */
static double clock_now(void) {
    if (waiting) return cur_pts;
    if (paused) return paused_at_clk;
    SDL_LockMutex(amtx);
    if (audio_clock() && !a_wall && a_ended && ar_n == 0) {
        /* audio finito davvero (non solo in ritardo): si continua con l'orologio monotono dal suo
           ultimo tempo (verifica Codex NEW-002) */
        a_wall = true;
        wall_pts = clk_pts;
        wall_time = clk_time;
        audio_end = clk_pts;
        ui_log("video: audio finito a %.1f s, il video continua", clk_pts);
    }
    double p;
    if (!audio_clock() || a_wall) p = wall_pts + (now_s() - wall_time);
    else {
        double e = now_s() - clk_time;
        p = clk_pts + (clk_run ? (e < 0 ? 0 : e > 0.2 ? 0.2 : e) : 0);
    }
    SDL_UnlockMutex(amtx);
    return p;
}

static void bar_show(void) { bar_until = UI.t + 3; }

static void set_pause(bool on) {
    if (on == paused || waiting) return;
    if (on) paused_at_clk = clock_now();
    paused = on;
    SDL_LockMutex(amtx);
    a_pause = on;
    clk_time = now_s();
    if (!on) { wall_pts = paused_at_clk; wall_time = now_s(); }   /* orologio monotono: riparte da qui */
    SDL_UnlockMutex(amtx);
    starve_since = 0;
    /* dopo 4 s di pausa l'altoparlante a riposo è spento: si riaccende alla ripresa (A62-005) */
    if (!on) plat_amp_wake();
}

static void seek_rel(double d) {
    double pos = waiting ? seek_to : clock_now(), t = pos + d;
    if (duration > 0 && t > duration - 3) t = duration - 3;
    if (t < 0) t = 0;
    SDL_LockMutex(mtx);
    seek_req = true;
    seek_to = t;
    drop_before = t;
    vserial++;
    eof_serial = -1;
    fq_clear();
    SDL_CondBroadcast(cnd);
    SDL_UnlockMutex(mtx);
    SDL_LockMutex(amtx);
    ar_r = ar_n = 0;
    a_hold = true;
    clk_run = a_ended = a_wall = false;
    SDL_UnlockMutex(amtx);
    starve_since = 0;
    cur_pts = t;
    waiting = true;
    wait_since = now_s();
    bar_show();
}

static void upload(AVFrame *f) {
    if (!vtex || tex_w != f->width || tex_h != f->height) {
        if (vtex) SDL_DestroyTexture(vtex);
        vtex = SDL_CreateTexture(UI.ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
        tex_w = f->width;
        tex_h = f->height;
        if (vtex) SDL_SetTextureScaleMode(vtex, SDL_ScaleModeLinear);
        else ui_log("video: texture %dx%d non creata (%s)", f->width, f->height, SDL_GetError());
    }
    if (vtex) SDL_UpdateYUVTexture(vtex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
}

/* fine del video o uscita: si ricorda dove si era */
static void remember(void) {
    if (cur < 0) return;
    double pos = waiting ? seek_to : cur_pts, dur = duration;
    bool done = dur > 0 && pos > dur - finished_margin(dur);
    if (done || pos >= 5) { mark_set(vids[cur].rel, done ? -1 : pos, dur); return; }
    mark_t *m = mark_find(vids[cur].rel);   /* appena iniziato: niente da ricordare */
    if (!m) return;
    memmove(m, m + 1, (size_t)(&marks[nmarks] - (m + 1)) * sizeof *m);
    nmarks--;
    marks_save();
}

/* un giro del lettore: prende il fotogramma da mostrare; true se c'è qualcosa di nuovo */
static bool player_step(bool *ended) {
    *ended = false;
    AVFrame *show = NULL;
    double show_pts = 0;
    SDL_LockMutex(mtx);
    if (waiting) {
        if (fq_n > 0) {
            show = fq[fq_r];
            show_pts = fq_pts[fq_r];
            fq_r = (fq_r + 1) % FQ;
            fq_n--;
        }
    } else if (!paused) {
        double c = clock_now();
        while (fq_n > 0 && fq_pts[fq_r] <= c + 0.004) {
            if (show) { av_frame_free(&show); dropped++; }
            show = fq[fq_r];
            show_pts = fq_pts[fq_r];
            fq_r = (fq_r + 1) % FQ;
            fq_n--;
        }
    }
    bool eof = eof_serial == vserial && fq_n == 0;
    int ok = vframes_ok, bad = vframes_bad;
    if (show) SDL_CondBroadcast(cnd);
    SDL_UnlockMutex(mtx);
    if (show) {
        upload(show);
        av_frame_free(&show);
        cur_pts = show_pts;
        shown++;
        if (waiting) {   /* primo fotogramma dopo l'apertura o un salto: l'orologio parte da qui */
            waiting = false;
            paused_at_clk = show_pts;
            SDL_LockMutex(amtx);
            /* l'audio messo da parte prima di questo fotogramma non si suona (NEW-003) */
            if (ar_n > 0 && ring_front() < show_pts - 0.05) ring_drop((int)((show_pts - ring_front()) * abps));
            a_hold = false;
            clk_pts = play_pos = wall_pts = show_pts;
            clk_time = wall_time = now_s();
            clk_run = false;
            /* oltre la fine già nota dell'audio: subito l'orologio monotono */
            if (audio_clock() && audio_end >= 0 && show_pts >= audio_end - 0.05) a_wall = true;
            SDL_UnlockMutex(amtx);
        }
    }
    /* finito: niente più fotogrammi e l'audio ha suonato tutto */
    if (eof && !waiting) {
        int left = 0;
        if (audio_clock()) { SDL_LockMutex(amtx); left = a_wall ? 0 : ar_n; SDL_UnlockMutex(amtx); }
        if (!left) *ended = true;
    }
    /* Audio che non arriva più: anello vuoto, orologio fermo, e la lettura bloccata dalle code
       video piene (l'audio è troppo lontano nel file, o il decoder non dà niente). Dopo 1,5 s così
       non è una semplice attesa: orologio monotono da dove si era (NEW-002). */
    if (audio_clock() && !waiting && !paused) {
        SDL_LockMutex(mtx);
        bool full = fq_n == FQ && (pq_n == PQ || pq_bytes > PQ_BYTES);
        SDL_UnlockMutex(mtx);
        SDL_LockMutex(amtx);
        bool starving = !a_wall && !clk_run && ar_n == 0 && full;
        if (!starving) starve_since = 0;
        else if (!starve_since) starve_since = now_s();
        else if (now_s() - starve_since > 1.5) {
            a_wall = true;
            wall_pts = clk_pts;
            wall_time = now_s();
            starve_since = 0;
            ui_log("video: l'audio non arriva (%.1f s): orologio del sistema", clk_pts);
        }
        SDL_UnlockMutex(amtx);
    }
    if (eof && waiting) *ended = true;   /* un salto oltre l'ultimo fotogramma */
    /* nessun fotogramma dopo 8 s: il video non si decodifica */
    if (waiting && now_s() - wait_since > 8 && !ok) {
        ui_log("video: nessun fotogramma (%d errori)", bad);
        open_err = "video.bad_file";
        *ended = true;
    }
    /* tanti fotogrammi saltati per 3 s di fila: si alleggerisce, poi si avvisa */
    double t = now_s();
    if (t - win_start >= 3) {
        if (!paused && !waiting && shown + dropped > 10 && dropped * 3 > shown + dropped) {
            if (!go_light) go_light = true;
            else if (!light_told) { light_told = true; ui_toast(FT("video.too_heavy")); ui_log("video: troppo pesante (%d mostrati, %d saltati in 3 s)", shown, dropped); }
        }
        shown = dropped = 0;
        win_start = t;
    }
    return show != NULL;
}

/* Dopo una sospensione (gamewatch ferma l'app, la console dorme, poi la riprende) l'uscita audio
   può non rispondere più: si riapre, e il video resta in pausa. CLOCK_BOOTTIME conta il sonno,
   CLOCK_MONOTONIC no. */
static void after_sleep(void) {
    ui_log("video: ripresa dalla sospensione, riapro l'audio");
    if (playing && !waiting) set_pause(true);
    if (adev) {   /* l'anello resta: lo legge la nuova richiamata */
        guard_begin();
        audio_close();
        /* VIDEO_PROVA_RIAPERTURA=fallita: solo per le prove, la riapertura non riesce */
        const char *pr = getenv("VIDEO_PROVA_RIAPERTURA");
        bool ok = !(pr && !strcmp(pr, "fallita")) && audio_open(true);
        if (!ok && playing && has_audio) {   /* senza uscita: si va avanti muti fino alla fine */
            ui_log("video: uscita audio persa, il video continua senza suono");
            a_mute = true;
            SDL_LockMutex(amtx);
            ar_r = ar_n = 0;
            a_wall = true;
            wall_pts = paused_at_clk;
            wall_time = now_s();
            SDL_UnlockMutex(amtx);
        }
        guard_end();
    }
    if (playing) bar_show();
}

static void resume_check(void) {
    /* VIDEO_PROVA_SOSPENSIONE=secondi: solo per le prove (sul Mac non c'è CLOCK_BOOTTIME), una
       sospensione finta dopo tanti secondi di video */
    static double fake_at = -1;
    const char *fs = getenv("VIDEO_PROVA_SOSPENSIONE");
    if (fs && playing && fake_at < 0) fake_at = now_s() + atof(fs);
    if (fake_at > 0 && now_s() > fake_at) { fake_at = 0; after_sleep(); return; }
#ifdef CLOCK_BOOTTIME
    struct timespec bt, mt;
    static double last = -1;
    clock_gettime(CLOCK_BOOTTIME, &bt);
    clock_gettime(CLOCK_MONOTONIC, &mt);
    double gap = (bt.tv_sec - mt.tv_sec) + (bt.tv_nsec - mt.tv_nsec) / 1e9;
    bool slept = last >= 0 && gap - last > 1.0;
    last = gap;
    if (slept) after_sleep();
#endif
}

/* ------------------------------------------------------------ disegno */

static void hms(double s, char *out, size_t n) {
    if (s < 0) s = 0;
    int t = (int)s;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void video_draw(void) {
    SDL_SetRenderDrawColor(UI.ren, 0, 0, 0, 255);
    SDL_RenderClear(UI.ren);
    if (!vtex) return;
    double aw = tex_w * (sar > 0 ? sar : 1), ah = tex_h;
    bool side = rot == 90 || rot == 270;
    double bw = side ? ah : aw, bh = side ? aw : ah;   /* misure a schermo, dopo la rotazione */
    double s = fmin(UI.w / bw, UI.h / bh);
    int dw = (int)lround(bw * s), dh = (int)lround(bh * s);
    int cx = UI.w / 2, cy = UI.h / 2;
    if (!rot) {
        SDL_Rect d = { cx - dw / 2, cy - dh / 2, dw, dh };
        SDL_RenderCopy(UI.ren, vtex, NULL, &d);
    } else {
        int rw = side ? dh : dw, rh = side ? dw : dh;
        SDL_Rect d = { cx - rw / 2, cy - rh / 2, rw, rh };
        SDL_RenderCopyEx(UI.ren, vtex, NULL, &d, rot, NULL, SDL_FLIP_NONE);
    }
}

static void pause_icon(int cx, int cy, int s, color_t c) {
    int bw = s * 28 / 100, gap = s * 24 / 100;
    ui_rrect(cx - bw - gap / 2, cy - s / 2, bw, s, bw / 3, c);
    ui_rrect(cx + gap / 2, cy - s / 2, bw, s, bw / 3, c);
}

/* barra delle informazioni: titolo, tempo, durata, avanzamento */
static void info_draw(void) {
    theme_t *t = &UI.th;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), mx = M(t->margin);
    int bh = fnh + 2 * fsh + M(0.1f), y0 = UI.h - bh;
    ui_vgradient(0, y0 - M(0.06f), UI.w, M(0.06f), rgba(0, 0, 0, 0), rgba(0, 0, 0, 170));
    ui_rect(0, y0, UI.w, bh, rgba(0, 0, 0, 170));
    char fit[300], a[24], b[24], line[64];
    double pos = waiting ? cur_pts : clock_now();
    if (duration > 0 && pos > duration) pos = duration;
    hms(pos, a, sizeof a);
    hms(duration, b, sizeof b);
    snprintf(line, sizeof line, "%s / %s", a, duration > 0 ? b : "--:--");
    int tw = ui_text_w(fs, line), ty = y0 + M(0.025f);
    color_t white = rgba(255, 255, 255, 255), dim = rgba(200, 200, 200, 255);
    int icon = paused ? fnh : 0;
    if (paused) pause_icon(mx + icon / 2, ty + fnh / 2, fnh * 7 / 10, white);
    ui_text_fit(fn, vids[cur].name, UI.w - 2 * mx - tw - icon - M(0.05f), fit, sizeof fit);
    ui_text(fn, fit, mx + (icon ? icon + M(0.015f) : 0), ty, white, ALIGN_LEFT);
    ui_text(fs, line, UI.w - mx, ty + (fnh - fsh) / 2, dim, ALIGN_RIGHT);
    int py = ty + fnh + M(0.03f), ph = M(0.008f) > 3 ? M(0.008f) : 3, pw = UI.w - 2 * mx;
    ui_rrect(mx, py, pw, ph, ph / 2, rgba(255, 255, 255, 60));
    if (duration > 0) {
        int fw = (int)(pw * (pos / duration));
        if (fw > 0) ui_rrect(mx, py, fw, ph, ph / 2, t->accent);
        ui_circle(mx + fw, py + ph / 2, ph * 2, t->accent);
    }
    /* i tasti, al centro sotto la barra: glifi del tema in bianco su nero */
    theme_t keep = *t;
    t->text = rgba(235, 235, 235, 255);
    t->bg = rgba(0, 0, 0, 255);
    const char *k[] = { "A", FT(paused ? "video.continue" : "video.pause"), "<>", "10 s", "L", "-1 min", "R", "+1 min", "SELECT", FT("video.bar"), "B", FT("video.list") };
    int g = fsh, gap = M(0.035f), total = 0, n = (int)(sizeof k / sizeof k[0]) / 2;
    #define GW(b) (!strcmp(b, "<>") ? g * 3 / 2 : strlen(b) == 1 ? g : ui_text_w(fs, b) + g / 2)
    for (int i = 0; i < n; i++) total += (i ? gap : 0) + GW(k[2 * i]) + M(0.01f) + ui_text_w(fs, k[2 * i + 1]);
    int x = (UI.w - total) / 2, ky = py + ph + M(0.012f) + fsh / 2;
    if (total <= UI.w - 2 * mx)
        for (int i = 0; i < n; i++) {
            int gw = GW(k[2 * i]);
            ui_button_glyph(k[2 * i], x + gw / 2, ky, g);
            x += gw + M(0.01f);
            x += ui_text(fs, k[2 * i + 1], x, ky - fsh / 2, dim, ALIGN_LEFT) + gap;
        }
    #undef GW
    *t = keep;
}

static void player_draw(void) {
    video_draw();
    bool bar = bar_pinned || paused || UI.t < bar_until;
    if (bar) info_draw();
    if (waiting && now_s() - wait_since > 0.4) {   /* caricamento: tre punti che si accendono a turno */
        int r = M(0.012f), g = M(0.04f), cy = UI.h / 2;
        for (int k = 0; k < 3; k++) {
            int a = (int)(UI.t * 3) % 3 == k ? 255 : 90;
            ui_circle(UI.w / 2 + (k - 1) * g, cy, r, rgba(255, 255, 255, a));
        }
    }
}

static void list_draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    ui_topbar(FT("video.title"));
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    int top = M(t->topbar_h) + M(0.02f);
    char count[64], fit[300];
    snprintf(count, sizeof count, FT("video.count"), nvids);
    ui_text(fs, count, mx + W, top, t->text_dim, ALIGN_RIGHT);
    ui_text(fs, FT("video.folder"), mx, top, t->text_dim, ALIGN_LEFT);
    int ly = top + fsh + M(0.02f), rh = fnh + M(0.034f), bot = UI.h - ui_keybar_h() - M(0.01f), vis = (bot - ly) / rh;
    if (vis < 1) vis = 1;
    if (!nvids) {
        char lines[4][160];
        int n = ui_wrap(fn, FT("video.empty"), W, lines, 4);
        for (int k = 0; k < n; k++) ui_text(fn, lines[k], UI.w / 2, ly + rh + k * fnh, t->text_dim, ALIGN_CENTER);
    } else {
        int first = (int)scroll;
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        if (first < 0) first = 0;
        scroll = (float)first;
        for (int i = first; i < nvids && i < first + vis; i++) {
            int ry = ly + (i - first) * rh;
            bool on = i == sel;
            if (on) ui_rrect(mx - M(0.012f), ry, W + M(0.024f), rh, M(t->panel_radius), with_alpha(t->accent, 230));
            color_t tc = on ? t->accent_text : t->text, dc = on ? t->accent_text : t->text_dim;
            /* a destra: cartella, durata e risoluzione (quando lette) */
            char info[200] = "", d[24];
            if (vids[i].probed) {
                if (vids[i].dur > 0) hms(vids[i].dur, d, sizeof d); else d[0] = 0;
                const char *res = vids[i].h >= 2000 ? "4K" : vids[i].h >= 1000 ? "1080p" : vids[i].h >= 700 ? "720p" : vids[i].h >= 400 ? "480p" : vids[i].h > 0 ? "SD" : "";
                snprintf(info, sizeof info, "%s%s%s", d, d[0] && res[0] ? " · " : "", res);
            }
            char right[400];
            snprintf(right, sizeof right, "%s%s%s", vids[i].folder, vids[i].folder[0] && info[0] ? "  ·  " : "", info);
            int aw = right[0] ? ui_text_w(fs, right) : 0;
            if (aw > W * 45 / 100) aw = W * 45 / 100;
            if (aw) { ui_text_fit(fs, right, W * 45 / 100, fit, sizeof fit); ui_text(fs, fit, mx + W - M(0.005f), ry + (rh - fsh) / 2, dc, ALIGN_RIGHT); }
            ui_text_fit(fn, vids[i].name, W - aw - M(0.04f), fit, sizeof fit);
            ui_text(fn, fit, mx + M(0.005f), ry + (rh - fnh) / 2, tc, ALIGN_LEFT);
            /* punto raggiunto: una linea sotto il nome; piena e tenue se visto */
            mark_t *m = mark_find(vids[i].rel);
            if (m && (m->pos > 0 || m->pos < 0)) {
                int lh = M(0.005f) > 2 ? M(0.005f) : 2, lw = W - aw - M(0.04f), lyy = ry + rh - lh - M(0.006f);
                if (lw > W / 2) lw = W / 2;
                float frac = m->pos < 0 ? 1 : m->dur > 0 ? (float)(m->pos / m->dur) : 0;
                if (frac > 1) frac = 1;
                ui_rrect(mx + M(0.005f), lyy, lw, lh, lh / 2, with_alpha(dc, 60));
                ui_rrect(mx + M(0.005f), lyy, (int)(lw * frac), lh, lh / 2, m->pos < 0 ? with_alpha(dc, 140) : on ? t->accent_text : t->accent);
            }
        }
    }
    const char *h[] = { "B", FT("video.quit"), NULL, "A", FT("video.open"), NULL };
    ui_keybar(h, 6);
}

/* domanda: riprendere da dove si era o ricominciare */
static bool ask_open;
static int ask_sel = 1;
static double ask_pos;

static void ask_draw(void) {
    theme_t *t = &UI.th;
    TTF_Font *fn = UI.f_normal, *ft = UI.f_title;
    int fnh = TTF_FontHeight(fn), fth = TTF_FontHeight(ft), pad = M(0.04f);
    int w = M(0.9f) < UI.w - M(0.04f) ? M(0.9f) : UI.w - M(0.04f);
    char body[200], tm[24], lines[3][160], fit[300];
    hms(ask_pos, tm, sizeof tm);
    snprintf(body, sizeof body, FT("video.resume_at"), tm);
    int n = ui_wrap(fn, body, w - 2 * pad, lines, 3);
    int h = pad + fth + M(0.02f) + n * fnh + M(0.03f) + ui_popup_buttons_h(), x, y;
    app_panel(w, h, &x, &y);
    ui_text_fit(ft, vids[sel].name, w - 2 * pad, fit, sizeof fit);
    ui_text(ft, fit, x + pad, y + pad, t->text, ALIGN_LEFT);
    for (int k = 0; k < n; k++) ui_text(fn, lines[k], x + pad, y + pad + fth + M(0.02f) + k * fnh, t->text_dim, ALIGN_LEFT);
    const char *labels[2] = { FT("video.restart"), FT("video.resume") };
    ui_popup_buttons(x, y, w, h, labels, 2, ask_sel, -1);
}

/* messaggio di errore in una finestra, con OK */
static char err_msg[200];
static void err_draw(void) {
    theme_t *t = &UI.th;
    TTF_Font *fn = UI.f_normal, *ft = UI.f_title;
    int fnh = TTF_FontHeight(fn), fth = TTF_FontHeight(ft), pad = M(0.04f);
    int w = M(0.9f) < UI.w - M(0.04f) ? M(0.9f) : UI.w - M(0.04f);
    char lines[4][160];
    int n = ui_wrap(fn, err_msg, w - 2 * pad, lines, 4);
    int h = pad + fth + M(0.02f) + n * fnh + M(0.03f) + ui_popup_buttons_h(), x, y;
    app_panel(w, h, &x, &y);
    ui_text(ft, FT("video.cannot_play"), x + pad, y + pad, t->text, ALIGN_LEFT);
    for (int k = 0; k < n; k++) ui_text(fn, lines[k], x + pad, y + pad + fth + M(0.02f) + k * fnh, t->text_dim, ALIGN_LEFT);
    const char *labels[1] = { T("common.ok") };
    ui_popup_buttons(x, y, w, h, labels, 1, 0, -1);
}

static void show_error(void) {
    if (open_detail[0] && strstr(FT(open_err), "%s")) snprintf(err_msg, sizeof err_msg, FT(open_err), open_detail);
    else snprintf(err_msg, sizeof err_msg, "%s", FT(open_err));
    ui_sound(SND_ERROR);
}

/* ------------------------------------------------------------ comandi */

static double hold_t;

static void start(int i, double pos) {
    if (player_open(i, pos)) { bar_show(); ui_sound(SND_OK); return; }
    show_error();
}

static void stop_playing(bool finished) {
    if (finished && cur >= 0) ui_log("video: finito (%.1f s)", cur_pts);
    if (finished && cur >= 0) mark_set(vids[cur].rel, -1, duration);
    else remember();
    player_close();
    plat_amp_wake();
}

static void list_input(void) {
    input_t *in = &UI.in;
    if (err_msg[0]) {
        int s = 0, r = ui_popup_input(&s, 1);
        if (r != -1) { err_msg[0] = 0; ui_sound(SND_BACK); }
        return;
    }
    if (ask_open) {
        int r = ui_popup_input(&ask_sel, 2);
        if (r == -2) { ask_open = false; ui_sound(SND_BACK); }
        else if (r >= 0) { ask_open = false; start(sel, r == 1 ? ask_pos : 0); }
        return;
    }
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) { quit = true; return; }
    int old = sel;
    if (nvids) {
        if (in->pressed[BTN_UP]) sel = (sel + nvids - 1) % nvids;
        if (in->pressed[BTN_DOWN]) sel = (sel + 1) % nvids;
        if (in->pressed[BTN_L]) sel = sel > 8 ? sel - 8 : 0;
        if (in->pressed[BTN_R]) sel = sel + 8 < nvids ? sel + 8 : nvids - 1;
    }
    if (sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_A] && nvids) {
        mark_t *m = mark_find(vids[sel].rel);
        if (m && m->pos >= 5) { ask_open = true; ask_sel = 1; ask_pos = m->pos; ui_sound(SND_OK); }
        else start(sel, 0);
    }
}

static void player_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) { stop_playing(false); ui_sound(SND_BACK); return; }
    if (in->pressed[BTN_A]) { set_pause(!paused); bar_show(); }
    if (in->pressed[BTN_SELECT]) { bar_pinned = !bar_pinned; if (!bar_pinned) bar_until = 0; }
    if (in->pressed[BTN_R]) seek_rel(60);
    if (in->pressed[BTN_L]) seek_rel(-60);
    /* sinistra/destra: un salto di 10 s, poi ancora ogni 0,4 s se tenuti */
    int d = in->down[BTN_RIGHT] ? 1 : in->down[BTN_LEFT] ? -1 : 0;
    bool jump = false;
    if (in->pressed[BTN_RIGHT] || in->pressed[BTN_LEFT]) { jump = true; hold_t = UI.t + 0.6; }
    else if (d && UI.t > hold_t) { jump = true; hold_t = UI.t + 0.4; }
    if (jump && d) seek_rel(d * 10.0);
    if (in->any) bar_show();
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "video")) return 1;
    av_log_set_level(AV_LOG_FATAL);
    mtx = SDL_CreateMutex();
    amtx = SDL_CreateMutex();
    cnd = SDL_CreateCond();
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Video", app_root);
    mkdir(dir, 0755);
    scan(dir, "", 0);
    if (nvids > 1) qsort(vids, (size_t)nvids, sizeof *vids, vid_cmp);
    ui_log("video: %d file%s, FFmpeg %s", nvids, scan_cut ? " (elenco incompleto)" : "", av_version_info());
    if (scan_cut) ui_toast(FT("video.list_incomplete"));
    marks_load();
    /* l'audio passa all'app: i suoni dei tasti li mescola la sua richiamata */
    guard_begin();
    ui_audio_takeover(play_fx);
    audio_open(false);
    guard_end();
    SDL_Thread *pt = nvids ? SDL_CreateThread(probe_thread, "durate", NULL) : NULL;
    while (!quit) {
        UI.busy = UI.amp_keep = playing && !paused;   /* niente sospensione automatica mentre si guarda */
        a_gain = (int)(256 * ui_gain());
        if (!UI.automation) ui_clock();   /* anche quando non si disegna: barra, tasti tenuti */
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        resume_check();
        bool fresh = false, ended = false;
        if (playing) {
            player_input();
            if (playing) fresh = player_step(&ended);
            if (playing && ended) {
                bool bad = !vframes_ok;
                stop_playing(!bad);
                if (bad) show_error();
            }
        } else list_input();
        if (playing) {
            /* si disegna solo se c'è qualcosa di nuovo: un fotogramma, la barra, il caricamento */
            bool bar = bar_pinned || paused || UI.t < bar_until + 0.1;
            static int last_sec = -1;
            int sec = (int)clock_now();
            bool need = fresh || UI.automation || ui_toast_active() || waiting || (bar && sec != last_sec) || UI.in.any;
            static bool bar_was;
            if (bar != bar_was) { need = true; bar_was = bar; }
            last_sec = sec;
            if (!need) { plat_apply(&UI.set); SDL_Delay(3); continue; }
            ui_begin();
            player_draw();
            ui_end();
            continue;
        }
        ui_begin();
        list_draw();
        if (ask_open) ask_draw();
        if (err_msg[0]) err_draw();
        ui_end();
    }
    exit_begin();   /* da qui al massimo 5 s (app_exit_begin), anche per i thread e l'audio */
    if (playing) stop_playing(false);
    probe_stop = true;
    if (pt) SDL_WaitThread(pt, NULL);
    audio_close();
    app_quit();
    return 0;
}
