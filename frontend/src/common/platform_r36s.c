/* Hardware dell'R36S (Rockchip RK3326) per il prototipo del porting: docs/analisi-r36s.md.
   Si compila al posto della parte TrimUI di platform.c con -DR36S_DEVICE (Makefile.r36s).
   Usa solo interfacce standard di Linux, come le espongono i kernel per RK3326 (ROCKNIX,
   dArkOS): retroilluminazione in /sys/class/backlight, batteria e caricatore in
   /sys/class/power_supply, volume con ALSA, frequenza con cpufreq, tasti con evdev,
   sospensione con /sys/power/state. Niente LED comandabili, niente amplificatore separato.
   Ogni nome di dispositivo o controllo segnato "da verificare" va confermato sulla console. */
#ifdef R36S_DEVICE
#include "platform.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void ui_log(const char *fmt, ...);

bool plat_is_device(void) { return true; }
const char *plat_model(void) { return "r36s"; }

static bool put_str(const char *path, const char *v) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fputs(v, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int get_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (!f) return -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static bool get_str(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

/* prima cartella di /sys/class/<classe> che soddisfa la prova (NULL: la prima che c'è) */
static bool sys_find(const char *cls, bool (*ok)(const char *dir), char *out, size_t n) {
    char base[128];
    snprintf(base, sizeof base, "/sys/class/%s", cls);
    DIR *d = opendir(base);
    struct dirent *e;
    bool found = false;
    while (d && !found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(out, n, "%s/%s", base, e->d_name);
        found = !ok || ok(out);
    }
    if (d) closedir(d);
    return found;
}

/* ------------------------------------------------------------------ schermo */
static char bl_dir[256];
static int bl_max = -1;

static void set_brightness(int level, bool off) {
    if (!bl_dir[0] && !sys_find("backlight", NULL, bl_dir, sizeof bl_dir)) {
        static int logged;
        if (!logged++) ui_log("luminosità: nessuna retroilluminazione in /sys/class/backlight");
        return;
    }
    char p[320], v[16];
    if (bl_max < 0) { snprintf(p, sizeof p, "%s/max_brightness", bl_dir); bl_max = get_int(p); }
    if (bl_max <= 0) return;
    /* livelli 0-10 come sulla TrimUI; il minimo resta visibile */
    int lv = level < 0 ? 0 : level > 10 ? 10 : level;
    int raw = off ? 0 : bl_max * (lv + 1) / 11;
    snprintf(p, sizeof p, "%s/brightness", bl_dir);
    snprintf(v, sizeof v, "%d", raw);
    put_str(p, v);
    snprintf(p, sizeof p, "%s/bl_power", bl_dir);
    put_str(p, off ? "4" : "0");
}

/* ------------------------------------------------------------------- audio */
/* controllo del volume del codec RK817: il nome cambia fra kernel (da verificare) */
static void set_volume(int level) {
    static const char *const ctl[] = { "Master", "Playback", "DAC", "Speaker" };
    static int which = -1;
    char cmd[200];
    int pct = level * 5;
    for (int i = which < 0 ? 0 : which; i < 4; i++) {
        snprintf(cmd, sizeof cmd, "amixer -q sset '%s' %d%% unmute >/dev/null 2>&1", ctl[i], pct);
        if (system(cmd) == 0) {
            if (which != i) ui_log("volume: controllo ALSA '%s'", ctl[i]);
            which = i;
            return;
        }
        if (which >= 0) break;
    }
    static int logged;
    if (!logged++) ui_log("volume: nessun controllo ALSA conosciuto");
}

/* --------------------------------------------------------------------- CPU */
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"
/* Con il governor userspace si fissa la frequenza; senza (alcuni kernel non lo hanno) si limita
   il massimo e lascia fare al governor del sistema. Frequenze dal kernel, come sulla TrimUI. */
static int cpu_supported(int khz) {
    FILE *f = fopen(CPUFREQ "scaling_available_frequencies", "r");
    if (!f) return khz;
    int v, below = 0, lowest = 0;
    while (fscanf(f, "%d", &v) == 1) {
        if (v <= khz && v > below) below = v;
        if (v > 0 && (!lowest || v < lowest)) lowest = v;
    }
    fclose(f);
    return below ? below : lowest ? lowest : khz;
}

static void set_cpu(int khz) {
    int target = cpu_supported(khz);
    char v[16], gov[32] = "";
    snprintf(v, sizeof v, "%d", target);
    get_str(CPUFREQ "scaling_governor", gov, sizeof gov);
    bool user = !strcmp(gov, "userspace") || put_str(CPUFREQ "scaling_governor", "userspace");
    if (user) put_str(CPUFREQ "scaling_setspeed", v);
    else put_str(CPUFREQ "scaling_max_freq", v);
    static int logged;
    if (logged++ < 3) ui_log("CPU: chiesti %d kHz, impostati %d (%s)", khz, target, user ? "userspace" : "massimo");
}

/* profili come sulla TrimUI, scalati alle frequenze dell'RK3326 (fino a ~1,3-1,5 GHz) */
static int cpu_fixed, cpu_game;
static bool screen_off, reapply;
static int profile_khz(const settings_t *s) {
    static const int khz[3] = { 1008000, 1200000, 1512000 };
    int p = s->cpu_profile;
    return khz[p < 0 || p > 2 ? 1 : p];
}

void plat_apply(const settings_t *s) {
    static int bright = -1, vol = -1, cpu = -1;
    if (reapply) { reapply = false; bright = vol = cpu = -1; }
    int want_bright = screen_off ? -2 : s->brightness;
    if (want_bright != bright) { bright = want_bright; set_brightness(s->brightness, screen_off); }
    if (s->volume != vol) { vol = s->volume; set_volume(vol); }
    int want = cpu_fixed ? cpu_fixed : cpu_game ? cpu_game : profile_khz(s);
    if (want != cpu) { cpu = want; set_cpu(cpu); }
}

void plat_cpu_fixed(int khz) { cpu_fixed = khz; }
void plat_screen_off(bool off) { screen_off = off; }
void plat_cpu_game(int khz) { cpu_game = khz; }
void plat_cpu_release(const settings_t *s) { if (!cpu_fixed) return; cpu_fixed = 0; set_cpu(profile_khz(s)); }

void plat_cpu_status(int *mhz, int *temp) {
    *mhz = get_int(CPUFREQ "scaling_cur_freq") / 1000;
    *temp = get_int("/sys/class/thermal/thermal_zone0/temp");
    if (*temp > 1000) *temp /= 1000;
}

/* ----------------------------------------------------------------- batteria */
static bool is_battery(const char *dir) {
    char p[320], t[32] = "";
    snprintf(p, sizeof p, "%s/type", dir);
    return get_str(p, t, sizeof t) && !strcmp(t, "Battery");
}
static bool is_charger(const char *dir) {
    char p[320], t[32] = "";
    snprintf(p, sizeof p, "%s/type", dir);
    return get_str(p, t, sizeof t) && (!strcmp(t, "USB") || !strcmp(t, "Mains"));
}

bool plat_battery_detail(int *ma, long *tte_s, long *full_mah) { *ma = 0; *tte_s = -1; *full_mah = -1; return false; }

int plat_battery(bool *charging) {
    static char bat[256], chg[256];
    static bool looked;
    if (!looked) {
        looked = true;
        if (!sys_find("power_supply", is_battery, bat, sizeof bat)) bat[0] = 0;
        if (!sys_find("power_supply", is_charger, chg, sizeof chg)) chg[0] = 0;
        ui_log("batteria: %s, caricatore: %s", bat[0] ? bat : "—", chg[0] ? chg : "—");
    }
    char p[320], st[32] = "";
    *charging = false;
    if (chg[0]) { snprintf(p, sizeof p, "%s/online", chg); *charging = get_int(p) == 1; }
    if (bat[0] && !*charging) {   /* senza caricatore separato: lo stato della batteria */
        snprintf(p, sizeof p, "%s/status", bat);
        *charging = get_str(p, st, sizeof st) && !strcmp(st, "Charging");
    }
    if (!bat[0]) return -1;
    snprintf(p, sizeof p, "%s/capacity", bat);
    return get_int(p);
}

/* -------------------------------------------------------------------- tasti */
/* Tasto di accensione (KEY_POWER, dal PMIC RK817) e tasti del volume (gpio-keys o adc-keys):
   si cercano fra i dispositivi evdev quelli che li hanno. I tasti di gioco e le levette li
   legge SDL. Nomi e codici da verificare sulla console. */
static int power_fd = -1, vol_fd = -1;
static unsigned scan_tick;

bool plat_device_name(const char *path, char *out, int n) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    out[0] = 0;
    bool ok = ioctl(fd, EVIOCGNAME(n), out) >= 0;
    close(fd);
    return ok;
}

static bool has_key(int fd, int code) {
    unsigned char bits[KEY_MAX / 8 + 1];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return false;
    return bits[code / 8] >> (code % 8) & 1;
}

static void keys_open(void) {
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (power_fd < 0 && has_key(fd, KEY_POWER)) { power_fd = fd; continue; }
        if (vol_fd < 0 && has_key(fd, KEY_VOLUMEUP)) { vol_fd = fd; continue; }
        close(fd);
    }
    if (d) closedir(d);
}

