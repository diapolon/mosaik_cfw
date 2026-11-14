/* sdgrow: allarga la partizione ARCADE (FAT32) fino alla fine della scheda SD.
 *
 *   sdgrow DISCO [PARTIZIONE]      es. sdgrow /dev/mmcblk0 /dev/mmcblk0p4
 *
 * L'immagine SD è più piccola della scheda. Il suo FAT32 nasce con la tabella dei cluster
 * già grande per schede fino a 256 GB (m2/fatprep.py): per allargarlo basta cambiare il
 * numero di settori nel settore di avvio, senza spostare dati. Passi:
 *   1. GPT: l'ultima partizione (boot-resource, la ARCADE) arriva in fondo alla scheda e la
 *      copia di riserva della GPT si sposta alla fine. Prima la copia nuova in fondo, poi la
 *      principale: se manca la corrente a metà, almeno una delle due resta valida. Alla volta
 *      dopo, se la principale ha le voci rovinate ma l'intestazione buona, si ricostruisce
 *      dalla copia di riserva (stesso disco, voci integre).
 *   2. Il kernel rilegge le partizioni (BLKRRPART). Se non ci riesce serve un riavvio, e il
 *      FAT si allarga all'avvio dopo: mai un FAT più grande della partizione che il kernel vede.
 *   3. FAT32: nuovo numero di settori (anche nella copia del settore di avvio) e spazio libero
 *      aggiornato. Le voci dei cluster nuovi devono essere a zero (liberi), altrimenti si ferma.
 * Con PARTIZIONE "-" o assente (prove su file immagine) la misura è quella della GPT.
 *
 * Uscita: 0 allargata, 1 niente da fare, 2 errore o schema non riconosciuto (nessuna modifica
 * pericolosa), 3 GPT allargata ma serve un riavvio prima del FAT, 5 la GPT è più grande della
 * partizione che il kernel vede e non è stata appena cambiata (dopo un riavvio il kernel non la
 * legge): il FAT si ferma alla misura del kernel (audit SDGROW, B2), 6 come 3 ma il FAT vecchio
 * non risulta dentro la partizione che il kernel vede ancora: l'initramfs non lo monta per il
 * registro prima del riavvio (verifica audit SDGROW, SDG-R01).
 *
 * Dal sistema avviato (Impostazioni → Scheda e dati → Allarga la SD, dalla 0.9.53) il FAT è
 * montato e non si tocca mai:
 *   sdgrow -n DISCO     prova senza scrivere nulla: dice cosa farebbe o perché non si può
 *                       (0 si può allargare, 1 niente da fare, 2 errore)
 *   sdgrow -g DISCO     solo la GPT; 3 se al riavvio l'initramfs deve allargare il FAT, 1 niente
 *                       da fare, 2 errore
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <sys/sysmacros.h>
#include <unistd.h>

#define SEC 512
#define MIN_GAIN 2048   /* sotto 1 MB in più non vale la pena */

static int fd;

/* la schermata compare solo se c'è davvero da lavorare */
static int dry, gpt_only;   /* -n: niente scritture; -g: solo la GPT, mai il FAT */
static void notify(void) {
    static int shown;
    if (dry || gpt_only) return;   /* dal sistema avviato: lo schermo è del launcher */
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
    if (dry) return;   /* prova: si calcola tutto, non si scrive nulla */
    if (pwrite(fd, buf, n, (off_t)(lba * SEC)) != (ssize_t)n) die("scrittura non riuscita");
}
/* le scritture devono essere sulla scheda prima del passo dopo (audit SDGROW, B3) */
static void flush(void) {
    if (!dry && fsync(fd) != 0) die("scrittura non confermata dalla scheda (fsync)");
}
/* rilettura di quello che si è appena scritto */
static void verify(uint64_t lba, const void *buf, size_t n) {
    if (dry) return;
    uint8_t *c = malloc(n);
    if (!c) die("memoria");
    rd(lba, c, n);
    if (memcmp(c, buf, n)) die("la scheda non ha tenuto la scrittura");
    free(c);
}

/* Prima di scrivere qualsiasi cosa (audit SDGROW, B3): la GPT deve stare dentro il disco, le voci
   prima della zona utile, ogni partizione dentro la zona utile e nessuna sovrapposta a un'altra.
   Una GPT più grande del disco (scheda falsa o lettore che dichiara un'altra misura) si rifiuta. */
