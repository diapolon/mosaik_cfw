/* Spazio della SD: app di Mosaik, pacchetto dello Store (Arcade/Apps/sdgrow).

   Mostra quanto è grande la scheda e quanto spazio usa Mosaik. Al primo avvio l'initramfs
   allarga la partizione a tutta la scheda (m2/sdgrow.c); se non ci è riuscito (issue #7) qui si
   vede perché, e con A si allarga. Il FAT è montato e non si tocca: si allarga solo la tabella
   delle partizioni (sdgrow -g) e il FAT lo allarga l'initramfs alla prossima accensione, anche
   quello delle schede scritte prima (stesso sdgrow, da sempre nell'initramfs). Per questo l'app
   chiede di spegnere: esce con 120, che system-init tratta come uno spegnimento (anche col
   caricatore collegato, issue #2).

   sdgrow è compilato dentro l'app (m2/sdgrow.c con main rinominato) e gira in un processo figlio,
   perché si ferma con exit() al primo errore. Così l'app funziona su ogni sistema già installato.

   Sul Mac: build/sdgrow --root sdcard --app package/apps/sdgrow (scheda simulata). */
#include "appkit.h"
#include "../common/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#define main sdgrow_main
#include "../../../m2/sdgrow.c"
#undef main
#endif

/* ------------------------------------------------------------ scheda */

static char part[64], disk[64];          /* mmcblk0p4, mmcblk0 */
static long long card_bytes, part_bytes, fs_bytes, fs_free;
static int verdict = -1;                  /* esito della prova: 0 si può, 1 niente da fare, 2 no */
static char detail[600];                  /* quello che dice sdgrow */
static bool grown;                        /* tabella allargata: si spegne per finire */

static long long sys_sectors(const char *name) {
    char p[256];
    snprintf(p, sizeof p, "/sys/class/block/%s/size", name);
    FILE *f = fopen(p, "r");
    long long v = 0;
    if (f) { if (fscanf(f, "%lld", &v) != 1) v = 0; fclose(f); }
    return v * 512;
}

/* il dispositivo montato dove sta la cartella Arcade, e il suo disco */
static void find_card(void) {
    FILE *f = fopen("/proc/mounts", "r");
    char dev[256], mnt[512], best[256] = "";
    size_t bl = 0;
    while (f && fscanf(f, "%255s %511s %*s %*s %*d %*d", dev, mnt) == 2) {
        size_t l = strlen(mnt);
        if (strncmp(dev, "/dev/", 5) || strncmp(app_root, mnt, l) || (app_root[l] && app_root[l] != '/' && l > 1)) continue;
        if (l >= bl) { bl = l; snprintf(best, sizeof best, "%s", dev + 5); }
    }
    if (f) fclose(f);
    snprintf(part, sizeof part, "%s", best);
    char p[300], link[512];
    snprintf(p, sizeof p, "/sys/class/block/%s", part);
    ssize_t n = part[0] ? readlink(p, link, sizeof link - 1) : -1;
    disk[0] = 0;
    if (n > 0) {   /* .../mmcblk0/mmcblk0p4: la cartella sopra è il disco */
        link[n] = 0;
        char *s = strrchr(link, '/');
        if (s) { *s = 0; s = strrchr(link, '/'); if (s) snprintf(disk, sizeof disk, "%s", s + 1); }
    }
    card_bytes = disk[0] ? sys_sectors(disk) : 0;
    part_bytes = part[0] ? sys_sectors(part) : 0;
    struct statvfs v;
    if (statvfs(app_root, &v) == 0) { fs_bytes = (long long)v.f_blocks * v.f_frsize; fs_free = (long long)v.f_bavail * v.f_frsize; }
    ui_log("sd: partizione %s, disco %s, scheda %lld MB, partizione %lld MB, FAT %lld MB", part, disk, card_bytes >> 20, part_bytes >> 20, fs_bytes >> 20);
}

