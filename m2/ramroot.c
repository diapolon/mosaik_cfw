/* ramroot: il sistema passa dalla SD alla memoria senza riavvio (23 novembre).
 *
 *   ramroot LOOP COPIA        per esempio: ramroot /dev/loop0 /run/sistema.sqfs
 *
 * Il sistema di Mosaik è arcade-system.squashfs su un loop in sola lettura che legge il file sulla
 * SD (m2/initramfs-init). ramroot copia in memoria (COPIA, in un tmpfs) quello che il loop legge,
 * leggendo il loop stesso: la copia è per forza il sistema in uso, anche dopo un ritorno al .prec o
 * un aggiornamento che ha già messo un file nuovo sulla scheda. Poi dice al loop di leggere dalla
 * copia (ioctl LOOP_CHANGE_FD: il kernel lo permette solo a un loop in sola lettura e con un file
 * della stessa dimensione, altrimenti rifiuta e non cambia niente). Da lì in poi BusyBox,
 * system-init, launcher, runner e librerie del firmware non leggono più la scheda: è la base per
 * poterla togliere a console accesa. Se dopo la copia la memoria libera scenderebbe sotto
 * RAMROOT_MARGINE_MB (256), non copia: la memoria
 * serve prima ai giochi. Qualunque errore
 * lascia il sistema sulla scheda e toglie la copia. Statico (m2/build-m2.sh); m2/test-ramroot.sh. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static long long mem_available(void) {   /* byte, -1 se non si sa */
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    long long kb = -1;
    while (f && fgets(l, sizeof l, f))
        if (sscanf(l, "MemAvailable: %lld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb < 0 ? -1 : kb * 1024;
}

static int fail(const char *copy, const char *fmt, const char *a, const char *b) {
    if (copy) unlink(copy);
    printf(fmt, a, b);
    printf(": il sistema resta sulla scheda\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "uso: ramroot LOOP COPIA\n"); return 2; }
    const char *loop = argv[1], *copy = argv[2];
    int lfd = open(loop, O_RDONLY);
    if (lfd < 0) return fail(NULL, "loop %s non aperto (%s)", loop, strerror(errno));
    struct loop_info64 info;
    if (ioctl(lfd, LOOP_GET_STATUS64, &info) != 0) return fail(NULL, "loop %s senza file (%s)", loop, strerror(errno));
    if (!(info.lo_flags & LO_FLAGS_READ_ONLY)) return fail(NULL, "loop %s non in sola lettura%s", loop, "");
    unsigned long long size = 0;
    if (ioctl(lfd, BLKGETSIZE64, &size) != 0 || size == 0) return fail(NULL, "dimensione di %s sconosciuta%s", loop, "");
    long long margin = (getenv("RAMROOT_MARGINE_MB") ? atoll(getenv("RAMROOT_MARGINE_MB")) : 256) * 1024 * 1024;
    long long avail = mem_available();
    if (avail >= 0 && avail - (long long)size < margin) {
        char a[32], b[32];
        snprintf(a, sizeof a, "%llu", size >> 20);
        snprintf(b, sizeof b, "%lld", avail >> 20);
        return fail(NULL, "memoria scarsa (sistema %s MB, libera %s MB)", a, b);
    }
    int cfd = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (cfd < 0) return fail(NULL, "copia %s non creata (%s)", copy, strerror(errno));
    static char buf[1 << 20];
    unsigned long long done = 0;
    while (done < size) {
        ssize_t n = read(lfd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(cfd); return fail(copy, "lettura del sistema interrotta (%s)%s", n < 0 ? strerror(errno) : "fine anticipata", ""); }
        for (ssize_t w = 0; w < n;) {
            ssize_t m = write(cfd, buf + w, n - w);
            if (m < 0 && errno == EINTR) continue;
            if (m <= 0) { close(cfd); return fail(copy, "copia non scritta (%s)%s", m < 0 ? strerror(errno) : "spazio finito", ""); }
            w += m;
        }
        done += n;
    }
    close(cfd);
    int nfd = open(copy, O_RDONLY);
    if (nfd < 0) return fail(copy, "copia %s non riaperta (%s)", copy, strerror(errno));
    if (ioctl(lfd, LOOP_CHANGE_FD, nfd) != 0) return fail(copy, "cambio rifiutato dal kernel (%s)%s", strerror(errno), "");
    close(nfd);
    close(lfd);
    printf("sistema in memoria: %s legge %s (%llu MB, prima %s)\n", loop, copy, size >> 20, (char *)info.lo_file_name);
    return 0;
}