static void gpt_check(const uint8_t *h, const uint8_t *e, uint32_t nent, uint32_t esize, size_t esec, uint64_t total) {
    uint64_t first = g64(h + 40), last = g64(h + 48), alt = g64(h + 32), ent = g64(h + 72);
    if (g64(h + 24) != 1) die("GPT: intestazione principale fuori posto");
    if (total < 64 || alt >= total || last >= total) die("GPT più grande del disco: la scheda dichiara meno spazio della tabella");
    if (first > last || ent < 2 || ent + esec > first) die("GPT: limiti non validi");
    for (uint32_t i = 0; i < nent; i++) {
        const uint8_t *p = e + (size_t)i * esize;
        uint64_t a = g64(p + 32), b = g64(p + 40);
        if (!a && !b) continue;
        if (a > b || a < first || b > last) die("GPT: partizione fuori dalla zona utile");
        for (uint32_t k = i + 1; k < nent; k++) {
            const uint8_t *q = e + (size_t)k * esize;
            uint64_t c = g64(q + 32), d = g64(q + 40);
            if ((c || d) && !(d < a || c > b)) die("GPT: partizioni sovrapposte");
        }
    }
}

/* Il FAT32 della partizione ARCADE, prima di toccare la GPT: parametri coerenti, metadati dentro
   la partizione e mai più grande di lei. Così la GPT non si cambia per un FAT che poi non si
   potrebbe allargare (audit SDGROW, B3). */
static void fat_check(uint64_t pfirst, uint64_t psize) {
    uint8_t bs[SEC];
    rd(pfirst, bs, SEC);
    if (bs[510] != 0x55 || bs[511] != 0xAA || memcmp(bs + 82, "FAT32   ", 8)) die("la partizione ARCADE non è FAT32");
    unsigned bps = g16(bs + 11), spc = bs[13], rsvd = g16(bs + 14), nfats = bs[16];
    uint32_t fatsz = g32(bs + 36), tot = g32(bs + 32);
    if (bps != SEC || !spc || (spc & (spc - 1)) || !rsvd || !nfats || nfats > 2 || !fatsz || g16(bs + 19) || !tot)
        die("FAT32 con parametri inattesi");
    uint64_t data = rsvd + (uint64_t)nfats * fatsz;
    if (data + spc > tot) die("FAT32: area dati vuota o tabella oltre il filesystem");
    if (tot > psize) die("FAT32 più grande della sua partizione");
}

/* Prima del riavvio, con la GPT allargata e il kernel che non l'ha riletta: l'initramfs monta il
   FAT vecchio un momento per scrivere il registro. Si può solo se la partizione che il kernel vede
   è del disco giusto, comincia esattamente al primo settore del FAT e lo contiene tutto. L'inizio
   si confronta come numero (start in sysfs), non dal contenuto: il FAT ha una copia identica del
   primo settore poco più avanti (verifica audit SDGROW, SDG-R01). Sola lettura. */
