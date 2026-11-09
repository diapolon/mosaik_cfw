#include "wifi.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

__attribute__((unused)) static int bars_from_dbm(int dbm) { return dbm >= -60 ? 3 : dbm >= -70 ? 2 : dbm >= -80 ? 1 : 0; }

static int cmp_net(const void *a, const void *b) {
    const wifi_net_t *x = a, *y = b;
    if (x->current != y->current) return y->current - x->current;
    if (x->saved != y->saved) return y->saved - x->saved;
    if (x->bars != y->bars) return y->bars - x->bars;
    return strcmp(x->ssid, y->ssid);
}

static wifi_status_t st;
static double st_time = -10;

#ifdef TRIMUI_DEVICE
/* ------------------------------------------------------------ console: wpa_cli */
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

bool wifi_supported(void) { return plat_standalone(); }

void wifi_enable(bool on) {
    /* il servizio carica il driver e può metterci qualche secondo: in sottofondo */
    int r = system(on ? "touch /run/wifi-touched; wifi-ctl on > /dev/null 2>&1 &"
                      : "touch /run/wifi-touched; wifi-ctl off > /dev/null 2>&1 &");
    (void)r;
    st_time = -10;
}

/* wpa_cli con i suoi argomenti (niente shell: nomi e password passano così come sono) */
static bool wpa(char *out, size_t n, ...) {
    const char *argv[16] = { "wpa_cli", "-p", "/run/wpa", "-i", "wlan0" };
    int argc = 5;
    va_list ap;
    va_start(ap, n);
    const char *a;
    while (argc < 15 && (a = va_arg(ap, const char *))) argv[argc++] = a;
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
    int rc = posix_spawnp(&pid, "wpa_cli", &fa, NULL, (char *const *)argv, environ);
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

static bool wpa_ok(const char *out) { return !strncmp(out, "OK", 2); }

/* wpa_cli scrive i nomi con le sequenze di printf_encode (\xNN, \", \\ ...) */
static void ssid_decode(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p != '\\' || !p[1]) { out[o++] = *p; continue; }
        p++;
        switch (*p) {
        case 'x':
            if (p[1] && p[2]) { char h[3] = { p[1], p[2], 0 }; out[o++] = (char)strtol(h, NULL, 16); p += 2; }
            break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'e': out[o++] = 27; break;
        default: out[o++] = *p;
        }
    }
    out[o] = 0;
}

static const char *field(const char *text, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = text; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '\n');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return out;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