void plat_poll(plat_keys_t *k) {
    memset(k, 0, sizeof *k);
    k->fn = -1;
    if ((power_fd < 0 || vol_fd < 0) && scan_tick++ % 180 == 0) keys_open();
    static double power_t0 = -1;
    static bool long_sent;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    struct input_event ev;
    while (power_fd >= 0 && read(power_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY || ev.code != KEY_POWER) continue;
        if (ev.value == 1) { power_t0 = now; long_sent = false; }
        else if (ev.value == 0 && power_t0 >= 0) { if (!long_sent) k->power = true; power_t0 = -1; }
    }
    if (power_t0 >= 0 && !long_sent && now - power_t0 >= 1.2) { k->power_long = true; long_sent = true; }
    while (vol_fd >= 0 && read(vol_fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
        if (ev.type == EV_KEY && ev.value >= 1 && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN))
            k->vol += ev.code == KEY_VOLUMEUP ? 1 : -1;
}

/* ------------------------------------------------------------- tempo, ecc. */
bool plat_set_time(time_t t) {
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) { ui_log("data e ora: settimeofday non riuscita"); return false; }
    int r = system("hwclock -w -u 2>/dev/null");
    ui_log("data e ora impostate, orologio %s", r == 0 ? "aggiornato" : "non aggiornato");
    return true;
}

