/* vtest: prova di velocità della decodifica video sulla console, per decidere quali formati può
   promettere una futura app Video dello Store. Non fa parte del sistema.

   Per ogni video della sua cartella (mp4, mkv, webm, avi, mov, in ordine di nome) fa due giri:
   - "decodifica": decodifica video (4 thread, a fotogrammi e a fette) e audio (convertito in
     16 bit stereo 48 kHz come farebbe un lettore), senza mostrare niente;
   - "visione": lo stesso, più ogni fotogramma sullo schermo con SDL2 (texture IYUV scalata dal
     renderer, senza vsync), cioè il lavoro vero di un lettore.
   Ogni giro si ferma alla fine del file o dopo --max secondi. Scrive fotogrammi, secondi,
   fotogrammi al secondo e la velocità rispetto al tempo reale in Logs/video-test.txt (sotto
   $ROOT, o --log), una riga alla volta e con fsync, così un'interruzione lascia i risultati.

   Uso: vtest [--dir CARTELLA] [--log FILE] [--font FILE.ttf] [--max SECONDI] [--solo-decodifica]
   Compilato da toolchain/build-ffmpeg.sh (console) o toolchain/vtest/build-mac.sh (Mac). */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define MAXF 64

static FILE *logfile;
static volatile sig_atomic_t stop;
static SDL_Window *win;
static SDL_Renderer *ren;
static TTF_Font *font, *font_big;
static int sw = 1280, sh = 720;

typedef struct {
    char name[256], codec[32], acodec[32], pixfmt[32];
    int w, h, kbps;
    double src_fps;
    double fps[2], speed[2];   /* [0] decodifica, [1] visione */
    int frames[2], done[2];
} result_t;

static result_t res[MAXF];
static int nres;

static void on_sig(int s) { (void)s; stop = 1; }

static void scrivi(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fputs(b, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (logfile) {
        fputs(b, logfile);
        fputc('\n', logfile);
        fflush(logfile);
        fsync(fileno(logfile));
    }
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void read_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static int cpu_mhz(void) {
    char b[64];
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", b, sizeof b);
    return b[0] ? atoi(b) / 1000 : 0;
}

static int temp_c(void) {
    char b[64];
    read_line("/sys/class/thermal/thermal_zone0/temp", b, sizeof b);
    int t = b[0] ? atoi(b) : 0;
    return t > 1000 ? t / 1000 : t;
}

static void shot(const char *tag);

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) stop = 1;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) stop = 1;
    }
}

static void text(TTF_Font *f, const char *s, int x, int y, SDL_Color c) {
    if (!f || !ren || !*s) return;
    SDL_Surface *su = TTF_RenderUTF8_Blended(f, s, c);
    if (!su) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, su);
    SDL_Rect r = { x, y, su->w, su->h };
    if (t) { SDL_RenderCopy(ren, t, NULL, &r); SDL_DestroyTexture(t); }
    SDL_FreeSurface(su);
}

static const SDL_Color WHITE = { 255, 255, 255, 255 }, GREY = { 170, 170, 170, 255 },
                       GREEN = { 90, 220, 120, 255 }, YELLOW = { 240, 200, 60, 255 }, RED = { 240, 90, 80, 255 };

/* schermata di avanzamento: titolo, fino a 3 righe */
static void screen(const char *title, const char *l1, const char *l2, const char *l3) {
    if (!ren) return;
    SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
    SDL_RenderClear(ren);
    int y = sh / 6;
    text(font_big, title, sw / 12, y, WHITE);
    y += sh / 7;
    const char *l[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++) if (l[i]) { text(font, l[i], sw / 12, y, i ? GREY : WHITE); y += sh / 11; }
    static int shots;
    if (l2 && shots++ == 3) shot("avanzamento");
    SDL_RenderPresent(ren);
}

/* VTEST_SHOT=prefisso: foto dello schermo in BMP, per controllare la grafica sul Mac */
static void shot(const char *tag) {
    const char *p = getenv("VTEST_SHOT");
    if (!p || !ren) return;
    SDL_Surface *su = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!su) return;
    char f[1100];
    snprintf(f, sizeof f, "%s-%s.bmp", p, tag);
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, su->pixels, su->pitch) == 0) SDL_SaveBMP(su, f);
    SDL_FreeSurface(su);
}

static const char *verdict(double fps, double src, SDL_Color *c) {
    if (src <= 0) { if (c) *c = GREY; return "?"; }
    if (fps >= src * 1.2) { if (c) *c = GREEN; return "fluido"; }
    if (fps >= src) { if (c) *c = YELLOW; return "al limite"; }
    if (c) *c = RED;
    return "lento";
}

