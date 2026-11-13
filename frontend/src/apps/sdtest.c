/* Test della SD: app di Mosaik, pacchetto dello Store (Arcade/Apps/sdtest).

   Prova veloce: scrive 256 MB e li rilegge, per la velocità. Prova completa: riempie lo spazio
   libero (ne lascia sempre 256 MB al sistema) con file da 1 GB, li rilegge e controlla ogni
   blocco: una scheda falsa, che dichiara più spazio di quanto ne ha, o una scheda guasta danno
   dati diversi da quelli scritti. I dati sono una sequenza pseudocasuale che dipende solo dalla
   posizione, così per controllarli non serve tenerli in memoria. Si scrive in Arcade/.sdtest,
   che alla fine (anche interrompendo con B) si toglie; prima di rileggere si svuota la cache del
   sistema, altrimenti la lettura misurerebbe la memoria e non la scheda. L'ultimo risultato
   resta in last.txt accanto al programma.

   Sul Mac: build/sdtest --root sdcard --app package/apps/sdtest [--auto "..."] */
#include "appkit.h"
#include "../common/platform.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define BLOCK (4u << 20)                  /* 4 MB per scrittura e controllo */
#define FILE_BYTES (1024LL << 20)         /* file da 1 GB (il FAT arriva a 4) */
#define QUICK_BYTES (256LL << 20)
#define RESERVE (256LL << 20)             /* spazio lasciato al sistema nella prova completa */

enum { PH_IDLE, PH_WRITE, PH_READ, PH_DONE };
static struct {
    _Atomic int phase;
    _Atomic bool cancel, full;
    _Atomic long long done, total;      /* byte della fase in corso */
    _Atomic long long bad;              /* blocchi diversi da quelli scritti */
    _Atomic long long first_bad;        /* primo byte rovinato, -1 nessuno */
    _Atomic long long read_err;         /* blocchi che non si sono potuti leggere */
    bool cached;                        /* la cache non si è svuotata: lettura non dalla scheda */
    double t0, write_s, read_s;         /* tempi (s) */
    long long tested;                   /* byte scritti e riletti */
    bool error;                         /* errore di scrittura o lettura */
    bool no_space;                      /* lo spazio non basta più (o non si legge) al momento di partire */
    bool clean_failed;                  /* file di prova rimasti nella cartella .sdtest */
    int first_errno;                    /* primo errore del sistema, per il log */
    char msg[160];
} job;
static pthread_t thr;
/* fase, inizio e totale cambiano insieme: la finestra li legge sotto lo stesso mutex (V26-04) */
static pthread_mutex_t jl = PTHREAD_MUTEX_INITIALIZER;
static double now_s(void);
static void phase_set(int ph, long long total) {
    pthread_mutex_lock(&jl);
    job.total = total;
    job.done = 0;
    job.t0 = now_s();
    job.phase = ph;
    pthread_mutex_unlock(&jl);
}
static bool thr_on, quit;
static int sel;
static bool confirm_open;   /* prova completa: avviso e conferma */
static int confirm_sel;
static long long card_total, card_free;
static char last[300];

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* falso se lo spazio non si legge: allora vale -1 e nessuna prova parte (verifica audit APP-009) */
static bool space(void) {
    struct statvfs v;
    if (statvfs(app_root, &v) != 0) { card_free = -1; return false; }
    card_total = (long long)v.f_blocks * (long long)v.f_frsize;
    card_free = (long long)v.f_bavail * (long long)v.f_frsize;
    return true;
}
static void note_errno(void) { if (!job.first_errno) job.first_errno = errno ? errno : EIO; }

/* dati del blocco n: xorshift64 con il seme dal numero del blocco */
static void fill(uint64_t *b, long long n) {
    uint64_t x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)n * 0xD1B54A32D192ED03ULL);
    for (size_t i = 0; i < BLOCK / 8; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        b[i] = x ^ (uint64_t)i;
    }
}

