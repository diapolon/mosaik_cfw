#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool plat_standalone(void) { const char *e = getenv("ARCADE_STANDALONE"); return e && *e == '1'; }

#ifdef TRIMUI_DEVICE
#include <dirent.h>
#include <linux/input.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

bool plat_is_device(void) { return true; }

const char *plat_model(void) {
    const char *m = getenv("TRIMUI_MODEL");
    if (!m || !*m) return NULL;
    return strstr(m, "Brick") ? "brickpro" : "smartpro";
}

static void put_str(const char *path, const char *v) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(v, f);
    fclose(f);
}

static int get_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (!f) return -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* retroilluminazione: ioctl del driver display Allwinner, valori 0-255 */
#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103
void ui_log(const char *fmt, ...);   /* da ui.c (ui.h qui non si può includere: vedi linux/input.h) */

static void set_brightness(int level) {
    static const int raw[11] = { 4, 6, 10, 16, 32, 48, 64, 96, 128, 192, 255 };
    static int logged;
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        if (!logged++) ui_log("luminosità: /dev/disp non disponibile");
        return;
    }
    unsigned long param[4] = { 0, (unsigned long)raw[level < 0 ? 0 : level > 10 ? 10 : level], 0, 0 };
    int r = ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
    unsigned long q[4] = { 0, 0, 0, 0 };
    int now = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, &q);
    /* nel log solo le prime volte e gli errori: basta per capire se il driver risponde */
    if (r < 0 || logged < 3) { logged++; ui_log("luminosità: livello %d -> %lu (esito %d, ora %d)", level, param[1], r, now); }
    close(fd);
}

/* volume: il controllo "digital volume" del codec è invertito (0 = massimo) */
static void set_volume(int level) {
    int pct = level * 5;   /* 0..20 -> 0..100 */
    char cmd[160];
    snprintf(cmd, sizeof cmd, "amixer sset 'digital volume' -M %d%% >/dev/null 2>&1", 100 - pct);
    if (system(cmd) != 0) return;
    if (system(pct ? "amixer sset 'DAC volume' 160 >/dev/null 2>&1" : "amixer sset 'DAC volume' 0 >/dev/null 2>&1")) {}
}

static void set_cpu(int profile) {
    static const char *freq[3] = { "1200000", "1608000", "2000000" };
    put_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "userspace");
    put_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", freq[profile < 0 || profile > 2 ? 1 : profile]);
}

bool plat_device_name(const char *path, char *out, int n) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    out[0] = 0;
    bool ok = ioctl(fd, EVIOCGNAME(n), out) >= 0;
    close(fd);
    return ok;
}

static int fn_state = -1;

void plat_apply(const settings_t *s) {
    static int bright = -1, vol = -1, cpu = -1;
    if (s->brightness != bright) { bright = s->brightness; set_brightness(bright); }
    if (s->volume != vol) { vol = s->volume; set_volume(vol); }
    /* FN attivo: prestazioni massime, qualunque sia il profilo scelto */
    int want = fn_state == 1 ? CPU_PERF : s->cpu_profile;
    if (want != cpu) { cpu = want; set_cpu(cpu); }
}

/* gamepad di trimui_inputd: si cerca per nome, riprovando ogni tanto se non c'è ancora */
static int keys_fd = -1, power_fd = -1;
static unsigned scan_tick;

static void keys_open(void) {
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300], name[128] = "";
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (strstr(name, "TRIMUI Player1") && keys_fd < 0) { keys_fd = fd; continue; }
        if (strstr(name, "axp2202-pek") && power_fd < 0) { power_fd = fd; continue; }
        close(fd);
    }
    if (d) closedir(d);
}

static int fn_gpio(void) {
    static bool exported;
    if (!exported) { put_str("/sys/class/gpio/export", "243"); put_str("/sys/class/gpio/gpio243/direction", "in"); exported = true; }
    return get_int("/sys/class/gpio/gpio243/value");
}

void plat_poll(plat_keys_t *k) {
    memset(k, 0, sizeof *k);
    if ((keys_fd < 0 || power_fd < 0) && scan_tick++ % 180 == 0) keys_open();
    struct input_event pev;
    while (power_fd >= 0 && read(power_fd, &pev, sizeof pev) == (ssize_t)sizeof pev)
        if (pev.type == EV_KEY && pev.code == KEY_POWER && pev.value == 1) k->power = true;
    struct input_event ev;
    while (keys_fd >= 0 && read(keys_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.value == 1) {
            if (ev.code == KEY_HOMEPAGE) k->home = true;
            if (ev.code == KEY_F1) k->f1 = true;
            if (ev.code == KEY_F2) k->f2 = true;
        }
        /* volume: anche tenuto premuto (valore 2 = ripetizione del kernel) */
        if (ev.type == EV_KEY && ev.value >= 1) {
            if (ev.code == KEY_VOLUMEUP) k->vol++;
            if (ev.code == KEY_VOLUMEDOWN) k->vol--;
        }
    }
    /* FN: il GPIO 243 ogni mezzo secondo circa (a 60 fotogrammi al secondo) */
    static unsigned tick;
    if (tick++ % 30 == 0) {
        int v = fn_gpio();
        if (v >= 0 && v != fn_state) { k->fn_changed = fn_state != -1; fn_state = v; }
    }
    k->fn = fn_state;
}

bool plat_set_time(time_t t) {
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) { ui_log("data e ora: settimeofday non riuscita"); return false; }
    /* stesso formato che il sistema rilegge all'avvio con "hwclock -s" */
    int r = system("hwclock -w 2>/dev/null");
    ui_log("data e ora impostate, orologio della console %s", r == 0 ? "aggiornato" : "non aggiornato");
    return true;
}

int plat_battery(bool *charging) {
    *charging = get_int("/sys/class/power_supply/axp2202-usb/online") == 1;
    return get_int("/sys/class/power_supply/axp2202-battery/capacity");
}

#else

bool plat_is_device(void) { return false; }
const char *plat_model(void) { return NULL; }
void plat_apply(const settings_t *s) { (void)s; }
int plat_battery(bool *charging) { *charging = false; return -1; }
void plat_poll(plat_keys_t *k) { memset(k, 0, sizeof *k); k->fn = -1; }
bool plat_device_name(const char *path, char *out, int n) { (void)path; if (n) out[0] = 0; return false; }
bool plat_set_time(time_t t) { (void)t; return false; }

#endif
