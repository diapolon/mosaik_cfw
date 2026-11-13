/* Diagnostica: app di Mosaik, pacchetto dello Store (Arcade/Apps/diag).

   Una pagina con lo stato della console, per capire un problema senza collegare la scheda al
   computer: sistema e area di avvio, scheda e spazio, batteria (tensione, corrente, temperatura),
   processore e memoria, Wi-Fi, controller. A prova la velocità del Wi-Fi scaricando 4 MB
   dall'indirizzo degli aggiornamenti; X salva un rapporto in Logs/diagnostica-<data>.txt con
   tutto questo e le ultime righe dei log, da allegare a una segnalazione. Nel rapporto il nome
   della rete e gli indirizzi non ci sono: le segnalazioni sono pubbliche.

   Sul Mac: build/diag --root sdcard --app package/apps/diag (molti valori mancano). */
#include "appkit.h"
#include "../common/platform.h"
#include "../common/wifi.h"
#include <curl/curl.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef UPDATE_DEFAULT_URL
#define UPDATE_DEFAULT_URL ""
#endif

/* ------------------------------------------------------------ dati */

#define MAXROWS 64
static struct { char k[64], v[160]; bool head; } rows[MAXROWS];
static int nrows;
static char ssid_now[40], ip_now[20];   /* da togliere dal rapporto */

static void head(const char *key) {
    if (nrows >= MAXROWS) return;
    snprintf(rows[nrows].k, sizeof rows[0].k, "%s", FT(key));
    rows[nrows].v[0] = 0;
    rows[nrows++].head = true;
}

static void row(const char *key, const char *fmt, ...) {
    if (nrows >= MAXROWS) return;
    snprintf(rows[nrows].k, sizeof rows[0].k, "%s", FT(key));
    va_list a;
    va_start(a, fmt);
    vsnprintf(rows[nrows].v, sizeof rows[0].v, fmt, a);
    va_end(a);
    rows[nrows++].head = false;
}

static bool read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok && out[0];
}

static long long read_ll(const char *path, bool *ok) {
    char b[64];
    *ok = read_line(path, b, sizeof b);
    return *ok ? atoll(b) : 0;
}

static void size_text(long long b, char *out, size_t n) { app_size(b, out, n); }

/* prova del Wi-Fi in un thread: 0 nessuna, 1 in corso, 2 finita, 3 errore */
static atomic_int speed_state;
static double speed_mbs;
static char speed_err[96];

static size_t sink(void *p, size_t s, size_t n, void *u) { (void)p; (void)u; return s * n; }

static void *speed_worker(void *arg) {
    (void)arg;
    char url[600], ca[1200];
    snprintf(url, sizeof url, "%s/file-0", UPDATE_DEFAULT_URL);
    snprintf(ca, sizeof ca, "%s/System/cacert.pem", app_root);
    CURL *c = curl_easy_init();
    if (!c) { snprintf(speed_err, sizeof speed_err, "curl"); speed_state = 3; return NULL; }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-4194303");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mosaik-diag");
    struct stat sb;
    if (stat(ca, &sb) == 0) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    CURLcode r = curl_easy_perform(c);
    curl_off_t bytes = 0, speed = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &bytes);
    curl_easy_getinfo(c, CURLINFO_SPEED_DOWNLOAD_T, &speed);
    curl_easy_cleanup(c);
    if (r != CURLE_OK || bytes < 65536) {
        snprintf(speed_err, sizeof speed_err, "%s", r != CURLE_OK ? curl_easy_strerror(r) : "troppo pochi dati");
        speed_state = 3;
    } else {
        speed_mbs = speed / 1048576.0;
        speed_state = 2;
    }
    ui_log("diagnostica: prova del Wi-Fi %s (%.2f MB/s, %lld byte)", speed_state == 2 ? "riuscita" : speed_err, speed_mbs, (long long)bytes);
    return NULL;
}

static pthread_t speed_thr;
static bool speed_thr_on;

static void speed_start(void) {
    if (speed_state == 1) return;
    if (speed_thr_on) { pthread_join(speed_thr, NULL); speed_thr_on = false; }
    speed_state = 1;
    speed_thr_on = pthread_create(&speed_thr, NULL, speed_worker, NULL) == 0;
    if (!speed_thr_on) speed_state = 0;
}

