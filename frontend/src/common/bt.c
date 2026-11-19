#include "bt.h"
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

__attribute__((unused)) static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool addr_ok(const char *a) {
    if (!a || strlen(a) != 17) return false;
    for (int i = 0; i < 17; i++) {
        char c = a[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

/* abbinati prima, e fra questi il collegato; poi audio, controller, il resto; poi per nome */
static int cmp_dev(const void *a, const void *b) {
    const bt_dev_t *x = a, *y = b;
    if (x->connected != y->connected) return y->connected - x->connected;
    if (x->paired != y->paired) return y->paired - x->paired;
    if (x->kind != y->kind) return x->kind - y->kind;
    return strcasecmp(x->name, y->name);
}

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static bt_status_t st;
static double st_time = -10, enabled_at = -100;
static bool st_busy, want_on, scanning;

#if defined(TRIMUI_DEVICE)
/* ------------------------------------------------------------ console: m2/bt-ctl */
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

bool bt_supported(void) { return plat_standalone() && access("/usr/arcade/bin/bt-ctl", X_OK) == 0; }

/* bt-ctl con i suoi argomenti, senza shell; l'uscita in out */
static bool ctl(char *out, size_t n, ...) {
    const char *argv[8] = { "bt-ctl" };
    int argc = 1;
    va_list ap;
    va_start(ap, n);
    const char *a;
    while (argc < 7 && (a = va_arg(ap, const char *))) argv[argc++] = a;
    va_end(ap);
    argv[argc] = NULL;
    int p[2];
    if (pipe(p) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    pid_t pid;
    int rc = posix_spawnp(&pid, "bt-ctl", &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    size_t len = 0;
    if (rc == 0) {
        ssize_t r;
        char tmp[512];
        while ((r = read(p[0], tmp, sizeof tmp)) > 0)
            for (ssize_t i = 0; i < r; i++)
                if (out && len + 1 < n) out[len++] = tmp[i];
    }
    if (out && n) out[len] = 0;
    close(p[0]);
    int status = 0;
    if (rc == 0) waitpid(pid, &status, 0);
    return rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void bt_enable(bool on) {
    want_on = on;
    if (on) enabled_at = now_s();
    int r = system(on ? "touch /run/bt-touched; bt-ctl on > /dev/null 2>&1 &"
                      : "touch /run/bt-touched; bt-ctl off > /dev/null 2>&1 &");
    (void)r;
    st_time = -10;
}

/* "spento" / "acceso" / "acceso\ncollegato INDIRIZZO tipo nome" */
static void status_read(bt_status_t *t) {
    char buf[256];
    memset(t, 0, sizeof *t);
    ctl(buf, sizeof buf, "status", NULL);
    struct stat sb;
    bool starting = stat("/run/bt/avvio", &sb) == 0 || (want_on && now_s() - enabled_at < 30);
    if (strncmp(buf, "acceso", 6)) { t->state = starting ? BT_STARTING : BT_OFF; return; }
    t->state = BT_ON;
    char *c = strstr(buf, "\ncollegato ");
    if (c && strlen(c) > 28 && c[28] == ' ') {
        memcpy(t->addr, c + 11, 17);
        t->addr[17] = 0;
        const char *k = c + 29, *sp = strchr(k, ' ');
        t->kind = !strncmp(k, "audio ", 6) ? BT_AUDIO : !strncmp(k, "cassa ", 6) ? BT_SPEAKER : !strncmp(k, "input ", 6) ? BT_INPUT : BT_OTHER;
        snprintf(t->name, sizeof t->name, "%s", sp ? sp + 1 : "");
        char *nl = strchr(t->name, '\n');
        if (nl) *nl = 0;
        if (addr_ok(t->addr)) t->state = BT_CONNECTED;
    }
}

static void *status_worker(void *arg) {
    (void)arg;
    bt_status_t t;
    status_read(&t);
    pthread_mutex_lock(&st_lock);
    st = t;
    pthread_mutex_unlock(&st_lock);
    __atomic_store_n(&st_busy, false, __ATOMIC_SEQ_CST);
    return NULL;
}

const bt_status_t *bt_status(void) {
    static bt_status_t view;
    double t = now_s();
    if (bt_supported() && t - st_time >= 3.0 && !__atomic_load_n(&st_busy, __ATOMIC_SEQ_CST)) {
        st_time = t;
        __atomic_store_n(&st_busy, true, __ATOMIC_SEQ_CST);
        pthread_t th;
        if (pthread_create(&th, NULL, status_worker, NULL) == 0) pthread_detach(th);
        else __atomic_store_n(&st_busy, false, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_lock(&st_lock);
    view = st;
    pthread_mutex_unlock(&st_lock);
    return &view;
}

static int scan_secs;
static void *scan_worker(void *arg) {
    (void)arg;
    char s[8];
    snprintf(s, sizeof s, "%d", scan_secs);
    ctl(NULL, 0, "scan", s, NULL);
    __atomic_store_n(&scanning, false, __ATOMIC_SEQ_CST);
    return NULL;
}

void bt_scan_start(int secs) {
    if (__atomic_load_n(&scanning, __ATOMIC_SEQ_CST)) return;
    scan_secs = secs;
    __atomic_store_n(&scanning, true, __ATOMIC_SEQ_CST);
    pthread_t th;
    if (pthread_create(&th, NULL, scan_worker, NULL) == 0) pthread_detach(th);
    else __atomic_store_n(&scanning, false, __ATOMIC_SEQ_CST);
}

/* righe "INDIRIZZO\ttipo\tabbinato\tcollegato\tnome" */
int bt_devices(bt_dev_t *out, int max) {
    static char buf[16384];
    if (!ctl(buf, sizeof buf, "devices", NULL)) return 0;
    int n = 0;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        char *f[5];
        int k = 0;
        for (char *p = l; k < 5; k++) {
            f[k] = p;
            char *tab = k < 4 ? strchr(p, '\t') : NULL;
            if (k < 4 && !tab) break;
            if (tab) { *tab = 0; p = tab + 1; }
        }
        if (k < 5 || !addr_ok(f[0])) continue;
        bt_dev_t *d = &out[n++];
        memset(d, 0, sizeof *d);
        snprintf(d->addr, sizeof d->addr, "%s", f[0]);
        d->kind = !strcmp(f[1], "audio") ? BT_AUDIO : !strcmp(f[1], "cassa") ? BT_SPEAKER : !strcmp(f[1], "input") ? BT_INPUT : BT_OTHER;
        d->paired = f[2][0] == '1';
        d->connected = f[3][0] == '1';
        snprintf(d->name, sizeof d->name, "%s", f[4]);
    }
    qsort(out, n, sizeof *out, cmp_dev);
    return n;
}

static bool act(const char *cmd, const char *addr) {
    if (!addr_ok(addr)) return false;
    bool ok = ctl(NULL, 0, cmd, addr, NULL);
    st_time = -10;
    return ok;
}

#elif defined(R36S_DEVICE)
/* ------------------------------------------------------------ R36S: niente Bluetooth */
bool bt_supported(void) { return false; }
void bt_enable(bool on) { (void)on; (void)want_on; (void)enabled_at; }
const bt_status_t *bt_status(void) { (void)st_busy; (void)st_time; (void)st_lock; return &st; }
void bt_scan_start(int secs) { (void)secs; }
int bt_devices(bt_dev_t *out, int max) { (void)out; (void)max; (void)cmp_dev; return 0; }
static bool act(const char *cmd, const char *addr) { (void)cmd; return addr_ok(addr) && false; }

#else
/* ------------------------------------------------------------ Mac: dispositivi simulati */
static struct { const char *addr, *name; int kind; double at; bool paired, connected; } sim[] = {
    { "02:00:00:00:00:01", "Wireless Earbuds", BT_AUDIO, 6, false, false },
    { "11:22:33:44:55:66", "8BitDo Pro 2", BT_INPUT, 3, false, false },
    { "02:00:00:00:00:03", "JBL Flip 5", BT_SPEAKER, 4, false, false },
    { "02:00:00:00:00:04", "Sensore BLE", BT_OTHER, 1, false, false },
    { "02:00:00:00:00:02", "Cuffie da corsa", BT_AUDIO, 0, true, false },
};
#define NSIM ((int)(sizeof sim / sizeof *sim))
static bool sim_on;
static double scan_t0 = -100, scan_end = -100;

bool bt_supported(void) { return true; }
void bt_enable(bool on) {
    sim_on = on; want_on = on; enabled_at = now_s();
    if (!on) for (int i = 0; i < NSIM; i++) sim[i].connected = false;
}

static void status_read(bt_status_t *t) {
    memset(t, 0, sizeof *t);
    if (!sim_on) return;
    if (now_s() - enabled_at < 3) { t->state = BT_STARTING; return; }
    t->state = BT_ON;
    for (int i = 0; i < NSIM; i++)
        if (sim[i].connected) {
            t->state = BT_CONNECTED;
            t->kind = sim[i].kind;
            snprintf(t->addr, sizeof t->addr, "%s", sim[i].addr);
            snprintf(t->name, sizeof t->name, "%s", sim[i].name);
        }
}

const bt_status_t *bt_status(void) {
    pthread_mutex_lock(&st_lock);
    status_read(&st);
    pthread_mutex_unlock(&st_lock);
    (void)st_busy; (void)st_time;
    return &st;
}

void bt_scan_start(int secs) { if (scan_t0 < 0) scan_t0 = now_s(); scan_end = now_s() + secs; }
static bool bt_scanning_sim(void) { return now_s() < scan_end; }

int bt_devices(bt_dev_t *out, int max) {
    if (!sim_on) return 0;
    int n = 0;
    for (int i = 0; i < NSIM && n < max; i++) {
        if (!sim[i].paired && now_s() - scan_t0 < sim[i].at) continue;
        if (!sim[i].paired && scan_t0 < 0) continue;
        bt_dev_t *d = &out[n++];
        memset(d, 0, sizeof *d);
        snprintf(d->addr, sizeof d->addr, "%s", sim[i].addr);
        snprintf(d->name, sizeof d->name, "%s", sim[i].name);
        d->kind = sim[i].kind;
        d->paired = sim[i].paired;
        d->connected = sim[i].connected;
    }
    qsort(out, n, sizeof *out, cmp_dev);
    return n;
}

static bool act(const char *cmd, const char *addr) {
    if (!addr_ok(addr)) return false;
    struct timespec ts = { 1, 500000000 };
    nanosleep(&ts, NULL);
    for (int i = 0; i < NSIM; i++) {
        if (strcmp(sim[i].addr, addr)) continue;
        if (!strcmp(cmd, "pair")) {
            if (!strcmp(sim[i].name, "JBL Flip 5")) return false;   /* per provare l'errore */
            for (int k = 0; k < NSIM; k++) sim[k].connected = false;
            sim[i].paired = sim[i].connected = true;
        } else if (!strcmp(cmd, "connect")) {
            for (int k = 0; k < NSIM; k++) sim[k].connected = false;
            sim[i].connected = true;
        } else if (!strcmp(cmd, "disconnect")) sim[i].connected = false;
        else if (!strcmp(cmd, "forget")) sim[i].paired = sim[i].connected = false;
        return true;
    }
    return false;
}
#endif

void bt_refresh(void) { st_time = -10; }

bool bt_scanning(void) {
#if defined(TRIMUI_DEVICE) || defined(R36S_DEVICE)
    return __atomic_load_n(&scanning, __ATOMIC_SEQ_CST);
#else
    (void)scanning;
    return bt_scanning_sim();
#endif
}

bool bt_pair(const char *addr) { return act("pair", addr); }
bool bt_connect(const char *addr) { return act("connect", addr); }
bool bt_disconnect(const char *addr) { return act("disconnect", addr); }
bool bt_forget(const char *addr) { return act("forget", addr); }
