/* sdgrow: allarga la partizione ARCADE (FAT32) fino alla fine della scheda SD.
 *
 *   sdgrow DISCO [PARTIZIONE]      es. sdgrow /dev/mmcblk0 /dev/mmcblk0p4
 *
 * L'immagine SD è più piccola della scheda. Il suo FAT32 nasce con la tabella dei cluster
 * già grande per schede fino a 256 GB (m2/fatprep.py): per allargarlo basta cambiare il
 * numero di settori nel settore di avvio, senza spostare dati. Passi:
 *   1. GPT: l'ultima partizione (boot-resource, la ARCADE) arriva in fondo alla scheda e la
 *      copia di riserva della GPT si sposta alla fine. Prima la copia nuova in fondo, poi la
 *      principale: se manca la corrente a metà, almeno una delle due resta valida.
 *   2. Il kernel rilegge le partizioni (BLKRRPART). Se non ci riesce serve un riavvio, e il
 *      FAT si allarga all'avvio dopo: mai un FAT più grande della partizione che il kernel vede.
 *   3. FAT32: nuovo numero di settori (anche nella copia del settore di avvio) e spazio libero
 *      aggiornato. Le voci dei cluster nuovi devono essere a zero (liberi), altrimenti si ferma.
 * Con PARTIZIONE "-" o assente (prove su file immagine) la misura è quella della GPT.
 *
 * Uscita: 0 allargata, 1 niente da fare, 2 errore o schema non riconosciuto (nessuna modifica
 * pericolosa), 3 GPT allargata ma serve un riavvio prima del FAT.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEC 512
#define MIN_GAIN 2048   /* sotto 1 MB in più non vale la pena */

static int fd;

/* la schermata compare solo se c'è davvero da lavorare */
static void notify(void) {
    static int shown;
    if (!shown++ && system("fbmsg show blu 0 'PREPARO LA SD' 'PRIMA ACCENSIONE' 2>/dev/null")) {}
}

static void die(const char *m) { fprintf(stderr, "sdgrow: %s\n", m); exit(2); }

static uint32_t crc_table[256];
static uint32_t crc32(const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint16_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return g16(p) | (uint32_t)g16(p + 2) << 16; }
static uint64_t g64(const uint8_t *p) { return g32(p) | (uint64_t)g32(p + 4) << 32; }
static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }

static void rd(uint64_t lba, void *buf, size_t n) {
    if (pread(fd, buf, n, (off_t)(lba * SEC)) != (ssize_t)n) die("lettura non riuscita");
}
static void wr(uint64_t lba, const void *buf, size_t n) {
    if (pwrite(fd, buf, n, (off_t)(lba * SEC)) != (ssize_t)n) die("scrittura non riuscita");
}