/* un giro su un file; mode 0 decodifica, 1 visione. 0 = finito, 1 = fermato da --max, -1 errore */
static int run(const char *path, result_t *r, int mode, double maxwall, int idx, int total) {
    AVFormatContext *fc = NULL;
    AVCodecContext *vc = NULL, *ac = NULL;
    struct SwsContext *sws = NULL;
    SwrContext *swr = NULL;
    SDL_Texture *tex = NULL, *osd = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    uint8_t *yuv[4] = { 0 }, *abuf = NULL;
    int ylin[4] = { 0 }, abuf_n = 0;
    int ret = -1, vi = -1, ai = -1;

    if (avformat_open_input(&fc, path, NULL, NULL) < 0) { scrivi("  non si apre"); goto out; }
    if (avformat_find_stream_info(fc, NULL) < 0) { scrivi("  stream illeggibili"); goto out; }
    const AVCodec *vcod = NULL, *acod = NULL;
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &vcod, 0);
    if (vi < 0 || !vcod) { scrivi("  nessun video decodificabile"); goto out; }
    ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi, &acod, 0);
    AVStream *vs = fc->streams[vi];
    vc = avcodec_alloc_context3(vcod);
    avcodec_parameters_to_context(vc, vs->codecpar);
    vc->thread_count = 4;
    vc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(vc, vcod, NULL) < 0) { scrivi("  decoder video non parte"); goto out; }
    if (ai >= 0 && acod) {
        ac = avcodec_alloc_context3(acod);
        avcodec_parameters_to_context(ac, fc->streams[ai]->codecpar);
        if (avcodec_open2(ac, acod, NULL) < 0) avcodec_free_context(&ac);
        if (ac) {
            AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr, &st, AV_SAMPLE_FMT_S16, 48000, &ac->ch_layout, ac->sample_fmt,
                                    ac->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) swr_free(&swr);
        }
    }
    snprintf(r->codec, sizeof r->codec, "%s", vcod->name);
    snprintf(r->acodec, sizeof r->acodec, "%s", ac ? ac->codec->name : "-");
    snprintf(r->pixfmt, sizeof r->pixfmt, "%s", av_get_pix_fmt_name(vc->pix_fmt) ? av_get_pix_fmt_name(vc->pix_fmt) : "?");
    r->w = vc->width;
    r->h = vc->height;
    r->src_fps = av_q2d(vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate);
    r->kbps = (int)(fc->bit_rate / 1000);

    SDL_Rect dst = { 0, 0, sw, sh };
    if (mode == 1 && ren) {
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, r->w, r->h);
        if (!tex) { scrivi("  texture IYUV: %s", SDL_GetError()); goto out; }
        double s = fmin((double)sw / r->w, (double)sh / r->h);
        dst.w = (int)(r->w * s);
        dst.h = (int)(r->h * s);
        dst.x = (sw - dst.w) / 2;
        dst.y = (sh - dst.h) / 2;
    }

    char head[160];
    snprintf(head, sizeof head, "%d/%d  %s %dx%d %.0f fps", idx, total, r->codec, r->w, r->h, r->src_fps);
    const char *mname = mode ? "visione" : "decodifica";
    double t0 = now(), last_osd = -1;
    int frames = 0, eof = 0, stopped = 0;
    while (!stop) {
        if (!eof) {
            int e = av_read_frame(fc, pkt);
            if (e < 0) {
                eof = 1;
                avcodec_send_packet(vc, NULL);
                if (ac) avcodec_send_packet(ac, NULL);
            } else if (pkt->stream_index == vi) {
                avcodec_send_packet(vc, pkt);
            } else if (ac && pkt->stream_index == ai) {
                if (avcodec_send_packet(ac, pkt) >= 0)
                    while (avcodec_receive_frame(ac, fr) >= 0) {
                        if (swr) {
                            int n = swr_get_out_samples(swr, fr->nb_samples);
                            if (n * 4 > abuf_n) { av_free(abuf); abuf_n = n * 4; abuf = av_malloc(abuf_n); }
                            uint8_t *o[1] = { abuf };
                            swr_convert(swr, o, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
                        }
                        av_frame_unref(fr);
                    }
            }
            av_packet_unref(pkt);
        }
        int got_any = 0;
        for (;;) {
            int e = avcodec_receive_frame(vc, fr);
            if (e == AVERROR_EOF) goto finished;
            if (e < 0) break;
            got_any = 1;
            frames++;
            if (mode == 1 && tex) {
                if (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_YUVJ420P) {
                    SDL_UpdateYUVTexture(tex, NULL, fr->data[0], fr->linesize[0], fr->data[1], fr->linesize[1],
                                         fr->data[2], fr->linesize[2]);
                } else {
                    if (!sws) {
                        sws = sws_getContext(fr->width, fr->height, fr->format, r->w, r->h, AV_PIX_FMT_YUV420P,
                                             SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        av_image_alloc(yuv, ylin, r->w, r->h, AV_PIX_FMT_YUV420P, 32);
                    }
                    if (sws) {
                        sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, yuv, ylin);
                        SDL_UpdateYUVTexture(tex, NULL, yuv[0], ylin[0], yuv[1], ylin[1], yuv[2], ylin[2]);
                    }
                }
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, tex, NULL, &dst);
                double t = now() - t0;
                if (t - last_osd >= 1.0 || !osd) {   /* la scritta si rifà una volta al secondo */
                    last_osd = t;
                    if (osd) SDL_DestroyTexture(osd);
                    osd = NULL;
                    char s[200];
                    snprintf(s, sizeof s, "%s  visione  %.1f fps", head, t > 0 ? frames / t : 0);
                    SDL_Surface *su = font ? TTF_RenderUTF8_Shaded(font, s, WHITE, (SDL_Color){ 0, 0, 0, 255 }) : NULL;
                    if (su) { osd = SDL_CreateTextureFromSurface(ren, su); SDL_FreeSurface(su); }
                }
                if (osd) {
                    int w, h;
                    SDL_QueryTexture(osd, NULL, NULL, &w, &h);
                    SDL_Rect o = { 8, 8, w, h };
                    SDL_RenderCopy(ren, osd, NULL, &o);
                }
                if (frames == 60 && idx == 1) shot("visione");
                SDL_RenderPresent(ren);
            }
            av_frame_unref(fr);
        }
        double t = now() - t0;
        if (mode == 0 && t - last_osd >= 1.0) {
            last_osd = t;
            char s1[200], s2[120];
            snprintf(s1, sizeof s1, "%s", head);
            snprintf(s2, sizeof s2, "decodifica: %d fotogrammi, %.1f fps", frames, t > 0 ? frames / t : 0);
            screen("Prova video", s1, s2, r->name);
        }
        if (mode == 1 || t - last_osd < 0.01) pump();
        if (t > maxwall) { stopped = 1; break; }
        if (eof && !got_any) {
            /* a fine file il decoder a thread può restituire EAGAIN ancora un po' */
            static int spins;
            if (++spins > 1000) { spins = 0; break; }
        }
    }