bool plat_sync_time(void) {
    /* una richiesta e poi esce (-q), in primo piano (-n); timeout di BusyBox per la rete lenta */
    int r = system("timeout 20 ntpd -n -q -p pool.ntp.org -p time.cloudflare.com > /dev/null 2>&1");
    if (r != 0) { ui_log("ora automatica: nessuna risposta (%d)", r); return false; }
    int w = system("hwclock -w -u 2>/dev/null");
    ui_log("ora automatica: presa da internet, orologio della console %s", w == 0 ? "aggiornato" : "non aggiornato");
    return true;
}

void plat_led_theme(unsigned rgb) { (void)rgb; }   /* solo il LED di carica, non comandabile */

/* sospensione: lo script del sistema se c'è (come arcade-suspend sulla TrimUI), altrimenti il
   kernel direttamente. Sull'RK3326 la sospensione profonda non è affidabile ovunque: da provare */
bool plat_suspend(void) {
    int r = access("/usr/arcade/bin/arcade-suspend", X_OK) == 0 ? system("arcade-suspend") : -1;
    if (r != 0) { sync(); r = put_str("/sys/power/state", "mem") ? 0 : -1; }
    reapply = true;
    return r == 0;
}

void plat_audio_release(void) {}
void plat_audio_mute(void) {}
void plat_audio_unmute(void) {}
void plat_amp_config(int method) { (void)method; }
void plat_amp_wake(void) {}
void plat_amp_idle(bool keep) { (void)keep; }
void plat_amp_restore(void) {}

#endif