static uint64_t dev_sectors(int f) {
    struct stat st;
    if (fstat(f, &st) != 0) die("stat non riuscita");
    if (S_ISBLK(st.st_mode)) {
        uint64_t b = 0;
        if (ioctl(f, BLKGETSIZE64, &b) != 0) die("misura del disco non leggibile");
        return b / SEC;
    }
    return (uint64_t)st.st_size / SEC;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: sdgrow DISCO [PARTIZIONE]\n"); return 2; }
    fd = open(argv[1], O_RDWR | O_SYNC);
    if (fd < 0) die("disco non apribile");
    uint64_t total = dev_sectors(fd);
    int changed = 0;

    /* ------------------------------------------------------------------ GPT */
    uint8_t h[SEC];
    rd(1, h, SEC);
    if (memcmp(h, "EFI PART", 8)) die("GPT non trovata");
    uint32_t hsize = g32(h + 12);
    if (hsize < 92 || hsize > SEC) die("intestazione GPT strana");
    uint8_t hc[SEC];
    memcpy(hc, h, SEC);
    p32(hc + 16, 0);
    if (crc32(hc, hsize) != g32(h + 16)) die("GPT principale danneggiata");
    uint64_t ent_lba = g64(h + 72);
    uint32_t nent = g32(h + 80), esize = g32(h + 84);
    if (esize != 128 || nent == 0 || nent > 1024) die("tabella GPT strana");
    size_t ebytes = (size_t)nent * esize, esec = (ebytes + SEC - 1) / SEC;
    uint8_t *e = calloc(esec, SEC);
    if (!e) die("memoria");
    rd(ent_lba, e, esec * SEC);
    if (crc32(e, ebytes) != g32(h + 88)) die("voci GPT danneggiate");

    /* la partizione da allargare è l'ultima sul disco e deve chiamarsi boot-resource */
    int last = -1;
    for (uint32_t i = 0; i < nent; i++) {
        uint8_t *p = e + i * esize;
        if (!g64(p + 32)) continue;
        if (last < 0 || g64(p + 40) > g64(e + last * esize + 40)) last = (int)i;
    }
    if (last < 0) die("nessuna partizione");
    uint8_t *pe = e + last * esize;
    const char *want = "boot-resource";
    for (int i = 0; want[i] || g16(pe + 56 + 2 * i); i++)
        if (g16(pe + 56 + 2 * i) != (uint8_t)want[i]) die("l'ultima partizione non è la ARCADE (boot-resource)");
    uint64_t pfirst = g64(pe + 32), plast = g64(pe + 40);

    uint64_t new_hdr = total - 1, new_ent = new_hdr - esec, new_lastuse = new_ent - 1;
    if (new_lastuse > g64(h + 48) && new_lastuse >= plast + MIN_GAIN) {
        notify();
        printf("partizione ARCADE: %" PRIu64 " -> %" PRIu64 " MB\n",
               (plast - pfirst + 1) / 2048, (new_lastuse - pfirst + 1) / 2048);
        p64(pe + 40, new_lastuse);
        uint32_t ecrc = crc32(e, ebytes);
        /* copia di riserva in fondo: voci, poi intestazione */
        uint8_t b[SEC];
        memcpy(b, h, SEC);
        p64(b + 24, new_hdr);       /* questa intestazione */
        p64(b + 32, 1);             /* l'altra */
        p64(b + 48, new_lastuse);
        p64(b + 72, new_ent);
        p32(b + 88, ecrc);
        p32(b + 16, 0);
        p32(b + 16, crc32(b, hsize));
        wr(new_ent, e, esec * SEC);
        wr(new_hdr, b, SEC);
        fsync(fd);
        /* principale */
        p64(h + 32, new_hdr);
        p64(h + 48, new_lastuse);
        p32(h + 88, ecrc);
        p32(h + 16, 0);
        p32(h + 16, crc32(h, hsize));
        wr(ent_lba, e, esec * SEC);
        wr(1, h, SEC);
        /* MBR protettivo: misura del disco (al massimo 2 TB) */
        uint8_t m[SEC];
        rd(0, m, SEC);
        if (m[510] == 0x55 && m[511] == 0xAA && m[446 + 4] == 0xEE)
            p32(m + 446 + 12, total - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(total - 1));
        wr(0, m, SEC);
        fsync(fd);
        plast = new_lastuse;
        changed = 1;
    }

    /* ---------------------------------------- misura che il kernel vede per la partizione */
    uint64_t psize = plast - pfirst + 1;
    if (argc > 2 && strcmp(argv[2], "-")) {
        if (changed && ioctl(fd, BLKRRPART) != 0) {
            printf("il kernel non ha riletto le partizioni (%s): riavvio necessario\n", strerror(errno));
            return 3;
        }
        int pf = -1;
        for (int t = 0; t < 40 && pf < 0; t++) {   /* il nodo della partizione viene ricreato */
            pf = open(argv[2], O_RDONLY);
            if (pf < 0) usleep(50000);
        }
        if (pf < 0) die("partizione non apribile");
        uint64_t k = dev_sectors(pf);
        close(pf);
        if (k < psize) psize = k;
    }

    /* ---------------------------------------------------------------- FAT32 */
    uint8_t bs[SEC];
    rd(pfirst, bs, SEC);
    if (bs[510] != 0x55 || bs[511] != 0xAA || memcmp(bs + 82, "FAT32   ", 8)) die("la partizione ARCADE non è FAT32");
    unsigned bps = g16(bs + 11), spc = bs[13], rsvd = g16(bs + 14), nfats = bs[16];
    uint32_t fatsz = g32(bs + 36), tot = g32(bs + 32);
    if (bps != SEC || !spc || (spc & (spc - 1)) || !rsvd || !nfats || nfats > 2 || !fatsz || g16(bs + 19) || !tot)
        die("FAT32 con parametri inattesi");
    uint64_t data = rsvd + (uint64_t)nfats * fatsz;
    uint64_t cap = (uint64_t)fatsz * SEC / 4 - 2;   /* cluster che la tabella può descrivere */
    if (cap > 0x0FFFFFF4u) cap = 0x0FFFFFF4u;
    uint64_t old_cl = (tot - data) / spc;
    uint64_t lim = psize > 0xFFFFFFFFu ? 0xFFFFFFFFu : psize;
    uint64_t new_cl = (lim - data) / spc;
    if (new_cl > cap) new_cl = cap;
    if (new_cl * spc < old_cl * spc + MIN_GAIN) {
        if (!changed) printf("niente da fare: FAT già alla misura possibile (%" PRIu64 " MB)\n", (uint64_t)tot / 2048);
        free(e);
        close(fd);
        return changed ? 0 : 1;
    }
    uint64_t new_tot = new_cl == cap ? data + new_cl * spc : lim;

    /* le voci dei cluster nuovi devono essere libere in tutte le copie della tabella */
    uint64_t from = (old_cl + 2) * 4, to = (new_cl + 2) * 4;
    uint8_t *buf = malloc(1 << 20);
    if (!buf) die("memoria");
    for (unsigned f = 0; f < nfats; f++) {
        uint64_t base = (pfirst + rsvd + (uint64_t)f * fatsz) * SEC;
        for (uint64_t o = from; o < to;) {
            size_t n = to - o > (1 << 20) ? (1 << 20) : (size_t)(to - o);
            if (pread(fd, buf, n, (off_t)(base + o)) != (ssize_t)n) die("lettura della tabella FAT non riuscita");
            for (size_t i = 0; i < n; i++)
                if (buf[i]) die("tabella FAT non vuota oltre la fine: FAT non preparato per crescere");
            o += n;
        }
    }
    free(buf);

    notify();
    printf("FAT: %" PRIu64 " -> %" PRIu64 " MB (%" PRIu64 " cluster da %u KB)\n",
           (uint64_t)tot / 2048, new_tot / 2048, new_cl, spc / 2);
    p32(bs + 32, (uint32_t)new_tot);
    unsigned bk = g16(bs + 50), fsi = g16(bs + 48);
    /* spazio libero: si aggiunge quello nuovo se il conteggio era valido */
    for (int copy = 0; copy < 2; copy++) {
        unsigned s = copy ? bk + fsi : fsi;
        if (!fsi || fsi >= rsvd || (copy && (!bk || bk >= rsvd))) continue;
        uint8_t fi[SEC];
        rd(pfirst + s, fi, SEC);
        if (g32(fi) != 0x41615252u || g32(fi + 484) != 0x61417272u) continue;
        uint32_t fr = g32(fi + 488);
        if (fr != 0xFFFFFFFFu && fr <= old_cl) p32(fi + 488, fr + (uint32_t)(new_cl - old_cl));
        else p32(fi + 488, 0xFFFFFFFFu);
        wr(pfirst + s, fi, SEC);
    }
    if (bk && bk < rsvd) {
        uint8_t b2[SEC];
        rd(pfirst + bk, b2, SEC);
        if (b2[510] == 0x55 && b2[511] == 0xAA && !memcmp(b2 + 82, "FAT32   ", 8)) {
            p32(b2 + 32, (uint32_t)new_tot);
            wr(pfirst + bk, b2, SEC);
        }
    }
    wr(pfirst, bs, SEC);
    fsync(fd);
    free(e);
    close(fd);
    return 0;
}