static uint64_t sys_num(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned long long v = 0;
    int ok = f && fscanf(f, "%llu", &v) == 1;
    if (f) fclose(f);
    return ok ? (uint64_t)v : UINT64_MAX;
}
static uint64_t dev_sectors(int f);
static int old_fat_fits(const char *part, uint64_t pfirst) {
    int pf = open(part, O_RDONLY);
    if (pf < 0) { printf("partizione %s non apribile: niente registro prima del riavvio\n", part); return 0; }
    struct stat ps, ds;
    uint64_t start = UINT64_MAX;
    int same_disk = 0;
    if (fstat(pf, &ps) == 0 && S_ISBLK(ps.st_mode)) {
        char p[96], d[32];
        snprintf(p, sizeof p, "/sys/dev/block/%u:%u/start", major(ps.st_rdev), minor(ps.st_rdev));
        start = sys_num(p);
        /* il disco padre della partizione deve essere quello aperto da sdgrow */
        snprintf(p, sizeof p, "/sys/dev/block/%u:%u/../dev", major(ps.st_rdev), minor(ps.st_rdev));
        FILE *f = fopen(p, "r");
        unsigned ma = 0, mi = 0;
        if (f && fscanf(f, "%u:%u", &ma, &mi) == 2 && fstat(fd, &ds) == 0 && S_ISBLK(ds.st_mode))
            same_disk = makedev(ma, mi) == ds.st_rdev;
        if (f) fclose(f);
        snprintf(d, sizeof d, "%u:%u", ma, mi);
        if (!same_disk) { close(pf); printf("la partizione %s non risulta del disco aperto (%s): niente registro prima del riavvio\n", part, d); return 0; }
    } else {
        /* prove su file immagine: l'inizio lo dice la prova (test-sdgrow.sh), altrimenti niente */
        const char *e = getenv("SDGROW_PART_START");
        if (e && *e) start = strtoull(e, NULL, 10);
    }
    if (start != pfirst) {
        close(pf);
        if (start == UINT64_MAX) printf("inizio della partizione del kernel non leggibile: niente registro prima del riavvio\n");
        else printf("la partizione del kernel comincia al settore %" PRIu64 ", il FAT al %" PRIu64 ": niente registro prima del riavvio\n", start, pfirst);
        return 0;
    }
    uint64_t k = dev_sectors(pf);
    uint8_t a[SEC], b[SEC];
    int ok = pread(pf, a, SEC, 0) == SEC;
    close(pf);
    rd(pfirst, b, SEC);
    if (!ok || memcmp(a, b, SEC)) { printf("la partizione del kernel non comincia con il FAT: niente registro prima del riavvio\n"); return 0; }
    uint32_t tot = g32(b + 32);
    if (!tot || tot > k) { printf("FAT di %u settori, la partizione del kernel ne ha %" PRIu64 ": niente registro prima del riavvio\n", tot, k); return 0; }
    return 1;
}

/* Copia di riserva della GPT in "lba", se è integra e appartiene allo stesso disco della
   principale "ph": intestazione in b, voci in e */
