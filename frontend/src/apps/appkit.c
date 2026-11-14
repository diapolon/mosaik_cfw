#include "appkit.h"
#include "../common/json.h"
#include "../common/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char app_root[1024], app_dir[1024];
static jval *texts, *texts_en;

/* "0.9.37" contro "0.9.5", numero per numero (come lo Store) */
static int vcmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) break;
    }
    return 0;
}

/* L'app dichiara in app.json il sistema di cui ha bisogno ("min_system"). Il launcher dalla 0.9.62
   non la avvia su un sistema più vecchio; quelli di prima sì, e allora lo dice l'app stessa invece
   di comportarsi male. La versione è in /etc/arcade-version (sulla console; ARCADE_SYSTEM_VERSION
   nelle prove). Falso se l'app non deve partire. */
static void system_version(char *ver, size_t n) {
    const char *env = getenv("ARCADE_SYSTEM_VERSION");
    ver[0] = 0;
    if (env) { snprintf(ver, n, "%s", env); return; }
    FILE *f = fopen("/etc/arcade-version", "r");
    if (f) { if (!fgets(ver, (int)n, f)) ver[0] = 0; fclose(f); }
}

bool app_system_at_least(const char *v) {
    char ver[64];
    system_version(ver, sizeof ver);
    return !ver[0] || vcmp(ver, v) >= 0;
}

static bool system_ok(void) {
    char p[1200], min[24] = "", ver[64];
    snprintf(p, sizeof p, "%s/app.json", app_dir);
    jval *j = json_load(p);
    snprintf(min, sizeof min, "%s", json_str(j, "min_system", ""));
    json_free(j);
    system_version(ver, sizeof ver);
    if (!min[0] || !ver[0] || vcmp(ver, min) >= 0) return true;
    ui_log("serve il sistema %s, in uso %s", min, ver);
    const char *fmt = T("store.needs_system");   /* nelle lingue del sistema dalla 0.9.45 */
    if (!strcmp(fmt, "store.needs_system")) fmt = "Requires Mosaik %s: update the system first.";
    char msg[300];
    snprintf(msg, sizeof msg, fmt, min);
    for (double until = UI.t + 30; !UI.in.quit && UI.t < until;) {
        ui_poll();
        if (UI.in.pressed[BTN_A] || UI.in.pressed[BTN_B] || UI.in.pressed[BTN_MENU]) break;
        ui_begin();
        int w = UI.w * 80 / 100, h = UI.h * 30 / 100, x, y;
        app_panel(w, h, &x, &y);
        char lines[3][160];
        int n = ui_wrap(UI.f_normal, msg, w - M(0.08f), lines, 3), fh = TTF_FontHeight(UI.f_normal);
        for (int k = 0; k < n; k++) ui_text(UI.f_normal, lines[k], UI.w / 2, y + h / 2 - n * fh / 2 + k * fh, UI.th.text, ALIGN_CENTER);
        const char *hk[] = { "B", T("common.back") };
        ui_hints(hk, 2);
        ui_end();
    }
    return false;
}

bool app_start(int argc, char **argv, const char *id) {
    const char *r = getenv("ROOT"), *auto_script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) r = argv[++i];
        else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_script = argv[++i];
        else if (!strcmp(argv[i], "--app") && i + 1 < argc) snprintf(app_dir, sizeof app_dir, "%s", argv[++i]);
    }
    if (!r) r = "sdcard";
    char *rp = realpath(r, NULL);   /* senza buffer fisso: realpath non conosce la sua misura */
    snprintf(app_root, sizeof app_root, "%s", rp ? rp : r);
    if (rp && strlen(rp) >= sizeof app_root) { free(rp); return false; }
    free(rp);
    if (!app_dir[0]) snprintf(app_dir, sizeof app_dir, "%s/Apps/%s", app_root, id);
    if (!ui_init(id, app_root, auto_script, 48000, true)) return false;
    /* Altoparlante a riposo come nel launcher (Brick Pro, issue #4): senza, dentro le app restava
       acceso e con la scheda al lavoro (Test della SD) il fruscio si sentiva sempre */
    plat_amp_config(UI.set.speaker_rest);
    UI.amp_auto = true;
    UI.rest_auto = true;
    char p[1200];
    snprintf(p, sizeof p, "%s/lang/%s.json", app_dir, UI.set.lang);
    texts = json_load(p);
    snprintf(p, sizeof p, "%s/lang/en.json", app_dir);
    texts_en = json_load(p);
    if (!system_ok()) { ui_shutdown(); return false; }
    return true;
}

/* chiusura sorvegliata: dall'inizio dell'uscita (prima di fermare audio e lavori in corso, che
   possono bloccarsi) al massimo 5 s, poi il segnale chiude l'app e gamewatch torna al launcher
   (audit APP-011, V26-03) */
void app_exit_begin(void) {
    static bool armed;
    if (!armed) { alarm(5); armed = true; }
}

void app_quit(void) {
    app_exit_begin();
    ui_shutdown();
}

const char *FT(const char *key) {
    const char *s = json_str(texts, key, NULL);
    if (!s) s = json_str(texts_en, key, NULL);
    return s ? s : T(key);
}

void app_panel(int w, int h, int *x, int *y) {
    theme_t *t = &UI.th;
    *x = (UI.w - w) / 2;
    *y = (UI.h - h) / 2;
    int r = M(t->panel_radius) * 2;
    ui_rect(0, 0, UI.w, UI.h, rgba(0, 0, 0, 120));
    ui_shadow(*x, *y + M(0.01f), w, h, r, M(0.04f), rgba(0, 0, 0, 90));
    ui_rrect(*x, *y, w, h, r, with_alpha(t->bg, 255));   /* fondo pieno: in Onda la superficie è trasparente */
    ui_rrect(*x, *y, w, h, r, t->surface);
}

void app_size(long long b, char *out, size_t n) {
    if (b >= 1LL << 30) snprintf(out, n, "%.1f GB", (double)b / (1 << 30));
    else if (b >= 1 << 20) snprintf(out, n, "%.1f MB", (double)b / (1 << 20));
    else snprintf(out, n, "%lld KB", (b + 1023) / 1024);
}
