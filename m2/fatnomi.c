/* fatnomi: ripara i nomi corti (8.3) uguali dentro la stessa cartella di un FAT montato.
 *
 *   fatnomi CARTELLA       es. fatnomi /mnt/SDCARD
 *
 * Il driver FAT di macOS a volte dà lo stesso nome corto a due file della stessa cartella
 * (riprodotto: mkdir aether_plaza, poi mkdir aether_rooftop, entrambe AETHER~2). Sul Mac e
 * sulla console non si vede, perché si usano i nomi lunghi. Ma al primo spegnimento brusco il
 * controllo della SD (fsck.fat -a nell'initramfs) rinomina il secondo in FSCK0000.000 e il
 * gioco perde quel file: una cartella di un gioco Linux, una copertina, un salvataggio.
 *
 * Per ogni cartella si leggono nome corto e nome lungo di ogni voce (VFAT_IOCTL_READDIR_BOTH,
 * solo Linux). In ogni gruppo con lo stesso nome corto ne resta uno; gli altri si rinominano
 * con un nome provvisorio e poi di nuovo con il loro: il kernel assegna un nome corto nuovo,
 * controllando che sia unico. Nome lungo e contenuto restano; i nomi passati al kernel sono
 * quelli che ha dato lui.
 * Il kernel trova un file anche per nome corto: una voce si può indicare per nome solo se il
 * suo nome lungo è diverso dal nome corto del gruppo (con a.txt e un secondo file b.txt di
 * nome corto A.TXT, "a.txt" indicherebbe tutti e due). Quella che non si può indicare resta
 * ferma; se sono più d'una, le altre si segnalano e si lasciano. Nessuna rinomina sovrascrive
 * un file (RENAME_NOREPLACE): con un nome ambiguo il kernel risponde EEXIST.
 *
 * Scrive una riga per ogni file riparato. Uscita: 0 tutto a posto (con o senza riparazioni),
 * 1 qualche doppione rimasto, 2 cartella non leggibile o non FAT.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/msdos_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAXDEPTH 32

typedef struct { char s[16], l[256]; } ent_t;   /* nome corto, nome lungo ("" se manca) */

static int fixed, left;

static const char *shown(const ent_t *e) { return e->l[0] ? e->l : e->s; }

/* il nome lungo indica solo questa voce: c'è, ed è diverso dal nome corto del gruppo */
static int addressable(const ent_t *e) { return e->l[0] && strcasecmp(e->l, e->s); }

static int by_short(const void *a, const void *b) {
    return strcasecmp(((const ent_t *)a)->s, ((const ent_t *)b)->s);
}

/* tutte le voci della cartella, senza "." e ".."; NULL se non è un FAT */
static ent_t *read_dir(int dfd, int *count) {
    int fd = openat(dfd, ".", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    struct __fat_dirent de[2];
    ent_t *v = NULL;
    int n = 0, cap = 0, r;
    while ((r = ioctl(fd, VFAT_IOCTL_READDIR_BOTH, de)) > 0) {
        if (!strcmp(de[0].d_name, ".") || !strcmp(de[0].d_name, "..")) continue;
        if (n == cap) {
            ent_t *nv = realloc(v, (cap = cap ? cap * 2 : 64) * sizeof *v);
            if (!nv) { r = -1; break; }
            v = nv;
        }
        snprintf(v[n].s, sizeof v[n].s, "%.15s", de[0].d_name);   /* 8.3: al massimo 12 caratteri */
        snprintf(v[n].l, sizeof v[n].l, "%s", de[1].d_reclen ? de[1].d_name : "");
        n++;
    }
    close(fd);
    if (r < 0) { free(v); return NULL; }
    *count = n;
    return v ? v : calloc(1, sizeof *v);
}

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
/* syscall diretta: il wrapper renameat2 manca nelle librerie C più vecchie */
static int rename_new(int dfd, const char *from, const char *to) {
    return (int)syscall(SYS_renameat2, dfd, from, dfd, to, RENAME_NOREPLACE);
}

/* rinomina avanti e indietro: il kernel sceglie un nome corto libero */
static int rename_twice(int dfd, const char *name) {
    char tmp[64];
    for (int i = 0; i < 1000; i++) {
        snprintf(tmp, sizeof tmp, "mosaik-nome-provvisorio-%d", i);
        if (rename_new(dfd, name, tmp) != 0) {
            if (errno == EEXIST) continue;
            return -1;
        }
        if (rename_new(dfd, tmp, name) == 0) return 0;
        /* il file resta col nome provvisorio: si dice quale, così si può rimettere a mano */
        int e = errno;
        printf("ATTENZIONE: %s è rimasto come %s\n", name, tmp);
        errno = e;
        return -1;
    }
    return -1;
}

static int walk(int dfd, const char *path, int depth) {
    int n;
    ent_t *v = read_dir(dfd, &n);
    if (!v) return -1;
    ent_t *sorted = malloc((n ? n : 1) * sizeof *v);
    if (!sorted) { free(v); return -1; }
    memcpy(sorted, v, n * sizeof *v);
    qsort(sorted, n, sizeof *v, by_short);
    for (int a = 0, b; a < n; a = b) {
        for (b = a + 1; b < n && !strcasecmp(sorted[b].s, sorted[a].s); b++) {}
        if (b - a < 2) continue;
        /* resta ferma la prima voce che non si può indicare per nome, altrimenti la prima */
        int keep = a;
        for (int j = a; j < b; j++)
            if (!addressable(&sorted[j])) { keep = j; break; }
        for (int j = a; j < b; j++) {
            if (j == keep) continue;
            if (!addressable(&sorted[j])) {
                printf("doppione che non si può indicare per nome, lasciato: %s/%s\n", path, shown(&sorted[j]));
                left++;
            } else if (rename_twice(dfd, sorted[j].l) == 0) {
                printf("riparato: %s/%s (era %s)\n", path, sorted[j].l, sorted[j].s);
                fixed++;
            } else {
                printf("non riparato (%s): %s/%s\n", strerror(errno), path, sorted[j].l);
                left++;
            }
        }
    }
    free(sorted);
    if (depth < MAXDEPTH)
        for (int i = 0; i < n; i++) {
            const char *name = shown(&v[i]);
            struct stat st;
            if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) continue;
            int sub = openat(dfd, name, O_RDONLY | O_DIRECTORY);
            if (sub < 0) continue;
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", path, name);
            walk(sub, p, depth + 1);
            close(sub);
        }
    free(v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "uso: fatnomi CARTELLA\n"); return 2; }
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (fd < 0 || walk(fd, "", 0) != 0) {
        fprintf(stderr, "fatnomi: %s non leggibile o non FAT\n", argv[1]);
        return 2;
    }
    close(fd);
    if (fixed) sync();
    return left ? 1 : 0;
}
