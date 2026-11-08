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

#define BRIGHT_OFF (-2)   /* retroilluminazione spenta (salvaschermo "schermo spento") */
static void set_brightness(int level) {
    static const int raw[11] = { 4, 6, 10, 16, 32, 48, 64, 96, 128, 192, 255 };
    static int logged;
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        if (!logged++) ui_log("luminosità: /dev/disp non disponibile");
        return;
    }
    unsigned long param[4] = { 0, level == BRIGHT_OFF ? 0ul : (unsigned long)raw[level < 0 ? 0 : level > 10 ? 10 : level], 0, 0 };
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

static void set_cpu(int khz) {
    char b[16];
    snprintf(b, sizeof b, "%d", khz);
    put_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "userspace");
    put_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", b);
}

static int cpu_fixed;   /* kHz chiesti dal launcher; 0 = profilo scelto nelle impostazioni */
static bool screen_off;
static int cpu_game;    /* kHz scelti per questo gioco o emulatore; 0 = profilo generale */

bool plat_device_name(const char *path, char *out, int n) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    out[0] = 0;
    bool ok = ioctl(fd, EVIOCGNAME(n), out) >= 0;
    close(fd);
    return ok;
}

static int fn_state = -1;
static bool reapply;   /* dopo la sospensione: si riapplica tutto */

/* LED (driver led_anim di TrimUI, come init_leds.sh e low_battery_led.sh del firmware).
   Zone: lr (sinistra/destra, anelli delle levette sulla Smart Pro), m (centro), f1 e f2 (le
   barrette della Brick Pro), rear (retro). Per ogni zona colore, ripetizioni, durata e poi
   l'effetto, che fa partire l'animazione. Le zone che un modello non ha non esistono. */
#define LED_DIR "/sys/class/led_anim/"
static const char *led_zones[] = { "lr", "m", "f1", "f2", "rear" };
static unsigned led_theme_rgb = 0xFFFFFF;

void plat_led_theme(unsigned rgb) { led_theme_rgb = rgb; }

static void led_put(const char *name, const char *zone, const char *v) {
    char p[96];
    snprintf(p, sizeof p, LED_DIR "%s%s%s", name, zone ? "_" : "", zone ? zone : "");
    put_str(p, v);
}

/* effect: codice del driver (0 spento, 2 respiro, 3 pulsazione, 4 fisso, 6 lampeggio) */
static void led_set(unsigned rgb, int effect, int duration_ms, int scale) {
    static int logged;
    if (access(LED_DIR "effect_enable", F_OK) != 0) {
        if (!logged++) ui_log("LED: driver led_anim assente");
        return;
    }
    char v[32];
    put_str(LED_DIR "effect_enable", "1");
    put_str(LED_DIR "anim_frames_enable", "0");
    snprintf(v, sizeof v, "%d", scale);
    put_str(LED_DIR "max_scale", v);
    put_str(LED_DIR "max_scale_lr", v);
    put_str(LED_DIR "max_scale_f1f2", v);
    put_str(LED_DIR "max_scale_rear", v);
    char zones[64] = "";
    for (unsigned i = 0; i < sizeof led_zones / sizeof *led_zones; i++) {
        char p[96];
        snprintf(p, sizeof p, LED_DIR "effect_%s", led_zones[i]);
        if (access(p, F_OK) != 0) continue;
        if (!logged) { strcat(zones, " "); strcat(zones, led_zones[i]); }
        snprintf(v, sizeof v, "%06X ", rgb & 0xFFFFFF);
        led_put("effect_rgb_hex", led_zones[i], v);
        led_put("effect_cycles", led_zones[i], effect ? "30000" : "0");
        snprintf(v, sizeof v, "%d", duration_ms);
        led_put("effect_duration", led_zones[i], v);
        snprintf(v, sizeof v, "%d", effect);
        led_put("effect", led_zones[i], v);
    }
    if (!logged++) ui_log("LED: zone%s", zones[0] ? zones : " nessuna");
    ui_log("LED: colore %06X, effetto %d, durata %d ms, scala %d", rgb & 0xFFFFFF, effect, duration_ms, scale);
}