finished:;
    double secs = now() - t0;
    r->frames[mode] = frames;
    r->fps[mode] = secs > 0 ? frames / secs : 0;
    r->speed[mode] = r->src_fps > 0 ? r->fps[mode] / r->src_fps : 0;
    r->done[mode] = 1;
    scrivi("  %-10s %5d fotogrammi in %5.1f s = %6.1f fps (%.2fx tempo reale)%s  cpu %d MHz, %d °C",
         mname, frames, secs, r->fps[mode], r->speed[mode], stop ? " INTERROTTO" : stopped ? " (fermato a --max)" : "",
         cpu_mhz(), temp_c());
    ret = stopped ? 1 : 0;
out:
    if (osd) SDL_DestroyTexture(osd);
    if (tex) SDL_DestroyTexture(tex);
    if (yuv[0]) av_freep(&yuv[0]);
    av_free(abuf);
    sws_freeContext(sws);
    swr_free(&swr);
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_close_input(&fc);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    return ret;
}

static int is_video(const char *n) {
    const char *e = strrchr(n, '.');
    if (!e || n[0] == '.') return 0;
    return !strcasecmp(e, ".mp4") || !strcasecmp(e, ".mkv") || !strcasecmp(e, ".webm") ||
           !strcasecmp(e, ".avi") || !strcasecmp(e, ".mov") || !strcasecmp(e, ".m4v");
}

static int cmp(const void *a, const void *b) { return strcmp(((const result_t *)a)->name, ((const result_t *)b)->name); }

int main(int argc, char **argv) {
    const char *root = getenv("ROOT");
    char dir[1024] = ".", logp[1100], fontp[1100];
    double maxwall = 25;
    int only_decode = 0;
    snprintf(logp, sizeof logp, "%s/Logs/video-test.txt", root ? root : "/mnt/SDCARD/Arcade");
    snprintf(fontp, sizeof fontp, "%s/Themes/_shared/fonts/Nunito.ttf", root ? root : "/mnt/SDCARD/Arcade");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) snprintf(logp, sizeof logp, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) snprintf(fontp, sizeof fontp, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxwall = atof(argv[++i]);
        else if (!strcmp(argv[i], "--solo-decodifica")) only_decode = 1;
    }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    logfile = fopen(logp, "a");
    if (!logfile) fprintf(stderr, "vtest: non posso scrivere %s, solo sullo schermo\n", logp);

    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nres < MAXF)
            if (is_video(e->d_name)) snprintf(res[nres++].name, sizeof res[0].name, "%s", e->d_name);
        closedir(d);
    }
    qsort(res, nres, sizeof res[0], cmp);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0) { sw = dm.w; sh = dm.h; }
