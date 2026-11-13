#include "appkit.h"
#include "../common/json.h"
#include "../common/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char app_root[1024], app_dir[1024];
static jval *texts, *texts_en;

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
    char p[1200];
    snprintf(p, sizeof p, "%s/lang/%s.json", app_dir, UI.set.lang);
    texts = json_load(p);
    snprintf(p, sizeof p, "%s/lang/en.json", app_dir);
    texts_en = json_load(p);
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