static void apply_leds(const settings_t *s, bool low_battery) {
    if (low_battery) { led_set(0xFF0000, 6, 2000, 40); return; }   /* come il firmware */
    if (s->led_mode == 0) { led_set(0, 0, 1000, 0); return; }
    static const int effect[4] = { 0, 4, 2, 3 }, duration[4] = { 1000, 1000, 3000, 1500 };
    unsigned rgb = s->led_color > 0 && s->led_color < NLED_COLORS ? LED_COLORS[s->led_color].rgb : led_theme_rgb;
    led_set(rgb, effect[s->led_mode & 3], duration[s->led_mode & 3], s->led_bright * 4);
}

static int profile_khz(const settings_t *s) {
    static const int khz[3] = { 1200000, 1608000, 2000000 };
    int p = fn_state == 1 ? CPU_PERF : s->cpu_profile;
    return khz[p < 0 || p > 2 ? 1 : p];
}

void plat_apply(const settings_t *s) {
    static int bright = -1, vol = -1, cpu = -1, lmode = -1, lcolor = -1, lbright = -1;
    if (reapply) { reapply = false; bright = vol = cpu = lmode = lcolor = lbright = -1; }
    /* LED: solo nel sistema su SD (nell'app li gestisce il firmware TrimUI) */
    if (plat_standalone()) {
        static unsigned ltheme;
        static bool low;
        static unsigned tick;
        bool low_now = low;
        if (tick++ % 600 == 0) {   /* batteria ogni 10 s circa */
            bool ch;
            int pct = plat_battery(&ch);
            low_now = pct >= 0 && pct <= 10 && !ch;
        }
        unsigned theme_now = s->led_color == 0 ? led_theme_rgb : 0;
        if (s->led_mode != lmode || s->led_color != lcolor || s->led_bright != lbright || theme_now != ltheme || low_now != low) {
            lmode = s->led_mode; lcolor = s->led_color; lbright = s->led_bright; ltheme = theme_now; low = low_now;
            apply_leds(s, low);
        }
    }
    int want_bright = screen_off ? BRIGHT_OFF : s->brightness;
    if (want_bright != bright) { bright = want_bright; set_brightness(bright); }
    if (s->volume != vol) { vol = s->volume; set_volume(vol); }
    /* FN attivo: prestazioni massime, qualunque sia il profilo scelto */
    /* l'interruttore FN (prestazioni massime) vale più della scelta per il gioco */
    int want = cpu_fixed ? cpu_fixed : cpu_game && fn_state != 1 ? cpu_game : profile_khz(s);
    if (want != cpu) { cpu = want; set_cpu(cpu); }
}

void plat_cpu_fixed(int khz) { cpu_fixed = khz; }
void plat_screen_off(bool off) { screen_off = off; }
void plat_cpu_game(int khz) { cpu_game = khz; }

void plat_cpu_release(const settings_t *s) {
    if (!cpu_fixed) return;
    cpu_fixed = 0;
    set_cpu(profile_khz(s));
}

void plat_cpu_status(int *mhz, int *temp) {
    *mhz = get_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") / 1000;
    *temp = get_int("/sys/class/thermal/thermal_zone0/temp");
    if (*temp > 1000) *temp /= 1000;
}

/* gamepad di trimui_inputd: si cerca per nome, riprovando ogni tanto se non c'è ancora */
static int keys_fd = -1, power_fd = -1, vol_fd = -1;
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
        if (strstr(name, "sunxi-keyboard") && vol_fd < 0) { vol_fd = fd; continue; }
        close(fd);
    }
    if (d) closedir(d);
}

static int fn_gpio(void) {
    static bool exported;
    if (!exported) { put_str("/sys/class/gpio/export", "243"); put_str("/sys/class/gpio/gpio243/direction", "in"); exported = true; }
    return get_int("/sys/class/gpio/gpio243/value");
}

/* Tasti del volume: secondo il modello arrivano dal gamepad di trimui_inputd o dal dispositivo
   del kernel sunxi-keyboard, a volte da entrambi. Vale il primo che parla: mai due passi per una
   pressione. Anche tenuti premuti (valore 2 = ripetizione del kernel). */
static void vol_event(const struct input_event *ev, int source, plat_keys_t *k) {
    static int vol_source;
    if (ev->type != EV_KEY || ev->value < 1 || (ev->code != KEY_VOLUMEUP && ev->code != KEY_VOLUMEDOWN)) return;
    if (!vol_source) {
        vol_source = source;
        ui_log("tasti del volume: letti da %s", source == 1 ? "TRIMUI Player1" : "sunxi-keyboard");
    }
    if (source == vol_source) k->vol += ev->code == KEY_VOLUMEUP ? 1 : -1;
}

