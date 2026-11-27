/* guardia-sd: SD tolta e rimessa a console accesa (23 novembre).
 *
 *   guardia-sd DISCO PARTIZIONE [SECONDI]     per esempio: guardia-sd mmcblk0 /dev/mmcblk0p4 60
 *
 * Il sistema gira in memoria (m2/ramroot.c): togliendo la scheda launcher e impostazioni restano
 * vivi, ma la cartella Arcade (/mnt/SDCARD) è agganciata alla scheda tolta. system-init copia questo
 * programma in memoria (/run/guardia, con fbmsg e axpctl, statici) e lo avvia da lì. Dieci volte al
 * secondo guarda se il disco c'è (dimensione maggiore di zero: il kernel vede la scheda sparire e
 * tornare con lo stesso nome) e il numero del dispositivo della partizione (sparita e tornata fra
 * due controlli = disco nuovo).
 * - Scheda sparita (basta un controllo: il vecchio aggancio è morto): LED rossi lampeggianti e
 *   /run/scheda-tolta, una riga «SCADENZA AZIONE» (SCADENZA in secondi di CLOCK_MONOTONIC, AZIONE
 *   spegni o sospendi): il launcher mostra la finestra con il conto alla rovescia. Secondi e azione
 *   sono quelli che il launcher ha scritto in /run/scheda-impostazioni (la scheda non si può più
 *   leggere), altrimenti SECONDI (60) e spegni.
 * - Alla scadenza: spegni = spegnimento con axpctl; sospendi = la sospende il launcher, e se dopo
 *   RISERVA secondi (30) nessuno l'ha fatto si spegne. Dopo un risveglio con la scheda ancora fuori
 *   il conto riparte. /run/scheda-spegni-ora (pulsante della finestra) spegne subito; una richiesta
 *   rimasta da prima si cancella a ogni estrazione.
 * - Rimessa: AZIONE diventa «rimonto»; se è la stessa scheda (numero di serie del FAT e CID della
 *   scheda letti all'avvio: il numero del FAT è lo stesso per tutte le schede scritte con la stessa
 *   immagine) e il sistema gira davvero in memoria, fsck.fat -a la ripara, il vecchio aggancio si
 *   stacca (MNT_DETACH: i file rimasti aperti restano sulla scheda tolta) e la partizione si
 *   rimonta come nell'initramfs (noatime, flush). Poi cresce il contatore /run/scheda-rimessa e
 *   sparisce /run/scheda-tolta: il launcher riapre i suoi file e riprende da dov'era. Tolta di
 *   nuovo durante il controllo, o infilata male (c'è ma non si legge): è ancora fuori, il conto
 *   alla rovescia continua e si riprova, mai un riavvio (senza scheda partirebbe il firmware interno). Una scheda diversa o un passo non
 *   riuscito: riavvio (col caricatore collegato spegnimento, perché dopo un riavvio U-Boot
 *   resterebbe a schermo nero, issue #2), con il segno lasciato perché nessuno riparta. Com'è
 *   andata finisce in Arcade/Logs/scheda-tolta.txt: dopo un passo non riuscito con la stessa
 *   scheda, riparata da fsck, la si aggancia da parte un momento e si aggiungono i messaggi del
 *   kernel.
 * Nelle prove (m2/test-guardia-sd.sh): GUARDIA_TEST=file scrive «riavvio» o «spegnimento» invece di
 * farlo, GUARDIA_RUN, GUARDIA_MNT e GUARDIA_SYS al posto di /run, /mnt/SDCARD e /sys/block,
 * GUARDIA_USB il file del caricatore, GUARDIA_LED la cartella dei LED, GUARDIA_RISERVA i secondi di
 * riserva, GUARDIA_FSCK il programma di controllo, GUARDIA_OPZIONI le opzioni del rimontaggio,
 * GUARDIA_RADICE il file con il percorso che legge il loop della radice. */
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
#include <sys/klog.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIR_RAM "/run/guardia"