/* reti salvate: id, nome, stato (lista "network id / ssid / bssid / flags") */
typedef struct { int id; char ssid[33]; bool current, failed; } saved_t;
static int saved_list(saved_t *out, int max) {
    char buf[4096];
    if (!wpa(buf, sizeof buf, "list_networks", NULL)) return 0;
    int n = 0;
    char *save = NULL;
    strtok_r(buf, "\n", &save);   /* intestazione */
    for (char *l = strtok_r(NULL, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        char *f[4] = { 0 };
        int k = 0;
        for (char *c = l; k < 4; k++) {
            f[k] = c;
            c = strchr(c, '\t');
            if (!c) break;
            *c++ = 0;
        }
        if (!f[1]) continue;
        out[n].id = atoi(f[0]);
        ssid_decode(f[1], out[n].ssid, sizeof out[n].ssid);
        out[n].current = f[3] && strstr(f[3], "[CURRENT]");
        out[n].failed = f[3] && strstr(f[3], "DISABLED");
        n++;
    }
    return n;
}

static bool pending;   /* rete scelta da poco: a connessione riuscita si salva la configurazione */

/* legge lo stato da wpa_cli: può metterci decine di millisecondi, di più se il driver è occupato */
static void status_read(wifi_status_t *o) {
    char buf[2048], v[64];
    memset(o, 0, sizeof *o);
    if (!wpa(buf, sizeof buf, "status", NULL) || !field(buf, "wpa_state", v, sizeof v)) { o->state = WIFI_OFF; return; }
    char raw[160];
    if (field(buf, "ssid", raw, sizeof raw)) ssid_decode(raw, o->ssid, sizeof o->ssid);
    field(buf, "ip_address", o->ip, sizeof o->ip);
    if (!strcmp(v, "COMPLETED")) o->state = o->ip[0] ? WIFI_CONNECTED : WIFI_CONNECTING;
    else if (strstr(v, "ASSOC") || strstr(v, "AUTH") || strstr(v, "HANDSHAKE")) o->state = WIFI_CONNECTING;
    else o->state = WIFI_DISCONNECTED;
    if (o->state == WIFI_CONNECTED) {
        if (wpa(buf, sizeof buf, "signal_poll", NULL) && field(buf, "RSSI", v, sizeof v)) o->bars = bars_from_dbm(atoi(v));
        if (__atomic_exchange_n(&pending, false, __ATOMIC_SEQ_CST)) {
            /* select_network disattiva le altre reti salvate: si riattivano e si salva */
            wpa(buf, sizeof buf, "enable_network", "all", NULL);
            wpa(buf, sizeof buf, "save_config", NULL);
        }
    }
}

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static bool st_busy;

static void *status_worker(void *arg) {
    (void)arg;
    wifi_status_t t;
    status_read(&t);
    pthread_mutex_lock(&st_lock);
    st = t;
    pthread_mutex_unlock(&st_lock);
    __atomic_store_n(&st_busy, false, __ATOMIC_SEQ_CST);
    return NULL;
}

/* Senza refresh non blocca mai: restituisce l'ultimo stato e lo aggiorna in un thread. */
const wifi_status_t *wifi_status(bool refresh) {
    static wifi_status_t view;
    double t = now_s();
    if (refresh) {
        wifi_status_t r;
        status_read(&r);
        st_time = t;
        pthread_mutex_lock(&st_lock);
        st = r;
        pthread_mutex_unlock(&st_lock);
    } else if (t - st_time >= 2.0 && !__atomic_load_n(&st_busy, __ATOMIC_SEQ_CST)) {
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

/* Nei primi 30 secondi dalla accensione il driver sta ancora partendo e fa già la sua prima
   ricerca: non gliene si chiede un'altra, l'elenco si riempie comunque con i suoi risultati. */
static bool just_booted(void) {
    double up = 1e9;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 1e9; fclose(f); }
    return up < 30;
}

void wifi_scan(void) {
    if (just_booted()) return;
    char b[64];
    wpa(b, sizeof b, "scan", NULL);
}

int wifi_networks(wifi_net_t *out, int max) {
    static char buf[16384];
    if (!wpa(buf, sizeof buf, "scan_results", NULL)) return 0;
    saved_t sv[32];
    int nsv = saved_list(sv, 32), n = 0;
    char *save = NULL;
    strtok_r(buf, "\n", &save);   /* intestazione: bssid / frequency / signal level / flags / ssid */
    for (char *l = strtok_r(NULL, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *f[5] = { 0 };
        int k = 0;
        for (char *c = l; k < 5; k++) {
            f[k] = c;
            c = strchr(c, '\t');
            if (!c) break;
            *c++ = 0;
        }
        if (!f[4] || !f[4][0]) continue;   /* reti nascoste */
        char name[33];
        ssid_decode(f[4], name, sizeof name);
        if (!name[0]) continue;
        int bars = bars_from_dbm(atoi(f[2]));
        bool sec = strstr(f[3], "WPA") || strstr(f[3], "WEP") || strstr(f[3], "SAE") || strstr(f[3], "RSN");
        int j;
        for (j = 0; j < n; j++) if (!strcmp(out[j].ssid, name)) break;
        if (j < n) { if (bars > out[j].bars) out[j].bars = bars; continue; }   /* più antenne, stesso nome */
        if (n >= max) continue;
        wifi_net_t *w = &out[n++];
        memset(w, 0, sizeof *w);
        snprintf(w->ssid, sizeof w->ssid, "%s", name);
        w->bars = bars;
        w->secured = sec;
        for (int s = 0; s < nsv; s++)
            if (!strcmp(sv[s].ssid, name)) { w->saved = true; w->current = sv[s].current && st.state == WIFI_CONNECTED; w->failed = sv[s].failed; }
    }
    qsort(out, n, sizeof *out, cmp_net);
    return n;
}

bool wifi_connect(const char *ssid, const char *psk) {
    char buf[256], id[16] = "", hex[80] = "", q[160];
    saved_t sv[32];
    int nsv = saved_list(sv, 32);
    for (int s = 0; s < nsv; s++) if (!strcmp(sv[s].ssid, ssid)) snprintf(id, sizeof id, "%d", sv[s].id);
    if (!id[0]) {
        if (!wpa(buf, sizeof buf, "add_network", NULL)) return false;
        snprintf(id, sizeof id, "%d", atoi(buf));
        /* il nome in esadecimale: va bene qualunque carattere */
        for (size_t i = 0; ssid[i] && i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", (unsigned char)ssid[i]);
        if (!wpa(buf, sizeof buf, "set_network", id, "ssid", hex, NULL) || !wpa_ok(buf)) return false;
        if (!psk && !wpa(buf, sizeof buf, "set_network", id, "key_mgmt", "NONE", NULL)) return false;
    }
    if (psk) {
        snprintf(q, sizeof q, "\"%s\"", psk);
        if (!wpa(buf, sizeof buf, "set_network", id, "psk", q, NULL) || !wpa_ok(buf)) return false;
    }
    wpa(buf, sizeof buf, "enable_network", id, NULL);
    if (!wpa(buf, sizeof buf, "select_network", id, NULL) || !wpa_ok(buf)) return false;
    pending = true;
    st_time = -10;
    return true;
}

bool wifi_forget(const char *ssid) {
    char buf[64];
    saved_t sv[32];
    int nsv = saved_list(sv, 32);
    bool ok = false;
    for (int s = 0; s < nsv; s++) {
        if (strcmp(sv[s].ssid, ssid)) continue;
        char id[16];
        snprintf(id, sizeof id, "%d", sv[s].id);
        ok = wpa(buf, sizeof buf, "remove_network", id, NULL) && wpa_ok(buf);
    }
    if (ok) wpa(buf, sizeof buf, "save_config", NULL);
    st_time = -10;
    return ok;
}

#else
/* ------------------------------------------------------------ Mac: reti simulate */
static struct { const char *ssid; int bars; bool secured, saved; } sim[] = {
    { "Casa", 3, true, true }, { "Vodafone-A1B2C3", 2, true, false }, { "TIM-87654321", 2, true, false },
    { "Ospiti", 1, false, false }, { "Bar Centrale WiFi", 1, true, false }, { "FASTWEB-1F2E3D", 0, true, false },
};
static bool sim_on = true, sim_failed;
static int sim_cur = 0;           /* rete collegata o in collegamento, -1 nessuna */
static double sim_t0 = -100;      /* inizio del collegamento */

bool wifi_supported(void) { return true; }
void wifi_enable(bool on) { sim_on = on; sim_t0 = now_s(); st_time = -10; }

const wifi_status_t *wifi_status(bool refresh) {
    (void)refresh;
    memset(&st, 0, sizeof st);
    double t = now_s() - sim_t0;
    if (!sim_on || t < 1.5) { st.state = WIFI_OFF; return &st; }
    if (sim_cur < 0 || sim_failed) { st.state = WIFI_DISCONNECTED; return &st; }
    snprintf(st.ssid, sizeof st.ssid, "%s", sim[sim_cur].ssid);
    if (t < 3.5) { st.state = WIFI_CONNECTING; return &st; }
    st.state = WIFI_CONNECTED;
    snprintf(st.ip, sizeof st.ip, "192.168.1.42");
    st.bars = sim[sim_cur].bars;
    return &st;
}

void wifi_scan(void) {}

int wifi_networks(wifi_net_t *out, int max) {
    if (!sim_on) return 0;
    int n = 0;
    for (int i = 0; i < (int)(sizeof sim / sizeof *sim) && n < max; i++) {
        wifi_net_t *w = &out[n++];
        memset(w, 0, sizeof *w);
        snprintf(w->ssid, sizeof w->ssid, "%s", sim[i].ssid);
        w->bars = sim[i].bars;
        w->secured = sim[i].secured;
        w->saved = sim[i].saved;
        w->current = i == sim_cur && !sim_failed && wifi_status(false)->state == WIFI_CONNECTED;
        w->failed = i == sim_cur && sim_failed;
    }
    qsort(out, n, sizeof *out, cmp_net);
    return n;
}

bool wifi_connect(const char *ssid, const char *psk) {
    for (int i = 0; i < (int)(sizeof sim / sizeof *sim); i++) {
        if (strcmp(sim[i].ssid, ssid)) continue;
        sim_cur = i;
        sim_failed = psk && !strcmp(psk, "sbagliata");
        if (!sim_failed) sim[i].saved = true;
        sim_t0 = now_s() - 1.5;
        return true;
    }
    return false;
}

bool wifi_forget(const char *ssid) {
    for (int i = 0; i < (int)(sizeof sim / sizeof *sim); i++)
        if (!strcmp(sim[i].ssid, ssid)) { sim[i].saved = false; if (sim_cur == i) sim_cur = -1; return true; }
    return false;
}
#endif