void plat_poll(plat_keys_t *k) {
    memset(k, 0, sizeof *k);
    if ((keys_fd < 0 || power_fd < 0 || vol_fd < 0) && scan_tick++ % 180 == 0) keys_open();
    /* tasto di accensione: breve = sospensione, tenuto premuto = spegnimento. Se il driver
       non segnalasse il rilascio, dopo 1,2 s vale comunque come pressione lunga. */
    static double power_t0 = -1;
    static bool long_sent, release_seen;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    struct input_event pev;
    while (power_fd >= 0 && read(power_fd, &pev, sizeof pev) == (ssize_t)sizeof pev) {
        if (pev.type != EV_KEY || pev.code != KEY_POWER) continue;
        if (pev.value == 1) { power_t0 = now; long_sent = false; }
        else if (pev.value == 0) {
            if (!release_seen) { release_seen = true; ui_log("tasto di accensione: il rilascio viene segnalato"); }
            if (power_t0 >= 0 && !long_sent) k->power = true;
            power_t0 = -1;
        }
    }
    if (power_t0 >= 0 && !long_sent && now - power_t0 >= 1.2) { k->power_long = true; long_sent = true; }
    struct input_event ev;
    while (keys_fd >= 0 && read(keys_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.value == 1) {
            if (ev.code == KEY_HOMEPAGE) k->home = true;
            if (ev.code == KEY_F1) k->f1 = true;
            if (ev.code == KEY_F2) k->f2 = true;
        }
        vol_event(&ev, 1, k);
    }
    while (vol_fd >= 0 && read(vol_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) vol_event(&ev, 2, k);
    /* FN: il GPIO 243 ogni mezzo secondo circa (a 60 fotogrammi al secondo) */
    static unsigned tick;
    if (tick++ % 30 == 0) {
        int v = fn_gpio();
        if (v >= 0 && v != fn_state) { k->fn_changed = fn_state != -1; fn_state = v; }
    }
    k->fn = fn_state;
}

bool plat_suspend(void) {
    ui_log("sospensione");
    int r = system("ARCADE_KEEP_MUTE=1 arcade-suspend");
    ui_log("risveglio (%s)", r == 0 ? "sospensione riuscita" : "sospensione non riuscita");
    reapply = true;
    /* eventi del tasto arrivati durante il sonno (quello che ha svegliato la console) */
    struct input_event ev;
    while (power_fd >= 0 && read(power_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {}
    return r == 0;
}

void plat_audio_release(void) {
    if (unlink("/run/arcade-audio-held") == 0) plat_audio_unmute();
}

void plat_audio_mute(void) {
    if (system("arcade-suspend mute") != 0) ui_log("audio: spegnimento non riuscito");
}

void plat_audio_unmute(void) {
    if (system("arcade-suspend unmute") != 0) ui_log("audio: riattivazione non riuscita");
}

bool plat_set_time(time_t t) {
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) { ui_log("data e ora: settimeofday non riuscita"); return false; }
    /* in UTC, come lo rilegge il sistema all'avvio (hwclock -s -u) */
    int r = system("hwclock -w -u 2>/dev/null");
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
void plat_cpu_fixed(int khz) { (void)khz; }
void plat_screen_off(bool off) { (void)off; }
void plat_cpu_game(int khz) { (void)khz; }
void plat_cpu_release(const settings_t *s) { (void)s; }
void plat_cpu_status(int *mhz, int *temp) { *mhz = *temp = 0; }
int plat_battery(bool *charging) { *charging = false; return -1; }
void plat_poll(plat_keys_t *k) { memset(k, 0, sizeof *k); k->fn = -1; }
bool plat_device_name(const char *path, char *out, int n) { (void)path; if (n) out[0] = 0; return false; }
bool plat_set_time(time_t t) { (void)t; return false; }
void plat_led_theme(unsigned rgb) { (void)rgb; }
bool plat_suspend(void) { return false; }
void plat_audio_release(void) {}
void plat_audio_mute(void) {}
void plat_audio_unmute(void) {}

#endif
