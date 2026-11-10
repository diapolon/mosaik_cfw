#include "stickcal.h"
#include "../common/durable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Come trimui_inputd calcola l'asse, su entrambi i modelli (ricavato dal programma):
     f = (grezzo - zero) * 32760 / (max - zero)    sopra il centro
     f = (grezzo - zero) * 32760 / (zero - min)    sotto il centro
     valore mandato a SDL = -f
   Attorno al centro c'è una zona morta, quindi con lo zero vicino al centro vero quello non si
   potrebbe misurare. Per la misura si mette lo zero molto in basso: ogni posizione reale della
   levetta cade allora sul tratto "sopra il centro", fuori dalla zona morta e senza tagli. */
enum { M_ZERO = 300, M_MAX = 4095, M_MIN = 0, FULL = 32760 };

int stickcal_raw(int axis_value) {
    int raw = M_ZERO + (-axis_value) * (M_MAX - M_ZERO) / FULL;
    return raw < M_ZERO || raw > M_MAX ? -1 : raw;
}

static int min_span = 450;
void stickcal_set_min_span(int span) { min_span = span; }

bool stickcal_axis_ok(const stickcal_t *c, int a) {
    return c->zero[a] >= 1400 && c->zero[a] <= 2800 && c->min[a] >= 200 && c->max[a] <= 3900 &&
           c->zero[a] - c->min[a] >= min_span && c->max[a] - c->zero[a] >= min_span;
}

bool stickcal_ok(const stickcal_t *c) {
    for (int a = 0; a < SC_AXES; a++) if (!stickcal_axis_ok(c, a)) return false;
    return true;
}

static const char *const NAMES[2] = { "joypad.config", "joypad_right.config" };
static char saved[2][1024];
static bool saved_ok[2];

static void path_of(char *out, size_t n, const char *dir, int side) { snprintf(out, n, "%s/%s", dir, NAMES[side]); }

/* passa da un file provvisorio: quello vero o è il precedente o è completo */
static bool put_file(const char *path, const char *text) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs(text, f);
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok && durable_replace(tmp, path) == 0) return true;
    remove(tmp);
    return false;
}

static bool get_file(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    return true;
}

/* grilletti e zona morta esistono solo sulla Brick Pro: si conservano, altrimenti i valori di
   fabbrica del programma */
static double value_of(const char *text, const char *key, double def) {
    const char *p = text ? strstr(text, key) : NULL;
    double v;
    return p && sscanf(p + strlen(key), " = %lf", &v) == 1 ? v : def;
}

static void format_cfg(char *out, size_t n, const int v[6], bool brick, const char *old, bool measuring) {
    int w = snprintf(out, n, "x_min=%d\nx_max=%d\ny_min=%d\ny_max=%d\nx_zero=%d\ny_zero=%d\n", v[0], v[1], v[2], v[3], v[4], v[5]);
    if (brick && w > 0 && (size_t)w < n)
        snprintf(out + w, n - (size_t)w, "z_min=%d\nz_max=%d\ndeadzone=%.2f\n",
                 (int)value_of(old, "z_min", 250), (int)value_of(old, "z_max", 750),
                 measuring ? 0.0 : value_of(old, "deadzone", 0.10));
}

bool stickcal_begin(const char *udisk, bool brick) {
    bool ok = true;
    for (int s = 0; s < 2; s++) {
        char p[1100], text[256];
        path_of(p, sizeof p, udisk, s);
        saved_ok[s] = get_file(p, saved[s], sizeof saved[s]);
        const int v[6] = { M_MIN, M_MAX, M_MIN, M_MAX, M_ZERO, M_ZERO };
        format_cfg(text, sizeof text, v, brick, saved_ok[s] ? saved[s] : NULL, true);
        ok = put_file(p, text) && ok;
    }
    return ok;
}

void stickcal_cancel(const char *udisk, bool brick) {
    (void)brick;
    for (int s = 0; s < 2; s++) {
        char p[1100];
        path_of(p, sizeof p, udisk, s);
        if (saved_ok[s]) put_file(p, saved[s]);
        else unlink(p);
    }
}

/* un filo dentro i limiti misurati: così il massimo si raggiunge davvero anche senza forzare */
static int inward(int edge, int zero) { return edge + (zero - edge) * 3 / 100; }

bool stickcal_save(const char *sysdir, const char *udisk, const stickcal_t *c, bool brick) {
    if (!stickcal_ok(c)) { stickcal_cancel(udisk, brick); return false; }
    for (int s = 0; s < 2; s++) {
        int x = s ? SC_RX : SC_LX, y = s ? SC_RY : SC_LY;
        const int v[6] = { inward(c->min[x], c->zero[x]), inward(c->max[x], c->zero[x]),
                           inward(c->min[y], c->zero[y]), inward(c->max[y], c->zero[y]), c->zero[x], c->zero[y] };
        char text[256], p[1100];
        format_cfg(text, sizeof text, v, brick, saved_ok[s] ? saved[s] : NULL, false);
        path_of(p, sizeof p, sysdir, s);
        if (!put_file(p, text)) { stickcal_cancel(udisk, brick); return false; }
        path_of(p, sizeof p, udisk, s);
        if (!put_file(p, text)) { stickcal_cancel(udisk, brick); return false; }
    }
    return true;
}

bool stickcal_has_custom(const char *sysdir) {
    char p[1100];
    path_of(p, sizeof p, sysdir, 0);
    return access(p, F_OK) == 0;
}

void stickcal_reset(const char *sysdir, const char *udisk, bool brick) {
    (void)brick;
    for (int s = 0; s < 2; s++) {
        char p[1100];
        path_of(p, sizeof p, sysdir, s);
        unlink(p);
        path_of(p, sizeof p, udisk, s);
        unlink(p);
    }
}