static void test_dir(char *out, size_t n) { snprintf(out, n, "%s/.sdtest", app_root); }

/* toglie solo i propri file (NNNN.bin, file normali) dalla cartella della prova */
static bool cleanup(void) {
    char d[1100], p[1300];
    test_dir(d, sizeof d);
    DIR *dd = opendir(d);
    struct dirent *e;
    bool ok = true;
    while (dd && (e = readdir(dd))) {
        const char *n = e->d_name;
        if (strlen(n) != 8 || strcmp(n + 4, ".bin") || strspn(n, "0123456789") != 4) continue;
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", d, n);
        if (lstat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
        if (unlink(p) != 0) ok = false;
    }
    if (dd) closedir(dd);
    rmdir(d);
    sync();
    if (!ok) ui_log("sd: file di prova non tolti in %s", d);
    return ok;
}

/* la cache dei file si svuota (serve root, sulla console c'è): la lettura arriva dalla scheda */
static bool drop_caches(void) {
    sync();
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (!f) return false;
    bool ok = fputs("3\n", f) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static void *worker(void *arg) {
    (void)arg;
    uint64_t *buf = malloc(BLOCK), *rd = malloc(BLOCK);
    char d[1100], p[1300];
    test_dir(d, sizeof d);
    cleanup();
    /* lo spazio si rilegge qui: fra la conferma e l'inizio può essere cambiato */
    bool sp = space();
    long long want = job.full ? card_free - RESERVE : QUICK_BYTES;
    want -= want % BLOCK;
    if (!sp || want < (long long)BLOCK || card_free - want < RESERVE) { job.no_space = true; goto end; }
    if (!buf || !rd) { job.error = true; goto end; }
    mkdir(d, 0755);
    /* scrittura (i dati della fase prima della fase stessa: la finestra li legge) */
    phase_set(PH_WRITE, want);
    long long blk = 0;
    for (int fi = 0; job.done < want && !job.cancel; fi++) {
        snprintf(p, sizeof p, "%s/%04d.bin", d, fi);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { note_errno(); job.error = true; break; }
        long long in_file = 0;
        while (in_file < FILE_BYTES && job.done < want && !job.cancel) {
            fill(buf, blk++);
            ssize_t w = write(fd, buf, BLOCK);
            if (w != (ssize_t)BLOCK) { note_errno(); job.error = true; break; }
            in_file += BLOCK;
            job.done += BLOCK;
        }
        if (fsync(fd) != 0) { note_errno(); job.error = true; }
        close(fd);
        if (job.error) break;
    }
    job.write_s = now_s() - job.t0;
    long long written = job.done;
    if (job.cancel || job.error) goto end;
    /* lettura e controllo */
    job.cached = !drop_caches();   /* sul Mac, o senza i permessi: si legge dalla memoria */
    phase_set(PH_READ, written);
    blk = 0;
    for (int fi = 0; job.done < written && !job.cancel; fi++) {
        snprintf(p, sizeof p, "%s/%04d.bin", d, fi);
        int fd = open(p, O_RDONLY);
        if (fd < 0) { note_errno(); job.error = true; break; }
        long long in_file = 0;
        while (in_file < FILE_BYTES && job.done < written && !job.cancel) {
            ssize_t r = 0;
            for (ssize_t got; r < (ssize_t)BLOCK; r += got)
                if ((got = read(fd, (char *)rd + r, BLOCK - (size_t)r)) <= 0) { if (got < 0) note_errno(); break; }
            fill(buf, blk++);
            if (r != (ssize_t)BLOCK) job.read_err++;   /* contato a parte: non letto non è "rovinato" */
            if (r != (ssize_t)BLOCK || memcmp(buf, rd, BLOCK)) {
                if (job.first_bad < 0) job.first_bad = job.done;
                job.bad++;
            }
            in_file += BLOCK;
            job.done += BLOCK;
        }
        close(fd);
    }
    job.read_s = now_s() - job.t0;
    job.tested = job.cancel ? 0 : written;
end:
    free(buf);
    free(rd);
    job.clean_failed = !cleanup();
    space();
    job.phase = PH_DONE;
    return NULL;
}

static void start(bool full) {
    memset((void *)&job, 0, sizeof job);
    job.full = full;
    job.first_bad = -1;
    job.t0 = now_s();      /* prima di creare il lavoro: nessuno lo legge ancora */
    job.phase = PH_WRITE;
    ui_log("sd: prova %s, libero %lld MB", full ? "completa" : "veloce", card_free >> 20);
    thr_on = pthread_create(&thr, NULL, worker, NULL) == 0;
    if (!thr_on) job.phase = PH_IDLE;
}

/* ------------------------------------------------------------ risultato */

static double mbs(long long b, double s) { return s > 0 ? (double)b / (1 << 20) / s : 0; }

static void result_text(char *out, size_t n) {
    char sz[32];
    app_size(job.tested, sz, sizeof sz);
    double w = mbs(job.tested, job.write_s), r = mbs(job.tested, job.read_s);
    /* lettura dalla cache: la velocità di lettura non è della scheda, niente giudizio (APP-012) */
    const char *verdict = job.read_err ? FT("sdtest.read_errors") : job.bad ? FT("sdtest.bad") : job.cached ? FT("sdtest.cached")
                        : w >= 30 && r >= 70 ? FT("sdtest.great") : w >= 12 && r >= 40 ? FT("sdtest.good") : FT("sdtest.slow");
    snprintf(out, n, FT("sdtest.result"), sz, w, r, verdict);
}

static void last_save(const char *s) {
    char p[1100];
    snprintf(p, sizeof p, "%s/last.txt", app_dir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static void last_load(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/last.txt", app_dir);
    FILE *f = fopen(p, "r");
    if (f) { if (!fgets(last, sizeof last, f)) last[0] = 0; fclose(f); }
    last[strcspn(last, "\r\n")] = 0;
}

/* ------------------------------------------------------------ disegno */

static void hms(double s, char *out, size_t n) {
    int t = (int)s;
    if (t >= 3600) snprintf(out, n, "%d h %02d min", t / 3600, t / 60 % 60);
    else snprintf(out, n, "%d min %02d s", t / 60, t % 60);
}

static void draw_idle(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small, *ft = UI.f_title;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs), fth = TTF_FontHeight(ft);
    int y = M(t->topbar_h) + M(0.025f);
    char a[32], b[32], line[300];
    app_size(card_total, a, sizeof a);
    app_size(card_free, b, sizeof b);
    snprintf(line, sizeof line, FT("sdtest.card"), a, b);
    ui_text(fn, line, mx, y, t->accent, ALIGN_LEFT);
    y += fnh + M(0.03f);
    static const char *k[2][2] = { { "sdtest.quick", "sdtest.quick_desc" }, { "sdtest.full", "sdtest.full_desc" } };
    int bh = fth + 2 * fsh + M(0.04f);
    for (int i = 0; i < 2; i++, y += bh + M(0.02f)) {
        bool on = i == sel;
        ui_rrect(mx, y, W, bh, M(t->panel_radius), on ? t->accent : with_alpha(t->text_dim, 35));
        ui_text(ft, FT(k[i][0]), mx + M(0.025f), y + M(0.015f), on ? t->accent_text : t->text, ALIGN_LEFT);
        char lines[2][160];
        int n = ui_wrap(fs, FT(k[i][1]), W - M(0.05f), lines, 2);
        for (int l = 0; l < n; l++) ui_text(fs, lines[l], mx + M(0.025f), y + M(0.015f) + fth + l * fsh, on ? t->accent_text : t->text_dim, ALIGN_LEFT);
    }
    if (last[0]) {   /* l'ultima prova */
        y += M(0.01f);
        ui_text(fs, FT("sdtest.last"), mx, y, t->text_dim, ALIGN_LEFT);
        char lines[3][160];
        int n = ui_wrap(fn, last, W, lines, 3);
        for (int l = 0; l < n; l++) ui_text(fn, lines[l], mx, y + fsh + l * fnh, t->text, ALIGN_LEFT);
    }
    const char *h[] = { "B", FT("sdtest.quit"), NULL, "A", FT("sdtest.start"), NULL };
    ui_keybar(h, 6);
}

static void draw_confirm(void) {
    theme_t *t = &UI.th;
    TTF_Font *fn = UI.f_normal, *ft = UI.f_title;
    int fnh = TTF_FontHeight(fn), w = M(0.86f) < UI.w - M(0.04f) ? M(0.86f) : UI.w - M(0.04f), pad = M(0.04f);
    char lines[8][160];
    int n = ui_wrap(fn, FT("sdtest.full_confirm"), w - 2 * pad, lines, 8);
    int h = pad + TTF_FontHeight(ft) + M(0.02f) + n * fnh + M(0.02f) + ui_popup_buttons_h(), x, y;
    app_panel(w, h, &x, &y);
    ui_text(ft, FT("sdtest.full"), x + pad, y + pad, t->text, ALIGN_LEFT);
    for (int k = 0; k < n; k++) ui_text(fn, lines[k], x + pad, y + pad + TTF_FontHeight(ft) + M(0.02f) + k * fnh, t->text, ALIGN_LEFT);
    const char *labels[2] = { T("common.cancel"), FT("sdtest.start") };
    ui_popup_buttons(x, y, w, h, labels, 2, confirm_sel, 1);
}

static void draw_run(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small, *ft = UI.f_title;
    int fsh = TTF_FontHeight(fs), fth = TTF_FontHeight(ft);
    int y = M(t->topbar_h) + M(0.06f);
    pthread_mutex_lock(&jl);
    int ph = job.phase;
    long long tot = job.total;
    double t0 = job.t0;
    pthread_mutex_unlock(&jl);
    ui_text(ft, FT(ph == PH_READ ? "sdtest.reading" : "sdtest.writing"), mx, y, t->text, ALIGN_LEFT);
    y += fth + M(0.03f);
    long long done = job.done;
    double el = now_s() - t0, sp = mbs(done, el);
    int bh = M(0.02f);
    ui_rrect(mx, y, W, bh, bh / 2, with_alpha(t->text_dim, 60));
    if (tot > 0) ui_rrect(mx, y, (int)((long long)W * (done < tot ? done : tot) / tot), bh, bh / 2, t->accent);
    y += bh + M(0.025f);
    char a[32], b[32], c[32], line[300];
    app_size(done, a, sizeof a);
    app_size(tot, b, sizeof b);
    snprintf(line, sizeof line, "%s / %s  ·  %.1f MB/s", a, b, sp);
    ui_text(fn, line, mx, y, t->text, ALIGN_LEFT);
    y += TTF_FontHeight(fn) + M(0.015f);
    if (sp > 0 && tot > done) {
        hms((double)(tot - done) / (1 << 20) / sp, c, sizeof c);
        snprintf(line, sizeof line, FT("sdtest.remaining"), c);
        ui_text(fs, line, mx, y, t->text_dim, ALIGN_LEFT);
    }
    y += fsh + M(0.03f);
    if (job.bad) {
        snprintf(line, sizeof line, FT("sdtest.bad_blocks"), (long long)job.bad);
        ui_text(fn, line, mx, y, rgba(220, 70, 70, 255), ALIGN_LEFT);
    }
    if (job.full) {
        char lines[2][160];
        int n = ui_wrap(fs, FT("sdtest.full_warn"), W, lines, 2);
        for (int l = 0; l < n; l++) ui_text(fs, lines[l], mx, UI.h - ui_keybar_h() - M(0.02f) - (n - l) * fsh, t->text_dim, ALIGN_LEFT);
    }
    const char *h[] = { "B", FT("sdtest.stop"), NULL };
    ui_keybar(h, 3);
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "sdtest")) return 1;
    if (!cleanup()) ui_toast(FT("sdtest.cleanup_failed"));   /* avanzi di una prova interrotta da uno spegnimento */
    space();
    last_load();
    /* Durante la prova l'audio tace del tutto, come nel launcher durante i lavori sulla scheda: con
       la SD al lavoro la Brick Pro fruscia (issue #4), e il clic di A teneva acceso l'altoparlante
       per i primi 4 s della scrittura. Si spegne appena finito il clic, si riaccende alla fine. */
    Uint32 mute_at = 0;
    bool muted = false;
    while (!quit) {
        if (mute_at && job.phase != PH_IDLE && job.phase != PH_DONE && SDL_GetTicks() >= mute_at) {
            mute_at = 0;
            muted = true;
            plat_audio_mute();
        }
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        input_t *in = &UI.in;
        int ph = job.phase;
        if (ph == PH_IDLE && confirm_open) {
            int r = ui_popup_input(&confirm_sel, 2);
            if (r == 1) { confirm_open = false; ui_sound(SND_OK); start(true); mute_at = SDL_GetTicks() + 300; }
            else if (r == 0 || r == -2) { confirm_open = false; ui_sound(SND_BACK); }
        } else if (ph == PH_IDLE) {
            if (in->pressed[BTN_UP] || in->pressed[BTN_DOWN]) { sel = !sel; ui_sound(SND_MOVE); }
            if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) quit = true;
            else if (in->pressed[BTN_A]) {
                bool charging;
                int bat = ui_battery(&charging);
                bool sp = space();
                if (!sp) { ui_toast(FT("sdtest.no_space")); ui_sound(SND_ERROR); }
                else if (sel == 1 && !charging && bat >= 0 && bat < 50) { ui_toast(FT("sdtest.charge")); ui_sound(SND_ERROR); }
                else if (card_free < (sel == 1 ? 1LL << 30 : QUICK_BYTES) + RESERVE) { ui_toast(FT("sdtest.no_space")); ui_sound(SND_ERROR); }
                else if (sel == 1) { confirm_open = true; confirm_sel = 0; ui_sound(SND_OK); }   /* prima l'avviso */
                else { ui_sound(SND_OK); start(false); mute_at = SDL_GetTicks() + 300; }
            }
        } else if (ph == PH_DONE) {
            if (thr_on) { pthread_join(thr, NULL); thr_on = false; }
            mute_at = 0;
            if (muted) { muted = false; plat_audio_unmute(); }   /* prima dei suoni della fine */
            if (job.cancel) { ui_toast(FT("sdtest.stopped")); snprintf(last, sizeof last, "%s", ""); last_load(); }
            else if (job.no_space) { ui_toast(FT("sdtest.no_space")); ui_sound(SND_ERROR); last_load(); }
            else if (job.error) { snprintf(last, sizeof last, "%s", FT("sdtest.error")); last_save(last); }
            else { result_text(last, sizeof last); last_save(last); }
            ui_log("sd: %s (rovinati %lld, primo %lld, non letti %lld, errore %d%s)", job.cancel ? "interrotta" : job.no_space ? "spazio" : last,
                   (long long)job.bad, (long long)job.first_bad, (long long)job.read_err, job.first_errno, job.clean_failed ? ", file di prova rimasti" : "");
            if (job.clean_failed) ui_toast(FT("sdtest.cleanup_failed"));
            job.phase = PH_IDLE;
        } else if (in->pressed[BTN_B]) { job.cancel = true; ui_sound(SND_BACK); }
        ui_begin();
        ui_topbar(FT("sdtest.title"));
        if (job.phase == PH_IDLE) { draw_idle(); if (confirm_open) draw_confirm(); } else draw_run();
        ui_end();
    }
    if (muted) plat_audio_unmute();   /* uscita a prova in corso (chiusura da gamewatch): l'audio torna */
    app_exit_begin();
    if (thr_on) { job.cancel = true; pthread_join(thr, NULL); }
    app_quit();
    return 0;
}