#ifdef __APPLE__
    sw = getenv("VTEST_W") ? atoi(getenv("VTEST_W")) : 1280;
    sh = getenv("VTEST_H") ? atoi(getenv("VTEST_H")) : 720;
    win = SDL_CreateWindow("vtest", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh, 0);
#else
    win = SDL_CreateWindow("vtest", 0, 0, sw, sh, SDL_WINDOW_FULLSCREEN);
#endif
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");   /* si misura il lavoro, non l'attesa dello schermo */
    if (win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (ren) SDL_GetRendererOutputSize(ren, &sw, &sh);
    if (TTF_Init() == 0) {
        font = TTF_OpenFont(fontp, sh / 26 > 12 ? sh / 26 : 12);
        font_big = TTF_OpenFont(fontp, sh / 14 > 16 ? sh / 14 : 16);
    }

    /* intestazione: abbastanza per confrontare prove diverse */
    time_t tt = time(NULL);
    char when[64], model[128], gov[64], maxf[64], minf[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&tt));
    read_line("/proc/device-tree/model", model, sizeof model);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof gov);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", maxf, sizeof maxf);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", minf, sizeof minf);
    struct utsname u;
    uname(&u);
    SDL_RendererInfo ri = { 0 };
    if (ren) SDL_GetRendererInfo(ren, &ri);
    SDL_version sv;
    SDL_GetVersion(&sv);
    scrivi("==== vtest %s", when);
    scrivi("macchina: %s (%s %s), processori: %ld", model[0] ? model : "?", u.sysname, u.machine, sysconf(_SC_NPROCESSORS_ONLN));
    scrivi("cpufreq: %s, %d-%d MHz, ora %d MHz, %d °C", gov[0] ? gov : "?", minf[0] ? atoi(minf) / 1000 : 0,
         maxf[0] ? atoi(maxf) / 1000 : 0, cpu_mhz(), temp_c());
    scrivi("schermo %dx%d, renderer %s, SDL %d.%d.%d, FFmpeg %s, --max %.0f s", sw, sh, ri.name ? ri.name : "nessuno",
         sv.major, sv.minor, sv.patch, av_version_info(), maxwall);
    scrivi("cartella %s: %d video", dir, nres);
    if (!nres) screen("Prova video", "Nessun video nella cartella", dir, NULL);

    for (int i = 0; i < nres && !stop; i++) {
        char path[1300];
        snprintf(path, sizeof path, "%.1024s/%.255s", dir, res[i].name);
        scrivi("%s", res[i].name);
        if (run(path, &res[i], 0, maxwall, i + 1, nres) < 0) continue;
        scrivi("  %s %dx%d %.2f fps %s, %d kbit/s, audio %s", res[i].codec, res[i].w, res[i].h, res[i].src_fps,
             res[i].pixfmt, res[i].kbps, res[i].acodec);
        if (!only_decode && !stop) run(path, &res[i], 1, maxwall, i + 1, nres);
    }

    /* riepilogo: il giudizio viene dalla visione (il lavoro vero di un lettore) */
    scrivi("riepilogo (fluido = almeno il 20%% sopra i fps del video, al limite = appena sopra):");
    for (int i = 0; i < nres; i++) {
        if (!res[i].done[0]) continue;
        int m = res[i].done[1] ? 1 : 0;
        scrivi("  %-34s %-5s %4dx%-4d %5.1f fps video | decodifica %6.1f | visione %6.1f | %s", res[i].name, res[i].codec,
             res[i].w, res[i].h, res[i].src_fps, res[i].fps[0], res[i].done[1] ? res[i].fps[1] : 0,
             verdict(res[i].fps[m], res[i].src_fps, NULL));
    }
    scrivi("%s", stop ? "==== interrotta" : "==== finita");

    if (ren && !getenv("VTEST_NO_WAIT")) {
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        int y = sh / 20, lh = TTF_FontLineSkip(font ? font : font_big) + 2;
        text(font_big, stop ? "Prova interrotta" : "Prova finita", sw / 20, y, WHITE);
        y += sh / 9;
        for (int i = 0; i < nres && y < sh - lh; i++) {
            if (!res[i].done[0]) continue;
            SDL_Color c;
            int m = res[i].done[1] ? 1 : 0;
            const char *v = verdict(res[i].fps[m], res[i].src_fps, &c);
            char s[200];
            snprintf(s, sizeof s, "%s %dx%d %.0f fps: %.0f fps, %s", res[i].codec, res[i].w, res[i].h, res[i].src_fps,
                     res[i].fps[m], v);
            text(font, s, sw / 20, y, c);
            y += lh;
        }
        shot("riepilogo");
        SDL_RenderPresent(ren);
        double t0 = now();
        while (now() - t0 < 15 && !stop) { pump(); SDL_Delay(50); }
    }
    if (logfile) fclose(logfile);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