static int backup_ok(uint64_t lba, const uint8_t *ph, uint32_t hsize, uint8_t *b, uint8_t *e, size_t esec, size_t ebytes) {
    uint8_t c[SEC];
    if (pread(fd, b, SEC, (off_t)(lba * SEC)) != SEC || memcmp(b, "EFI PART", 8)) return 0;
    if (g32(b + 12) != hsize || g64(b + 24) != lba) return 0;
    memcpy(c, b, SEC);
    p32(c + 16, 0);
    if (crc32(c, hsize) != g32(b + 16)) return 0;
    if (memcmp(b + 56, ph + 56, 16) || g32(b + 80) != g32(ph + 80) || g32(b + 84) != g32(ph + 84)) return 0;
    if (pread(fd, e, esec * SEC, (off_t)(g64(b + 72) * SEC)) != (ssize_t)(esec * SEC)) return 0;
    return crc32(e, ebytes) == g32(b + 88);
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
    while (argc > 1 && (!strcmp(argv[1], "-n") || !strcmp(argv[1], "-g"))) {
        if (argv[1][1] == 'n') dry = 1; else gpt_only = 1;
        argv++;
        argc--;
    }
    if (argc < 2) { fprintf(stderr, "uso: sdgrow [-n|-g] DISCO [PARTIZIONE]\n"); return 2; }
    if (dry || gpt_only) argc = 2;   /* la partizione montata non si apre: misura dalla GPT */
    fd = open(argv[1], dry ? O_RDONLY : O_RDWR | O_SYNC);
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
    uint64_t repair_from = 0;   /* GPT principale da rifare dalla copia di riserva: si scrive dopo i controlli */
    if (crc32(e, ebytes) != g32(h + 88)) {
        /* corrente mancata mentre si scriveva la principale: si rifà dalla copia di riserva,
           prima quella in fondo alla scheda (la più recente), poi quella indicata */
        uint8_t b[SEC];
        uint64_t cand[2] = { total - 1, g64(h + 32) }, from = 0;
        for (int i = 0; i < 2 && !from; i++)
            if (cand[i] > ent_lba && backup_ok(cand[i], h, hsize, b, e, esec, ebytes)) from = cand[i];
        if (!from) die("voci GPT danneggiate e copia di riserva non utilizzabile");
        p64(h + 32, from);
        p64(h + 40, g64(b + 40));
        p64(h + 48, g64(b + 48));
        p32(h + 88, g32(b + 88));
        p32(h + 16, 0);
        p32(h + 16, crc32(h, hsize));
        repair_from = from;
    }

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
    gpt_check(h, e, nent, esize, esec, total);
    fat_check(pfirst, plast - pfirst + 1);

    /* tutti i controlli passati (anche nome della partizione e FAT, audit SDGROW verifica 3):
       ora si può rimettere la GPT principale dalla copia di riserva */
    if (repair_from) {
        notify();
        printf("GPT principale %s dalla copia di riserva (settore %" PRIu64 ")\n", dry ? "da ricostruire" : "ricostruita", repair_from);
        wr(ent_lba, e, esec * SEC);
        wr(1, h, SEC);
        flush();
        verify(ent_lba, e, esec * SEC);
        verify(1, h, SEC);
        changed = 1;
    }

    /* Si allarga se la partizione ha spazio dopo di sé. Non conta più che il limite utile
       dell'intestazione sia già in fondo: con il limite spostato da un altro programma e la
       partizione rimasta piccola, prima si diceva "niente da fare" (issue #7, audit SDGROW) */
    uint64_t new_hdr = total - 1, new_ent = new_hdr - esec, new_lastuse = new_ent - 1;
    if (new_ent <= g64(h + 40)) die("disco troppo piccolo per la GPT");
    if (new_lastuse >= plast + MIN_GAIN) {
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
        flush();
        verify(new_ent, e, esec * SEC);
        verify(new_hdr, b, SEC);
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
        flush();
        verify(ent_lba, e, esec * SEC);
        verify(1, h, SEC);
        plast = new_lastuse;
        changed = 1;
    }

    /* ---------------------------------------- misura che il kernel vede per la partizione */
    uint64_t psize = plast - pfirst + 1;
    int gpt_kernel_differ = 0;
    if (argc > 2 && strcmp(argv[2], "-")) {
        if (changed && ioctl(fd, BLKRRPART) != 0) {
            printf("il kernel non ha riletto le partizioni (%s): riavvio necessario\n", strerror(errno));
            return old_fat_fits(argv[2], pfirst) ? 3 : 6;
        }
        int pf = -1;
        for (int t = 0; t < 40 && pf < 0; t++) {   /* il nodo della partizione viene ricreato */
            pf = open(argv[2], O_RDONLY);
            if (pf < 0) usleep(50000);
        }
        if (pf < 0) die("partizione non apribile");
        uint64_t k = dev_sectors(pf);
        close(pf);
        if (k + MIN_GAIN <= psize) {   /* GPT più grande di quello che vede il kernel */
            printf("partizione nella GPT %" PRIu64 " MB, il kernel ne vede %" PRIu64 " MB\n", psize / 2048, k / 2048);
            gpt_kernel_differ = 1;
        }
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
    if (lim < tot || tot <= data) die("FAT32 più grande della partizione vista dal kernel");
    uint64_t new_cl = (lim - data) / spc;
    if (new_cl > cap) new_cl = cap;
    if (new_cl * spc < old_cl * spc + MIN_GAIN) {
        if (!changed) printf("niente da fare: FAT già alla misura possibile (%" PRIu64 " MB)\n", (uint64_t)tot / 2048);
        if (dry)   /* prova dall'app: le misure, per capire una scheda rimasta piccola (issue #7) */
            printf("disco %" PRIu64 " MB, partizione nella GPT %" PRIu64 " MB (settori %" PRIu64 "-%" PRIu64 "), FAT %" PRIu64 " MB, massimo del FAT %" PRIu64 " MB\n",
                   total / 2048, psize / 2048, pfirst, plast, (uint64_t)tot / 2048, (data + cap * spc) / 2048);
        free(e);
        close(fd);
        if (!changed && gpt_kernel_differ) {
            printf("il kernel non vede la partizione allargata: niente da fare finché non la legge\n");
            return 5;
        }
        return changed ? 0 : 1;
    }
    uint64_t new_tot = new_cl == cap ? data + new_cl * spc : lim;
    if (dry || gpt_only) {   /* il FAT montato non si tocca: lo allarga l'initramfs al riavvio */
        printf("FAT: %" PRIu64 " -> %" PRIu64 " MB%s\n", (uint64_t)tot / 2048, new_tot / 2048, dry ? "" : " al prossimo avvio");
        free(e);
        close(fd);
        return dry ? 0 : 3;
    }

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
    flush();
    verify(pfirst, bs, SEC);
    free(e);
    close(fd);
    return 0;
}
