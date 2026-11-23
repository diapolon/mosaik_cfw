/* guardia-sd: SD tolta e rimessa a console accesa (23 novembre).
 *
 *   guardia-sd DISCO PARTIZIONE [SECONDI]     per esempio: guardia-sd mmcblk0 /dev/mmcblk0p4 60
 *
 * Il sistema gira in memoria (m2/ramroot.c): togliendo la scheda launcher e impostazioni restano
 * vivi, ma la cartella Arcade (/mnt/SDCARD) è agganciata alla scheda tolta. system-init copia questo
 * programma in memoria (/run/guardia, con fbmsg e axpctl, statici) e lo avvia da lì. Ogni mezzo
 * secondo guarda se il disco c'è (dimensione maggiore di zero: la prova del 23 novembre ha mostrato
 * che il kernel vede la scheda sparire e tornare con lo stesso nome).
 * - Scheda sparita per più di un secondo: LED rossi lampeggianti e /run/scheda-tolta, una riga
 *   «SCADENZA AZIONE» (SCADENZA in secondi di CLOCK_MONOTONIC, AZIONE spegni o sospendi): il
 *   launcher mostra la finestra con il conto alla rovescia. Secondi e azione sono quelli che il
 *   launcher ha scritto in /run/scheda-impostazioni (la scheda non si può più leggere), altrimenti
 *   SECONDI (60) e spegni.
 * - Alla scadenza: spegni = spegnimento con axpctl; sospendi = la sospende il launcher, e se dopo
 *   RISERVA secondi (30) nessuno l'ha fatto si spegne. Dopo un risveglio con la scheda ancora fuori
 *   il conto riparte. /run/scheda-spegni-ora (pulsante della finestra) spegne subito.
 * - Rimessa: AZIONE diventa «rimonto»; se è la stessa scheda (numero di serie del FAT letto
 *   all'avvio) fsck.fat -a la ripara, il vecchio aggancio si stacca (MNT_DETACH: i file rimasti
 *   aperti restano sulla scheda tolta) e la partizione si rimonta come nell'initramfs (noatime,
 *   flush). Poi /run/scheda-tolta sparisce e il contatore /run/scheda-rimessa cresce: il launcher
 *   riapre i suoi file e riprende da dov'era. Una scheda diversa o un passo non riuscito: riavvio
 *   (col caricatore collegato spegnimento, perché dopo un riavvio U-Boot resterebbe a schermo nero,
 *   issue #2). Com'è andata finisce in Arcade/Logs/scheda-tolta.txt.
 * Nelle prove: GUARDIA_TEST=file scrive «riavvio» o «spegnimento» invece di farlo, GUARDIA_RUN e
 * GUARDIA_MNT al posto di /run e /mnt/SDCARD, GUARDIA_USB il file del caricatore, GUARDIA_LED la
 * cartella dei LED, GUARDIA_RISERVA i secondi di riserva (m2/test-guardia-sd.sh). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIR_RAM "/run/guardia"

static const char *run_dir, *mnt;
static char log_buf[4096];
static size_t log_len;

static double clock_s(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static double mono(void) { return clock_s(CLOCK_MONOTONIC); }
/* tempo passato in sospensione: CLOCK_MONOTONIC si ferma, CLOCK_BOOTTIME no */
static double slept(void) { return clock_s(CLOCK_BOOTTIME) - clock_s(CLOCK_MONOTONIC); }

static void note(const char *fmt, ...) {
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    int n = snprintf(log_buf + log_len, sizeof log_buf - log_len, "%s %s\n", when, line);
    if (n > 0 && log_len + n < sizeof log_buf) log_len += n;
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

static void run_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", run_dir, name); }

static void put(const char *path, const char *v) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    if (write(fd, v, strlen(v)) < 0) { /* LED assenti */ }
    close(fd);
}

