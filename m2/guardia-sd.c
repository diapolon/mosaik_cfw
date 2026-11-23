/* guardia-sd: SD tolta a console accesa (23 novembre, primo passo della «Rimuovi la scheda»).
 *
 *   guardia-sd DISCO [SECONDI]        per esempio: guardia-sd mmcblk0 60
 *
 * Tutto Mosaik gira da file sulla SD: togliendola, lo schermo resta fermo e la console accesa a
 * consumare. system-init copia questo programma in memoria (/run/guardia, con fbmsg e axpctl,
 * statici) e lo avvia da lì, così continua anche senza scheda. Ogni mezzo secondo guarda se il
 * disco c'è (dimensione maggiore di zero, come m2/prova-scheda.c: la prova del 23 novembre ha
 * mostrato che il kernel vede la scheda sparire e tornare).
 * - Scheda sparita per più di un secondo: LED rossi lampeggianti e «SCHEDA TOLTA».
 * - Rimessa: la console si riavvia e riparte pulita (il sistema e la cartella Arcade restano
 *   agganciati alla scheda tolta); col caricatore collegato si spegne con axpctl, perché dopo un
 *   riavvio U-Boot resterebbe a schermo nero (issue #2).
 * - Non rimessa entro SECONDI (60): si spegne con axpctl.
 * Nelle prove GUARDIA_TEST=file scrive in quel file «riavvio» o «spegnimento» invece di farlo,
 * GUARDIA_USB è il file del caricatore e GUARDIA_LED la cartella dei LED (m2/test-guardia-sd.sh). */
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIR_RAM "/run/guardia"

static double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static bool disk_present(const char *disk) {
    char p[128];
    snprintf(p, sizeof p, "/sys/block/%s/size", disk);
    FILE *f = fopen(p, "r");
    long long size = 0;
    if (f) { if (fscanf(f, "%lld", &size) != 1) size = 0; fclose(f); }
    return size > 0;
}

static int read_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}

static void put(const char *path, const char *v) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    if (write(fd, v, strlen(v)) < 0) { /* LED assenti */ }
    close(fd);
}

static void leds(const char *rgb, int effect) {
    const char *dir = getenv("GUARDIA_LED") ? getenv("GUARDIA_LED") : "/sys/class/led_anim";
    static const char *zone[] = { "lr", "m", "f1", "f2", "rear" };
    char p[160], v[16];
    snprintf(p, sizeof p, "%s/effect_enable", dir); put(p, "1");
    snprintf(p, sizeof p, "%s/anim_frames_enable", dir); put(p, "0");
    for (unsigned i = 0; i < sizeof zone / sizeof *zone; i++) {
        snprintf(p, sizeof p, "%s/effect_rgb_hex_%s", dir, zone[i]); put(p, rgb);
        snprintf(p, sizeof p, "%s/effect_cycles_%s", dir, zone[i]); put(p, "30000");
        snprintf(p, sizeof p, "%s/effect_duration_%s", dir, zone[i]); put(p, "300");
        snprintf(p, sizeof p, "%s/effect_%s", dir, zone[i]); snprintf(v, sizeof v, "%d", effect); put(p, v);
    }
}

static void run(const char *prog, const char *a1, const char *a2, const char *a3, const char *a4, const char *a5) {
    pid_t p = fork();
    if (p == 0) { execl(prog, prog, a1, a2, a3, a4, a5, (char *)NULL); _exit(127); }
    if (p > 0) waitpid(p, NULL, 0);
}

static void msg(const char *a, const char *b) {
    if (!getenv("GUARDIA_TEST")) run(DIR_RAM "/fbmsg", "show", "rosso", "0", a, b);
}

/* fine: riavvio, o spegnimento col chip di alimentazione (anche al posto del riavvio col caricatore) */
static void finish(bool reboot_wanted) {
    const char *usb = getenv("GUARDIA_USB") ? getenv("GUARDIA_USB") : "/sys/class/power_supply/axp2202-usb/online";
    bool off = !reboot_wanted || read_int(usb) == 1;
    const char *test = getenv("GUARDIA_TEST");
    if (test) {
        FILE *f = fopen(test, "w");
        if (f) { fputs(off ? "spegnimento\n" : "riavvio\n", f); fclose(f); }
        exit(0);
    }
    msg(off ? "SPENGO" : "RIAVVIO", off ? "RIACCENDI CON IL TASTO DI ACCENSIONE" : "LA SCHEDA E' DI NUOVO AL SUO POSTO");
    sleep(2);
    if (off) {
        run(DIR_RAM "/axpctl", "poweroff", NULL, NULL, NULL, NULL);
        reboot(RB_POWER_OFF);   /* se axpctl non ce la fa */
    }
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: guardia-sd DISCO [SECONDI]\n"); return 2; }
    int wait_s = argc > 2 ? atoi(argv[2]) : 60;
    if (wait_s < 5) wait_s = 5;
    if (chdir("/") != 0) return 1;
    int absent = 0;
    double gone_at = 0;
    for (;;) {
        bool present = disk_present(argv[1]);
        if (!present) {
            absent++;
            if (absent == 2) {   /* un secondo intero senza disco: tolta davvero */
                gone_at = mono();
                leds("FF0000", 6);
                msg("SCHEDA TOLTA", "RIMETTILA: LA CONSOLE RIPARTE");
            }
            if (absent >= 2 && mono() - gone_at > wait_s) finish(false);
        } else {
            if (absent >= 2) finish(true);   /* rimessa */
            absent = 0;
        }
        usleep(500000);
    }
}