static const char *run_dir, *mnt, *sysb;
static char log_buf[4096];
static size_t log_len;
static bool same_card;   /* la scheda rimessa ha numero di serie e CID di quella tolta */
static bool fsck_ok;     /* fsck.fat è passato: sulla scheda rimessa si può scrivere il registro */

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

/* 1 il disco c'è, 0 tolto (la sua cartella in /sys è sparita, o dimensione zero), -1 non si sa: una
   lettura non riuscita non è una scheda tolta, o si controllerebbe e rimonterebbe la scheda in uso
   (revisione della stabile, 27 novembre) */
static int disk_state(const char *disk) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s/size", sysb, disk);
    FILE *f = fopen(p, "r");
    if (!f) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    long long size = -1;
    if (fscanf(f, "%lld", &size) != 1) size = -1;
    fclose(f);
    return size > 0 ? 1 : size == 0 ? 0 : -1;
}
static bool disk_present(const char *disk) { return disk_state(disk) == 1; }

/* numero del dispositivo della partizione («179:4»), vuoto se non c'è ancora: una scheda sparita e
   tornata fra due controlli è per il kernel un disco nuovo, e il vecchio aggancio è morto comunque */
static void part_dev(const char *disk, const char *part, char *out, size_t n) {
    char p[400];
    const char *b = strrchr(part, '/');
    snprintf(p, sizeof p, "%s/%s/%s/dev", sysb, disk, b ? b + 1 : part);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

/* CID della scheda (registro del produttore, diverso per ogni scheda): il numero di serie del FAT
   è lo stesso per tutte le schede scritte con la stessa immagine. Vuoto se non si legge. */
static void card_cid(const char *disk, char *out, size_t n) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s/device/cid", sysb, disk);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

/* radice in memoria (m2/ramroot.c): se il loop della radice legge ancora il file sulla scheda
   tolta, rimontare non serve, il sistema cade alla prima pagina da rileggere. Vero anche quando
   la radice non è un loop squashfs (nelle prove). GUARDIA_RADICE: file con il percorso al posto
   di backing_file. */
static bool root_in_ram(char *why, size_t n) {
    char back[512] = "", p[300];
    const char *test = getenv("GUARDIA_RADICE");
    if (!test) {
        FILE *m = fopen("/proc/mounts", "r");
        char dev[128], mp[256], type[32], line[600];
        bool loop = false;
        while (m && fgets(line, sizeof line, m))
            if (sscanf(line, "%127s %255s %31s", dev, mp, type) == 3 && !strcmp(mp, "/") && !strcmp(type, "squashfs")) { loop = !strncmp(dev, "/dev/loop", 9); break; }
        if (m) fclose(m);
        if (!loop) return true;
        snprintf(p, sizeof p, "%s/%s/loop/backing_file", sysb, dev + 5);
        test = p;
    }
    FILE *f = fopen(test, "r");
    if (f) { if (!fgets(back, sizeof back, f)) back[0] = 0; fclose(f); }
    back[strcspn(back, "\r\n")] = 0;
    snprintf(why, n, "%s", back[0] ? back : "loop della radice illeggibile");
    return !strncmp(back, "/run/", 5);
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

/* memoria che fsck.fat chiederebbe (byte): la tabella e un puntatore per ogni cluster, più un margine;
   0 se il settore di avvio non si legge. Su una scheda da 512 GB sono centinaia di MB, mentre il
   gioco fermo tiene la sua (revisione della stabile, 27 novembre) */
static long long fsck_memory(const char *part) {
    unsigned char b[512];
    int fd = open(part, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    if (n != (ssize_t)sizeof b) return 0;
    unsigned bps = b[11] | b[12] << 8, spc = b[13], res = b[14] | b[15] << 8, nfats = b[16];
    unsigned long long tot = b[19] | b[20] << 8, fsz = b[22] | b[23] << 8;
    if (!tot) tot = b[32] | b[33] << 8 | b[34] << 16 | (unsigned long long)b[35] << 24;
    if (!fsz) fsz = b[36] | b[37] << 8 | b[38] << 16 | (unsigned long long)b[39] << 24;
    if (!bps || !spc || tot <= res + nfats * fsz) return 0;
    unsigned long long clusters = (tot - res - nfats * fsz) / spc;
    return (long long)(clusters * 16 + (16ULL << 20));
}
/* MemAvailable in byte (GUARDIA_MEMORIA_MB nelle prove), -1 se non si sa */
static long long mem_available(void) {
    if (getenv("GUARDIA_MEMORIA_MB")) return atoll(getenv("GUARDIA_MEMORIA_MB")) << 20;
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    long long kb = -1;
    while (f && fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %lld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb < 0 ? -1 : kb << 10;
}
/* controllo non fatto ora: lo fa l'initramfs al prossimo avvio (system-init rimette il segno «in
   uso» dopo aver chiuso la scheda, fat_segno_controllo) */
static void check_later(const char *part) {
    char line[300];
    snprintf(line, sizeof line, "%s\n", part);
    put_state("fat-da-controllare", line);
}

/* scheda rimessa: RM_OK stessa scheda, riparata e rimontata; RM_RIAVVIO va riavviato; RM_NON_LEGGE
   c'è ma non si legge (infilata male: il contatto non regge), è come ancora fuori */
enum { RM_OK, RM_RIAVVIO, RM_NON_LEGGE };
static int remount(const char *disk, const char *part, bool have_serial, uint32_t serial, const char *cid0) {
    fsck_ok = false;
    if (!have_serial) { note("numero di serie della scheda sconosciuto dall'avvio"); return RM_RIAVVIO; }
    uint32_t now = 0;
    bool got = false;
    for (int i = 0; i < 12 && !(got = fat_serial(part, &now)); i++) usleep(250000);   /* la partizione arriva poco dopo il disco */
    if (!got) return RM_NON_LEGGE;
    if (now != serial) { note("scheda diversa (FAT %08X, all'avvio %08X)", now, serial); return RM_RIAVVIO; }
    if (cid0[0]) {
        char cid[64];
        card_cid(disk, cid, sizeof cid);
        if (strcmp(cid, cid0)) { note("scheda diversa: stesso FAT %08X, CID %s (all'avvio %s)", now, cid[0] ? cid : "illeggibile", cid0); return RM_RIAVVIO; }
    }
    same_card = true;
    char why[512];
    if (!root_in_ram(why, sizeof why)) { note("il sistema legge ancora dalla scheda tolta (%s)", why); return RM_RIAVVIO; }
    const char *fsck = getenv("GUARDIA_FSCK") ? getenv("GUARDIA_FSCK") : "fsck.fat";
    char *fa[] = { (char *)fsck, "-a", (char *)part, NULL };
    long long need = fsck_memory(part), avail = mem_available();
    if (need > 0 && avail >= 0 && need > avail - (128LL << 20)) {
        note("controllo rimandato al prossimo avvio: servirebbero %lld MB, liberi %lld MB", need >> 20, avail >> 20);
        check_later(part);
    } else {
        int secs = getenv("GUARDIA_FSCK_SECS") ? atoi(getenv("GUARDIA_FSCK_SECS")) : 120;
        int r = run(fsck, fa, secs);
        /* fsck.fat -a (senza -w) scrive le correzioni solo alla fine: fermato prima, il FAT è com'era */
        if (r == -2 || r == -1) {
            note("fsck.fat -a: %s, il controllo lo rifà il prossimo avvio", r == -2 ? "tempo scaduto" : "fermato");
            check_later(part);
        } else {
            note("fsck.fat -a: %s", r == 0 ? "nessun errore" : r == 1 ? "errori corretti" : r == 127 ? "non trovato" : "non riuscito");
            if (r != 0 && r != 1 && r != 127) return RM_RIAVVIO;
            fsck_ok = true;
        }
    }
    const char *opts = getenv("GUARDIA_OPZIONI") ? getenv("GUARDIA_OPZIONI") : "flush";   /* nelle prove: un rimontaggio che non riesce */
    if (umount2(mnt, MNT_DETACH) != 0) note("vecchio aggancio non staccato: %s", strerror(errno));
    if (mount(part, mnt, "vfat", MS_NOATIME, opts) != 0) { note("rimontaggio non riuscito: %s", strerror(errno)); return RM_RIAVVIO; }
    return RM_OK;
}

/* ultime righe dei messaggi del kernel, per capire un rimontaggio non riuscito */
static void kernel_tail(FILE *f, int lines) {
    int size = klogctl(10, NULL, 0);   /* SYSLOG_ACTION_SIZE_BUFFER */
    if (size <= 0) size = 1 << 17;
    char *b = malloc(size + 1);
    if (!b) return;
    int n = klogctl(3, b, size);       /* SYSLOG_ACTION_READ_ALL */
    if (n > 0) {
        b[n] = 0;
        char *s = b + n;
        for (int k = 0; s > b && k <= lines; ) if (*--s == '\n') k++;
        if (*s == '\n') s++;
        fprintf(f, "messaggi del kernel (ultime %d righe):\n%s", lines, s);
        if (b[n - 1] != '\n') fputc('\n', f);
    }
    free(b);
}

/* registro in DIR/Arcade/Logs/scheda-tolta.txt; con il kernel dopo un rimontaggio non riuscito */
static bool save_log_in(const char *dir, bool kernel) {
    char p[512];
    snprintf(p, sizeof p, "%s/Arcade/Logs", dir);
    if (access(p, F_OK) != 0) return false;
    snprintf(p, sizeof p, "%s/Arcade/Logs/scheda-tolta.txt", dir);
    FILE *f = fopen(p, "a");
    if (!f) return false;
    fwrite(log_buf, 1, log_len, f);
    if (kernel) kernel_tail(f, 60);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    log_len = 0;
    return true;
}
static void save_log(void) { save_log_in(mnt, false); }

/* rimontaggio non riuscito con la stessa scheda, riparata da fsck: prima del riavvio il registro va
   comunque sulla scheda, agganciata da parte un momento (senza, la prova del 25 novembre non ha
   lasciato niente) */
static void save_log_aside(const char *part) {
    if (!same_card || !fsck_ok) return;   /* su un'altra scheda o su un FAT non riparato non si scrive */
    char d[256];
    run_path(d, sizeof d, "guardia-registro");
    mkdir(d, 0700);
    if (mount(part, d, "vfat", MS_NOATIME, "flush") != 0) { rmdir(d); return; }
    save_log_in(d, true);
    sync();
    umount2(d, MNT_DETACH);
    rmdir(d);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: guardia-sd DISCO PARTIZIONE [SECONDI]\n"); return 2; }
    const char *disk = argv[1], *part = argv[2];
    int default_s = argc > 3 ? atoi(argv[3]) : 60;
    run_dir = getenv("GUARDIA_RUN") ? getenv("GUARDIA_RUN") : "/run";
    mnt = getenv("GUARDIA_MNT") ? getenv("GUARDIA_MNT") : "/mnt/SDCARD";
    sysb = getenv("GUARDIA_SYS") ? getenv("GUARDIA_SYS") : "/sys/block";
    int reserve = getenv("GUARDIA_RISERVA") ? atoi(getenv("GUARDIA_RISERVA")) : 30;
    if (chdir("/") != 0) return 1;
    uint32_t serial = 0;
    bool have_serial = fat_serial(part, &serial);
    char cid[64];
    card_cid(disk, cid, sizeof cid);
    char dev[32], dev_now[32];
    part_dev(disk, part, dev, sizeof dev);
    char p_state[256], p_now[256], p_set[256];
    run_path(p_state, sizeof p_state, "scheda-tolta");
    run_path(p_now, sizeof p_now, "scheda-spegni-ora");
    run_path(p_set, sizeof p_set, "scheda-impostazioni");
    unlink(p_state);
    unlink(p_now);
    int absent = 0, count = 0, secs = default_s;
    bool unreadable = false;   /* già scritto che la scheda c'è ma non si legge */
    char action[16] = "spegni", line[64];
    double deadline = 0, last_slept = slept();
    for (;;) {
        int ds = disk_state(disk);
        if (ds < 0) { usleep(100000); continue; }   /* non si sa: si guarda di nuovo */
        bool present = ds == 1;
        /* sparita e tornata fra due controlli: per il kernel è un disco nuovo */
        bool blip = false;
        if (present && !absent) {
            part_dev(disk, part, dev_now, sizeof dev_now);
            if (!dev[0]) snprintf(dev, sizeof dev, "%s", dev_now);
            blip = dev[0] && dev_now[0] && strcmp(dev, dev_now);
        }
        /* il disco sparisce solo se è stato tolto (un controllo basta): il vecchio aggancio è morto */
        if ((!present && ++absent == 1) || blip) {
            unlink(p_now);   /* una richiesta rimasta da prima non spegne questa volta */
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
            note(blip ? "scheda sparita e tornata in un attimo" : "scheda tolta: %s fra %d s", action, secs);
        }
        if (!present) {
            if (absent >= 1) {
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
            if (absent >= 1 || blip) {   /* rimessa */
                if (!unreadable) {   /* infilata male: si riprova in silenzio, la finestra resta sul conto */
                    snprintf(line, sizeof line, "%.0f rimonto\n", deadline);
                    put_state("scheda-tolta", line);
                    note("scheda rimessa");
                }
                same_card = false;
                int rm = remount(disk, part, have_serial, serial, cid);
                if (rm != RM_OK) {
                    /* infilata male (c'è ma non si legge) o tolta di nuovo durante il controllo: per
                       tutti è ancora fuori, il conto alla rovescia continua e si riprova. Mai un
                       riavvio: senza scheda la console partirebbe dal firmware interno (Brick Pro,
                       26 novembre: scheda avvicinata e non infilata, riavvio) */
                    if (rm == RM_NON_LEGGE || !disk_present(disk)) {
                        if (!unreadable) note(rm == RM_NON_LEGGE ? "scheda presente ma non leggibile: si aspetta" : "tolta di nuovo durante il controllo");
                        unreadable = rm == RM_NON_LEGGE;
                        snprintf(line, sizeof line, "%.0f %s\n", deadline, action);
                        put_state("scheda-tolta", line);
                        absent = 1;
                        last_slept = slept();
                        if (access(p_now, F_OK) == 0) { unlink(p_now); finish(false, "SPEGNIMENTO CHIESTO"); }
                        double now = mono();
                        if (!strcmp(action, "spegni") && now >= deadline) finish(false, "LA SCHEDA NON E' STATA RIMESSA");
                        if (now >= deadline + reserve) finish(false, "LA SCHEDA NON E' STATA RIMESSA");
                        usleep(100000);
                        continue;
                    }
                    note("passo non riuscito: riavvio");
                    save_log_aside(part);
                    finish(true, "SCHEDA RIMESSA: RIPARTO");   /* il segno resta: nessuno riparte sulla scheda morta */
                }
                unreadable = false;
                note("rimontata in %s: si riprende", mnt);
                save_log();
                /* prima il contatore, poi via il segno: chi vede la scheda di nuovo dentro vede anche
                   il contatore nuovo, e non riprende un momento sul vecchio aggancio */
                snprintf(line, sizeof line, "%d\n", ++count);
                put_state("scheda-rimessa", line);
                unlink(p_state);
                unlink(p_now);
                part_dev(disk, part, dev, sizeof dev);
            }
            absent = 0;
        }
        last_slept = slept();
        usleep(100000);   /* dieci volte al secondo: anche una scheda fuori per un attimo si vede */
    }
}
