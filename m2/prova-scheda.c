/* prova-scheda: che cosa succede togliendo la SD a console accesa (23 novembre, prima prova).
 *
 *   prova-scheda DISCO PARTIZIONE      per esempio: prova-scheda mmcblk1 /dev/mmcblk1p4
 *
 * Lo avvia system-init solo se c'è Arcade/System/prova-scheda.txt, dopo averlo copiato in memoria
 * (/run/prova-scheda, insieme a fbmsg): non legge niente dalla scheda finché la scheda non torna.
 * Ogni mezzo secondo guarda se il disco c'è (/sys/block/DISCO) e intanto raccoglie i messaggi del
 * kernel (/dev/kmsg). Quando la scheda sparisce: LED rossi lampeggianti e «SCHEDA TOLTA» con fbmsg.
 * Quando torna: LED verdi, monta da sé la partizione in /run/prova-scheda/mnt, aggiunge a
 * Arcade/Logs/prova-scheda.txt gli orari, i mount e i messaggi del kernel, la smonta e lo dice a
 * schermo. Serve solo a capire se il kernel si accorge della scheda tolta e rimessa e che cosa fanno
 * gli altri programmi; non ripara niente. Statico (m2/build-m2.sh). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIR_RAM "/run/prova-scheda"
#define NLINES 400
static char *lines[NLINES];   /* ultime righe: eventi e messaggi del kernel, in ordine */
static int nl, first;

static double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void add(const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char *s = strdup(buf);
    if (!s) return;
    int i = (first + nl) % NLINES;
    if (nl == NLINES) { free(lines[first]); lines[first] = s; first = (first + 1) % NLINES; }
    else { lines[i] = s; nl++; }
}

static void put(const char *path, const char *v) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    if (write(fd, v, strlen(v)) < 0) { /* LED assenti: niente */ }
    close(fd);
}

/* LED come platform.c (driver led_anim): effetto 6 lampeggio, 4 fisso */
static void leds(const char *rgb, int effect) {
    static const char *zone[] = { "lr", "m", "f1", "f2", "rear" };
    put("/sys/class/led_anim/effect_enable", "1");
    put("/sys/class/led_anim/anim_frames_enable", "0");
    for (unsigned i = 0; i < sizeof zone / sizeof *zone; i++) {
        char p[96], v[16];
        snprintf(p, sizeof p, "/sys/class/led_anim/effect_rgb_hex_%s", zone[i]); put(p, rgb);
        snprintf(p, sizeof p, "/sys/class/led_anim/effect_cycles_%s", zone[i]); put(p, "30000");
        snprintf(p, sizeof p, "/sys/class/led_anim/effect_duration_%s", zone[i]); put(p, effect == 6 ? "300" : "1000");
        snprintf(p, sizeof p, "/sys/class/led_anim/effect_%s", zone[i]); snprintf(v, sizeof v, "%d", effect); put(p, v);
    }
}

static void msg(const char *color, const char *a, const char *b) {
    pid_t p = fork();
    if (p == 0) {
        execl(DIR_RAM "/fbmsg", "fbmsg", "show", color, "0", a, b, (char *)NULL);
        _exit(127);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void kmsg_read(int fd) {
    char buf[1024];
    ssize_t n;
    while (fd >= 0 && (n = read(fd, buf, sizeof buf - 1)) > 0) {
        buf[n] = 0;
        char *t = strchr(buf, ';');   /* "livello,numero,microsecondi,-;testo" */
        char *e = strchr(t ? t : buf, '\n');
        if (e) *e = 0;
        long long us = 0;
        char *c1 = strchr(buf, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
        if (c2) us = atoll(c2 + 1);
        add("kernel %lld.%03lld: %s", us / 1000000, us / 1000 % 1000, t ? t + 1 : buf);
    }
}

/* la scheda c'è: il disco esiste e ha una dimensione (un loop staccato resta con dimensione 0) */
static bool disk_present(const char *sys) {
    char p[128];
    snprintf(p, sizeof p, "%s/size", sys);
    FILE *f = fopen(p, "r");
    long long size = 0;
    if (f) { if (fscanf(f, "%lld", &size) != 1) size = 0; fclose(f); }
    return size > 0;
}

static void copy_file(const char *path, const char *title) {
    FILE *f = fopen(path, "r");
    if (!f) { add("(%s: non leggibile)", title); return; }
    char l[512];
    add("--- %s", title);
    while (fgets(l, sizeof l, f)) { l[strcspn(l, "\n")] = 0; add("  %s", l); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "uso: prova-scheda DISCO PARTIZIONE\n"); return 2; }
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (chdir("/") != 0) return 1;
    char sys[96];
    snprintf(sys, sizeof sys, "/sys/block/%s", argv[1]);
    int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kfd >= 0) lseek(kfd, 0, SEEK_END);   /* solo i messaggi da qui in poi */
    time_t t0 = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t0));
    double m0 = mono();
    add("=== prova-scheda avviata %s, disco %s, partizione %s", when, argv[1], argv[2]);
    bool present = disk_present(sys), logged_once = false;
    add("%.1f s: scheda %s", 0.0, present ? "presente" : "ASSENTE");
    for (;;) {
        kmsg_read(kfd);
        bool now_present = disk_present(sys);
        if (now_present != present) {
            present = now_present;
            add("%.1f s: scheda %s", mono() - m0, present ? "RIMESSA" : "TOLTA");
            if (!present) {
                leds("FF0000", 6);
                msg("rosso", "SCHEDA TOLTA", "RIMETTILA PER SCRIVERE IL REGISTRO");
            } else {
                leds("00FF00", 6);
                msg("blu", "SCHEDA RIMESSA", "ASPETTO LA PARTIZIONE");
                /* la partizione compare in /dev poco dopo il disco */
                for (int i = 0; i < 40 && access(argv[2], F_OK) != 0; i++) { usleep(250000); kmsg_read(kfd); }
                add("%.1f s: partizione %s %s", mono() - m0, argv[2], access(argv[2], F_OK) == 0 ? "c'è" : "NON c'è");
                copy_file("/proc/mounts", "mount con la scheda rimessa");
                mkdir(DIR_RAM "/mnt", 0755);
                int r = mount(argv[2], DIR_RAM "/mnt", "vfat", 0, NULL);
                add("%.1f s: mount di prova: %s", mono() - m0, r == 0 ? "riuscito" : strerror(errno));
                if (r == 0) {
                    kmsg_read(kfd);
                    FILE *f = fopen(DIR_RAM "/mnt/Arcade/Logs/prova-scheda.txt", "a");
                    if (f) {
                        for (int i = 0; i < nl; i++) fprintf(f, "%s\n", lines[(first + i) % NLINES]);
                        fprintf(f, "\n");
                        fflush(f);
                        fsync(fileno(f));
                        fclose(f);
                    }
                    sync();
                    bool ok = f != NULL && umount(DIR_RAM "/mnt") == 0;
                    logged_once = f != NULL;
                    leds("00FF00", 4);
                    msg(ok ? "blu" : "rosso", f ? "REGISTRO SCRITTO" : "REGISTRO NON SCRITTO",
                        "TIENI PREMUTO ACCENSIONE PER SPEGNERE");
                } else {
                    leds("FFA000", 4);
                    msg("rosso", "SCHEDA RIMESSA MA NON MONTATA", "TIENI PREMUTO ACCENSIONE PER SPEGNERE");
                }
            }
        }
        (void)logged_once;
        usleep(500000);
    }
}