static void collect(void) {
    nrows = 0;
    char b[256], c[64], d[64];
    bool ok;
    /* sistema */
    head("diag.system");
    row("diag.model", "%s", plat_model() ? plat_model() : device_label(UI.set.device));   /* sul Mac: il modello simulato */
    if (!read_line("/etc/arcade-version", b, sizeof b)) snprintf(b, sizeof b, "%s", "-");
    row("diag.version", "%s", b);
    char cardgen[16] = "0", sysgen[16] = "-", p[1300];
    snprintf(p, sizeof p, "%s/System/area-avvio.txt", app_root);
    read_line(p, cardgen, sizeof cardgen);
    read_line("/etc/area-avvio", sysgen, sizeof sysgen);
    row("diag.boot_area", FT("diag.boot_area_v"), cardgen, sysgen);
    struct utsname u;
    if (uname(&u) == 0) row("diag.kernel", "%s", u.release);
    if (read_line("/proc/uptime", b, sizeof b)) { long s = atol(b); row("diag.uptime", "%ld h %02ld min", s / 3600, s / 60 % 60); }
    /* scheda */
    head("diag.card");
    struct statvfs v;
    if (statvfs(app_root, &v) == 0) {
        size_text((long long)v.f_bavail * v.f_frsize, c, sizeof c);
        size_text((long long)v.f_blocks * v.f_frsize, d, sizeof d);
        row("diag.space", FT("diag.space_v"), c, d);
    }
    int frag = 0;   /* frammenti del controllo della scheda nella cartella principale */
    char root_dir[1100];
    snprintf(root_dir, sizeof root_dir, "%s/..", app_root);
    DIR *dd = opendir(root_dir);
    struct dirent *e;
    while (dd && (e = readdir(dd))) frag += !strncasecmp(e->d_name, "FSCK", 4) || (strlen(e->d_name) == 12 && !strcasecmp(e->d_name + 9, "CHK"));
    if (dd) closedir(dd);
    row("diag.fragments", "%d", frag);
    /* batteria */
    head("diag.battery");
    bool charging;
    int pct = ui_battery(&charging);
    if (pct >= 0) row("diag.charge", "%d%%%s", pct, charging ? FT("diag.charging") : "");
    long long uv = read_ll("/sys/class/power_supply/axp2202-battery/voltage_now", &ok);
    if (ok) row("diag.voltage", "%.2f V", uv / 1e6);
    long long ua = read_ll("/sys/class/power_supply/axp2202-battery/current_now", &ok);
    if (ok) row("diag.current", "%lld mA", ua / 1000);
    long long bt = read_ll("/sys/class/power_supply/axp2202-battery/temp", &ok);
    if (ok) row("diag.bat_temp", "%.1f °C", bt / 10.0);
    if (read_line("/sys/class/power_supply/axp2202-battery/health", b, sizeof b)) row("diag.health", "%s", b);
    /* processore e memoria */
    head("diag.cpu");
    int mhz = 0, temp = 0;
    plat_cpu_status(&mhz, &temp);
    if (mhz > 0) row("diag.freq", "%d MHz", mhz);
    if (temp > 0) row("diag.temp", "%d °C", temp);
    long tot = -1, avail = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    while (f && fgets(b, sizeof b, f)) { long kb; if (sscanf(b, "MemTotal: %ld kB", &kb) == 1) tot = kb; if (sscanf(b, "MemAvailable: %ld kB", &kb) == 1) avail = kb; }
    if (f) fclose(f);
    if (tot > 0 && avail >= 0) row("diag.memory", FT("diag.memory_v"), avail / 1024, tot / 1024);
    if (read_line("/proc/loadavg", b, sizeof b)) { b[14] = 0; row("diag.load", "%s", b); }
    /* Wi-Fi */
    head("diag.wifi");
    const wifi_status_t *ws = wifi_status(false);
    ssid_now[0] = ip_now[0] = 0;
    if (!UI.set.wifi) row("diag.state", "%s", T("common.off"));
    else if (ws && ws->ssid[0]) {
        snprintf(ssid_now, sizeof ssid_now, "%s", ws->ssid);
        snprintf(ip_now, sizeof ip_now, "%s", ws->ip);
        row("diag.state", FT("diag.connected"), ws->ssid, ws->bars);
        if (ws->ip[0]) row("diag.address", "%s", ws->ip);
    } else row("diag.state", "%s", FT("diag.not_connected"));
    int st = speed_state;
    if (st == 1) row("diag.speed", "%s", FT("diag.testing"));
    else if (st == 2) row("diag.speed", "%.2f MB/s", speed_mbs);
    else if (st == 3) row("diag.speed", FT("diag.speed_failed"), speed_err);
    else row("diag.speed", "%s", FT("diag.speed_hint"));
    /* controller */
    head("diag.pads");
    int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj && i < 4; i++) { const char *n = SDL_JoystickNameForIndex(i); row("diag.pad", "%s", n ? n : "?"); }
    if (!nj) row("diag.pad", "-");
}

/* ------------------------------------------------------------ rapporto */

/* una riga del log con il nome della rete e l'indirizzo tolti */
static void put_clean(FILE *o, const char *line) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", line);
    const char *tok[2] = { ssid_now, ip_now };
    for (int t = 0; t < 2; t++) {
        if (!tok[t][0]) continue;
        char *s;
        while ((s = strstr(buf, tok[t]))) {
            char rest[1024];
            snprintf(rest, sizeof rest, "%s", s + strlen(tok[t]));
            snprintf(s, sizeof buf - (size_t)(s - buf), "%s%s", t ? "<indirizzo>" : "<rete>", rest);
        }
    }
    fputs(buf, o);
}

