/* axpctl: il chip di alimentazione AXP2202 (indirizzo 0x34) letto e comandato da utente.
 *
 *   axpctl dump       registri 0x00-0x4F in esadecimale, su una riga
 *   axpctl restore    rimette il bit 4 di 0x22, che poweroff toglie: il chip lo ricorda anche
 *                     dopo la riaccensione, e senza quel bit un riavvio diventa uno spegnimento
 *                     (provato sulla Smart Pro: 0x22 è 0x1E all'avvio normale). Scrive cosa ha
 *                     fatto: "0x22 1e" se era già a posto, "0x22 0e -> 1e" se l'ha corretto
 *   axpctl poweroff   spegnimento come il poweroff_next di NextUI (TG5040): interruzioni del
 *                     chip disattivate (0x40-0x44 = 0) e cancellate (0x48-0x4C = 0xFF), poi
 *                     0x22 = 0x0A e 0x27 = 0x01. Serve col caricatore collegato: con lo
 *                     spegnimento normale un'interruzione del caricatore rimasta in sospeso
 *                     riaccende la console, che si ferma in U-Boot con lo schermo nero.
 *
 * Il bus si trova da /sys/bus/i2c/devices/<bus>-0034 (il driver del kernel tiene l'indirizzo:
 * I2C_SLAVE_FORCE). Uscita 0 riuscito, 1 errore. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ADDR 0x34

static int open_bus(void) {
    int bus = -1;
    DIR *d = opendir("/sys/bus/i2c/devices");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        int b;
        unsigned a;
        if (sscanf(e->d_name, "%d-%x", &b, &a) == 2 && a == ADDR) { bus = b; break; }
    }
    if (d) closedir(d);
    if (bus < 0) bus = 6;   /* TG5040: s_twi, /dev/i2c-6 */
    char p[32];
    snprintf(p, sizeof p, "/dev/i2c-%d", bus);
    int fd = open(p, O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "axpctl: %s: %s\n", p, strerror(errno)); return -1; }
    if (ioctl(fd, I2C_SLAVE_FORCE, ADDR) < 0) { fprintf(stderr, "axpctl: I2C_SLAVE_FORCE: %s\n", strerror(errno)); close(fd); return -1; }
    fprintf(stderr, "axpctl: bus %d\n", bus);
    return fd;
}

static int rd(int fd, uint8_t reg, uint8_t *val) {
    struct i2c_msg m[2] = { { ADDR, 0, 1, &reg }, { ADDR, I2C_M_RD, 1, val } };
    struct i2c_rdwr_ioctl_data x = { m, 2 };
    return ioctl(fd, I2C_RDWR, &x) == 2 ? 0 : -1;
}

static int wr(int fd, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    int ok = write(fd, b, 2) == 2 ? 0 : -1;
    if (ok) fprintf(stderr, "axpctl: scrittura di %02x in %02x non riuscita: %s\n", val, reg, strerror(errno));
    return ok;
}

static void pause_ms(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "dump") && strcmp(argv[1], "poweroff") && strcmp(argv[1], "restore"))) {
        fprintf(stderr, "uso: axpctl dump|poweroff|restore\n");
        return 1;
    }
    int fd = open_bus();
    if (fd < 0) return 1;
    if (!strcmp(argv[1], "dump")) {
        for (int r = 0; r <= 0x4F; r++) {
            uint8_t v;
            if (rd(fd, (uint8_t)r, &v) == 0) printf("%02x=%02x ", r, v);
            else printf("%02x=?? ", r);
        }
        printf("\n");
        close(fd);
        return 0;
    }
    if (!strcmp(argv[1], "restore")) {
        uint8_t v;
        if (rd(fd, 0x22, &v) != 0) { fprintf(stderr, "axpctl: lettura di 0x22 non riuscita\n"); close(fd); return 1; }
        if (v & 0x10) { printf("0x22 %02x\n", v); close(fd); return 0; }
        int e = wr(fd, 0x22, (uint8_t)(v | 0x10));
        uint8_t w = 0;
        rd(fd, 0x22, &w);
        printf("0x22 %02x -> %02x\n", v, w);
        close(fd);
        return e ? 1 : 0;
    }
    int err = 0;
    for (int r = 0x40; r <= 0x44; r++) err |= wr(fd, (uint8_t)r, 0x00);
    for (int r = 0x48; r <= 0x4C; r++) err |= wr(fd, (uint8_t)r, 0xFF);
    err |= wr(fd, 0x22, 0x0A);
    pause_ms(50);
    err |= wr(fd, 0x27, 0x01);
    close(fd);
    pause_ms(1000);   /* se il chip non spegne, si torna e system-init usa poweroff -f */
    return err ? 1 : 0;
}
