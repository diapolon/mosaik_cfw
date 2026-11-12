#include "appkit.h"
#include "../common/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (!realpath(r, app_root)) snprintf(app_root, sizeof app_root, "%s", r);
    if (!app_dir[0]) snprintf(app_dir, sizeof app_dir, "%s/Apps/%s", app_root, id);
    if (!ui_init(id, app_root, auto_script, 48000, true)) return false;
    char p[1200];
    snprintf(p, sizeof p, "%s/lang/%s.json", app_dir, UI.set.lang);
    texts = json_load(p);
    snprintf(p, sizeof p, "%s/lang/en.json", app_dir);
    texts_en = json_load(p);
    return true;
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