/* sdgrow in un processo figlio: uscita e testo */
static int run_sdgrow(const char *opt, char *out, size_t n) {
    out[0] = 0;
#ifdef __linux__
    if (!disk[0]) { snprintf(out, n, "sdgrow: scheda non trovata"); return 2; }
    int pf[2];
    if (pipe(pf) != 0) return 2;
    pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return 2; }
    if (pid == 0) {
        close(pf[0]);
        dup2(pf[1], 1);
        dup2(pf[1], 2);
        char dev[96];
        snprintf(dev, sizeof dev, "/dev/%s", disk);
        char *argv[] = { "sdgrow", (char *)opt, dev, NULL };
        int rc = sdgrow_main(3, argv);
        fflush(stdout);
        _exit(rc);
    }
    close(pf[1]);
    size_t got = 0;
    ssize_t r;
    while (got + 1 < n && (r = read(pf[0], out + got, n - 1 - got)) > 0) got += (size_t)r;
    out[got] = 0;
    close(pf[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
#else   /* Mac: una scheda da 64 GB rimasta a 2 GB */
    int rc = !strcmp(opt, "-n") ? 0 : 3;
    snprintf(out, n, "partizione ARCADE: 2132 -> 60906 MB\nFAT: 2132 -> 60906 MB%s", rc == 3 ? " al prossimo avvio" : "");
    card_bytes = 61865984LL * 1024;
    part_bytes = 2132LL << 20;
#endif
    for (char *c = out + strlen(out); c > out && (c[-1] == '\n' || c[-1] == ' '); ) *--c = 0;
    ui_log("sd: sdgrow %s -> %d: %s", opt, rc, out);
    for (char *c = out; *c; c++) if (*c == '\n') *c = ';';   /* una riga sola sullo schermo */
    return rc;
}

static void check(void) {
    find_card();
    verdict = run_sdgrow("-n", detail, sizeof detail);
    if (verdict > 2) verdict = 2;
}

/* ------------------------------------------------------------ disegno */

static bool ask_open, quit;
static int ask_sel, exit_code;

static void size_line(const char *key, long long a, long long b, char *out, size_t n) {
    char x[32], y[32];
    app_size(a, x, sizeof x);
    app_size(b, y, sizeof y);
    snprintf(out, n, FT(key), x, y);
}

static void draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small, *ft = UI.f_title;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    ui_topbar(FT("sdgrow.title"));
    int y = M(t->topbar_h) + M(0.03f);
    char line[400], a[32];
    app_size(card_bytes, a, sizeof a);
    snprintf(line, sizeof line, FT("sdgrow.card"), card_bytes > 0 ? a : "?");
    ui_text(fn, line, mx, y, t->text, ALIGN_LEFT);
    y += fnh + M(0.012f);
    app_size(part_bytes, a, sizeof a);
    snprintf(line, sizeof line, FT("sdgrow.part"), part_bytes > 0 ? a : "?");
    ui_text(fn, line, mx, y, t->text, ALIGN_LEFT);
    y += fnh + M(0.012f);
    size_line("sdgrow.free", fs_free, fs_bytes, line, sizeof line);
    ui_text(fn, line, mx, y, t->text_dim, ALIGN_LEFT);
    y += fnh + M(0.035f);
    /* esito, in un riquadro */
    const char *vk = grown ? "sdgrow.done" : verdict == 0 ? "sdgrow.can" : verdict == 1 ? "sdgrow.full" : "sdgrow.cannot";
    char lines[6][160], dl[8][160];
    int n = ui_wrap(fn, FT(vk), W - M(0.05f), lines, 6);
    int nd = ui_wrap(fs, detail, W - M(0.05f), dl, 8);
    int bh = M(0.03f) + TTF_FontHeight(ft) + n * fnh + M(0.015f) + nd * fsh + M(0.03f);
    color_t box = grown || verdict == 1 ? with_alpha(t->accent, 40) : verdict == 0 ? with_alpha(t->accent, 70) : rgba(230, 130, 20, 60);
    ui_rrect(mx, y, W, bh, M(t->panel_radius), box);
    int ty = y + M(0.03f);
    ui_text(ft, FT(grown ? "sdgrow.done_title" : verdict == 0 ? "sdgrow.can_title" : verdict == 1 ? "sdgrow.full_title" : "sdgrow.cannot_title"),
            mx + M(0.025f), ty, t->text, ALIGN_LEFT);
    ty += TTF_FontHeight(ft);
    for (int i = 0; i < n; i++, ty += fnh) ui_text(fn, lines[i], mx + M(0.025f), ty, t->text, ALIGN_LEFT);
    ty += M(0.015f);
    for (int i = 0; i < nd; i++, ty += fsh) ui_text(fs, dl[i], mx + M(0.025f), ty, t->text_dim, ALIGN_LEFT);
    if (grown) {
        const char *h[] = { "B", FT("sdgrow.quit"), NULL, "A", FT("sdgrow.power_off"), NULL };
        ui_keybar(h, 6);
    } else if (verdict == 0) {
        const char *h[] = { "B", FT("sdgrow.quit"), NULL, "A", FT("sdgrow.grow"), NULL };
        ui_keybar(h, 6);
    } else {
        const char *h[] = { "B", FT("sdgrow.quit"), NULL };
        ui_keybar(h, 3);
    }
    if (ask_open) ui_confirm_draw(FT("sdgrow.ask_title"), FT("sdgrow.ask_body"), T("common.cancel"), FT("sdgrow.grow"), ask_sel);
}

static void input(void) {
    input_t *in = &UI.in;
    if (ask_open) {
        int r = ui_confirm_input(&ask_sel);
        if (r < 0) return;
        ask_open = false;
        if (r != 1) return;
        int rc = run_sdgrow("-g", detail, sizeof detail);
        if (rc == 3 || rc == 0) { grown = true; ui_sound(SND_OK); }
        else { check(); ui_toast(FT("sdgrow.failed")); ui_sound(SND_ERROR); }
        return;
    }
    if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) { quit = true; return; }
    if (!in->pressed[BTN_A]) return;
    if (grown) { exit_code = 120; quit = true; ui_sound(SND_OK); }   /* system-init spegne */
    else if (verdict == 0) { ask_open = true; ask_sel = 0; ui_sound(SND_OK); }
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "sdgrow")) return 1;
    check();
    while (!quit) {
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        input();
        ui_begin();
        draw();
        ui_end();
    }
    app_exit_begin();
    app_quit();
    return exit_code;
}