static void tail_file(FILE *o, const char *name, int lines) {
    char p[1300];
    snprintf(p, sizeof p, "%s/Logs/%s", app_root, name);
    FILE *f = fopen(p, "r");
    if (!f) return;
    /* ultime righe: si legge tutto tenendo un anello di "lines" righe */
    size_t cap = (size_t)lines, n = 0;
    char (*ring)[512] = calloc(cap, sizeof *ring);
    char b[512];
    while (ring && fgets(b, sizeof b, f)) snprintf(ring[n++ % cap], sizeof ring[0], "%s", b);
    fclose(f);
    size_t first = n > cap ? n - cap : 0;
    fprintf(o, "\n===== %s (ultime %zu righe)\n", name, n - first);
    for (size_t i = first; ring && i < n; i++) put_clean(o, ring[i % cap]);
    free(ring);
}

static void report(void) {
    char p[1300], stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(p, sizeof p, "%s/Logs/diagnostica-%s.txt", app_root, stamp);
    FILE *o = fopen(p, "w");
    if (!o) { ui_toast(FT("diag.report_failed")); ui_sound(SND_ERROR); return; }
    fprintf(o, "Mosaik, diagnostica del %s\n", stamp);
    for (int i = 0; i < nrows; i++) {
        if (rows[i].head) { fprintf(o, "\n[%s]\n", rows[i].k); continue; }
        char line[300];
        snprintf(line, sizeof line, "%s: %s\n", rows[i].k, rows[i].v);
        put_clean(o, line);
    }
    static const char *const logs[] = { "passi.txt", "avvio.log", "Arcade.log", "sdgrow.txt", "fsck.txt", "sospensione.txt" };
    for (size_t i = 0; i < sizeof logs / sizeof *logs; i++) tail_file(o, logs[i], 60);
    bool ok = !ferror(o);
    if (fclose(o) != 0) ok = false;
    sync();
    char msg[200];
    snprintf(msg, sizeof msg, FT(ok ? "diag.report_done" : "diag.report_failed"), stamp);
    ui_toast(msg);
    ui_sound(ok ? SND_OK : SND_ERROR);
    ui_log("diagnostica: rapporto %s", ok ? p : "non scritto");
}

/* ------------------------------------------------------------ schermata */

static int sel;
static float scroll;

static void draw(void) {
    theme_t *t = &UI.th;
    ui_topbar(FT("diag.title"));
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int mx = M(t->margin), W = UI.w - 2 * mx, rh = TTF_FontHeight(fn) + M(0.012f);
    int top = M(t->topbar_h) + M(0.02f), bottom = UI.h - ui_keybar_h() - M(0.01f), vis = (bottom - top) / rh;
    int first = (int)scroll;
    if (sel < first) first = sel;
    if (sel >= first + vis) first = sel - vis + 1;
    if (first < 0) first = 0;
    scroll = (float)first;
    char fit[200];
    for (int i = first; i < nrows && i < first + vis; i++) {
        int y = top + (i - first) * rh;
        if (rows[i].head) { ui_text(fs, rows[i].k, mx, y + rh - TTF_FontHeight(fs) - M(0.004f), t->accent, ALIGN_LEFT); continue; }
        ui_text(fn, rows[i].k, mx + M(0.02f), y, t->text_dim, ALIGN_LEFT);
        ui_text_fit(fn, rows[i].v, W * 58 / 100, fit, sizeof fit);
        ui_text(fn, fit, mx + W, y, t->text, ALIGN_RIGHT);
    }
    const char *h[] = { "B", FT("diag.quit"), NULL, "X", FT("diag.report"), NULL, "A", FT("diag.test_wifi"), NULL };
    ui_keybar(h, 9);
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "diag")) return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    double next = 0;
    bool quit = false;
    while (!quit) {
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        UI.busy = atomic_load(&speed_state) == 1;   /* prova del Wi-Fi in corso */
        input_t *in = &UI.in;
        if (in->pressed[BTN_B] || in->pressed[BTN_MENU]) quit = true;
        int vis = 10;
        if (in->pressed[BTN_DOWN]) { scroll += 1; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_UP] && scroll > 0) { scroll -= 1; ui_sound(SND_MOVE); }
        if (in->pressed[BTN_R]) scroll += vis;
        if (in->pressed[BTN_L]) scroll = scroll > vis ? scroll - vis : 0;
        if (scroll > nrows - 1) scroll = nrows > 0 ? nrows - 1 : 0;
        sel = (int)scroll;
        if (in->pressed[BTN_A]) {
            if (!UI.set.wifi) { ui_toast(FT("diag.wifi_off")); ui_sound(SND_ERROR); }
            else { speed_start(); ui_sound(SND_OK); next = 0; }
        }
        if (in->pressed[BTN_X]) { collect(); report(); }
        if (UI.t >= next) { collect(); next = UI.t + 2; }   /* ogni 2 s, e subito dopo una prova */
        if (speed_state != 1 && speed_thr_on) next = next > UI.t + 0.1 ? UI.t : next;
        ui_begin();
        draw();
        ui_end();
    }
    app_exit_begin();
    if (speed_thr_on) pthread_join(speed_thr, NULL);
    curl_global_cleanup();
    app_quit();
    return 0;
}