/* file in /run scritto per intero e poi rinominato: il launcher non legge mai mezza riga */
static void put_state(const char *name, const char *text) {
    char p[256], tmp[260];
    run_path(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.n", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
    rename(tmp, p);
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

static int run(const char *prog, char *const argv[], int timeout_s) {
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execvp(prog, argv);
        _exit(127);
    }
    if (p < 0) return -1;
    for (int i = 0; i < timeout_s * 10; i++) {
        int st;
        if (waitpid(p, &st, WNOHANG) == p) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        usleep(100000);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    return -2;   /* non ha finito in tempo */
}

static void msg(const char *a, const char *b) {
    if (getenv("GUARDIA_TEST")) return;
    char *argv[] = { "fbmsg", "show", "rosso", "0", (char *)a, (char *)b, NULL };
    run(DIR_RAM "/fbmsg", argv, 10);
}

/* fine: riavvio, o spegnimento col chip di alimentazione (anche al posto del riavvio col caricatore) */
static void finish(bool reboot_wanted, const char *why) {
    const char *usb = getenv("GUARDIA_USB") ? getenv("GUARDIA_USB") : "/sys/class/power_supply/axp2202-usb/online";
    bool off = !reboot_wanted || read_int(usb) == 1;
    const char *test = getenv("GUARDIA_TEST");
    if (test) {
        FILE *f = fopen(test, "w");
        if (f) { fprintf(f, "%s\n%s\n", off ? "spegnimento" : "riavvio", why); fclose(f); }
        exit(0);
    }
    msg(off ? "SPENGO" : "RIAVVIO", why);
    sleep(2);
    sync();
    if (off) {
        char *argv[] = { "axpctl", "poweroff", NULL };
        run(DIR_RAM "/axpctl", argv, 10);
        reboot(RB_POWER_OFF);   /* se axpctl non ce la fa */
    }
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

/* numero di serie del FAT (quello che mostra blkid): stessa scheda o un'altra */
static bool fat_serial(const char *dev, uint32_t *serial) {
    unsigned char b[512];
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    if (n != (ssize_t)sizeof b || b[510] != 0x55 || b[511] != 0xAA) return false;
    int off = !memcmp(b + 0x52, "FAT32", 5) ? 0x43 : !memcmp(b + 0x36, "FAT", 3) ? 0x27 : -1;
    if (off < 0) return false;
    *serial = b[off] | b[off + 1] << 8 | b[off + 2] << 16 | (uint32_t)b[off + 3] << 24;
    return true;
}

/* scheda rimessa: stessa scheda, riparata e rimontata; falso = va riavviato */
static bool remount(const char *part, bool have_serial, uint32_t serial) {
    if (!have_serial) { note("numero di serie della scheda sconosciuto dall'avvio"); return false; }
    uint32_t now = 0;
    bool got = false;
    for (int i = 0; i < 60 && !(got = fat_serial(part, &now)); i++) usleep(250000);   /* la partizione arriva poco dopo il disco */
    if (!got) { note("partizione %s non leggibile", part); return false; }
    if (now != serial) { note("scheda diversa (FAT %08X, all'avvio %08X)", now, serial); return false; }
    const char *fsck = getenv("GUARDIA_FSCK") ? getenv("GUARDIA_FSCK") : "fsck.fat";
    char *fa[] = { (char *)fsck, "-a", (char *)part, NULL };
    int r = run(fsck, fa, 120);
    note("fsck.fat -a: %s", r == 0 ? "nessun errore" : r == 1 ? "errori corretti" : r == 127 ? "non trovato" : "non riuscito");
    if (r != 0 && r != 1 && r != 127) return false;
    if (umount2(mnt, MNT_DETACH) != 0) note("vecchio aggancio non staccato: %s", strerror(errno));
    if (mount(part, mnt, "vfat", MS_NOATIME, "flush") != 0) { note("rimontaggio non riuscito: %s", strerror(errno)); return false; }
    return true;
}

static void save_log(void) {
    char p[512];
    snprintf(p, sizeof p, "%s/Arcade/Logs", mnt);
    if (access(p, F_OK) != 0) return;
    snprintf(p, sizeof p, "%s/Arcade/Logs/scheda-tolta.txt", mnt);
    FILE *f = fopen(p, "a");
    if (f) { fwrite(log_buf, 1, log_len, f); fflush(f); fsync(fileno(f)); fclose(f); }
    log_len = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: guardia-sd DISCO PARTIZIONE [SECONDI]\n"); return 2; }
    const char *disk = argv[1], *part = argv[2];
    int default_s = argc > 3 ? atoi(argv[3]) : 60;
    run_dir = getenv("GUARDIA_RUN") ? getenv("GUARDIA_RUN") : "/run";
    mnt = getenv("GUARDIA_MNT") ? getenv("GUARDIA_MNT") : "/mnt/SDCARD";
    int reserve = getenv("GUARDIA_RISERVA") ? atoi(getenv("GUARDIA_RISERVA")) : 30;
    if (chdir("/") != 0) return 1;
    uint32_t serial = 0;
    bool have_serial = fat_serial(part, &serial);
    char p_state[256], p_now[256], p_set[256];
    run_path(p_state, sizeof p_state, "scheda-tolta");
    run_path(p_now, sizeof p_now, "scheda-spegni-ora");
    run_path(p_set, sizeof p_set, "scheda-impostazioni");
    unlink(p_state);
    unlink(p_now);
    int absent = 0, count = 0, secs = default_s;
    char action[16] = "spegni", line[64];
    double deadline = 0, last_slept = slept();
    for (;;) {
        bool present = disk_present(disk);
        if (!present) {
            absent++;
            if (absent == 2) {   /* un secondo intero senza disco: tolta davvero */
                secs = default_s;
                snprintf(action, sizeof action, "spegni");
                FILE *f = fopen(p_set, "r");
                if (f) {
                    char a[16];
                    int s;
                    if (fscanf(f, "%d %15s", &s, a) == 2) { secs = s; snprintf(action, sizeof action, "%s", strcmp(a, "sospendi") ? "spegni" : "sospendi"); }
                    fclose(f);
                }
                if (secs < 5) secs = 5;
                deadline = mono() + secs;
                snprintf(line, sizeof line, "%.0f %s\n", deadline, action);
                put_state("scheda-tolta", line);
                leds("FF0000", 6);
                note("scheda tolta: %s fra %d s", action, secs);
            }
            if (absent >= 2) {
                if (slept() - last_slept > 2) {   /* risveglio con la scheda ancora fuori: il conto riparte */
                    deadline = mono() + secs;
                    snprintf(line, sizeof line, "%.0f %s\n", deadline, action);
                    put_state("scheda-tolta", line);
                    note("risveglio senza scheda: %s fra %d s", action, secs);
                }
                if (access(p_now, F_OK) == 0) { unlink(p_now); finish(false, "SPEGNIMENTO CHIESTO"); }
                double now = mono();
                if (!strcmp(action, "spegni") && now >= deadline) finish(false, "LA SCHEDA NON E' STATA RIMESSA");
                if (now >= deadline + reserve) finish(false, "LA SCHEDA NON E' STATA RIMESSA");
            }
        } else {
            if (absent >= 2) {   /* rimessa */
                snprintf(line, sizeof line, "%.0f rimonto\n", deadline);
                put_state("scheda-tolta", line);
                note("scheda rimessa");
                if (!remount(part, have_serial, serial)) {
                    unlink(p_state);
                    finish(true, "SCHEDA RIMESSA: RIPARTO");
                }
                note("rimontata in %s: si riprende", mnt);
                save_log();
                unlink(p_state);
                snprintf(line, sizeof line, "%d\n", ++count);
                put_state("scheda-rimessa", line);
            }
            absent = 0;
        }
        last_slept = slept();
        usleep(500000);
    }
}
