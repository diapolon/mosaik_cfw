/* Lettura degli zip (directory centrale, voci salvate o compresse con deflate, controllo del
   CRC): backup e ripristino del trasferimento file, pacchetti dello Store. */
#include "zipread.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>


static unsigned get16(const unsigned char *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }

/* directory centrale; -1 se non è uno zip leggibile */
int zip_dir(FILE *f, zent_t **out) {
    *out = NULL;
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long size = ftell(f), from = size > 65557 ? size - 65557 : 0;
    if (size < 22) return -1;
    unsigned char *tail = malloc((size_t)(size - from));
    if (!tail || fseek(f, from, SEEK_SET) != 0 || fread(tail, 1, (size_t)(size - from), f) != (size_t)(size - from)) { free(tail); return -1; }
    long e = -1;
    for (long i = size - from - 22; i >= 0 && e < 0; i--)
        if (get32(tail + i) == 0x06054b50) e = i;
    if (e < 0) { free(tail); return -1; }
    unsigned n = get16(tail + e + 10);
    uint32_t cdsize = get32(tail + e + 12), cdoff = get32(tail + e + 16);
    free(tail);
    if ((long)cdoff + (long)cdsize > size || n > 65000) return -1;
    unsigned char *cd = malloc(cdsize ? cdsize : 1);
    zent_t *z = calloc(n ? n : 1, sizeof *z);
    if (!cd || !z || fseek(f, (long)cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsize, f) != cdsize) { free(cd); free(z); return -1; }
    uint32_t p = 0;
    int k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (p + 46 > cdsize || get32(cd + p) != 0x02014b50) { free(cd); free(z); return -1; }
        unsigned nl = get16(cd + p + 28), xl = get16(cd + p + 30), cl = get16(cd + p + 32);
        if (p + 46 + nl > cdsize) { free(cd); free(z); return -1; }
        z[k].method = get16(cd + p + 10);
        z[k].crc = get32(cd + p + 16);
        z[k].csize = get32(cd + p + 20);
        z[k].usize = get32(cd + p + 24);
        z[k].off = get32(cd + p + 42);
        snprintf(z[k].name, sizeof z[k].name, "%.*s", (int)nl, (const char *)cd + p + 46);
        k++;
        p += 46 + nl + xl + cl;
    }
    free(cd);
    *out = z;
    return k;
}

/* dati di una voce (anche compressa): in memoria (to NULL, max n byte in mem) o in un file */
bool zip_entry(FILE *f, const zent_t *e, FILE *to, char *mem, size_t n) {
    unsigned char h[30];
    if (fseek(f, (long)e->off, SEEK_SET) != 0 || fread(h, 1, 30, f) != 30 || get32(h) != 0x04034b50) return false;
    if (fseek(f, (long)(e->off + 30 + get16(h + 26) + get16(h + 28)), SEEK_SET) != 0) return false;
    if (e->method != 0 && e->method != 8) return false;
    if (mem && e->usize >= n) return false;
    /* buffer propri: trasferimento file e Store possono leggere zip insieme */
    unsigned char *in = malloc(65536), *outb = malloc(65536);
    if (!in || !outb) { free(in); free(outb); return false; }
    uLong crc = crc32(0L, Z_NULL, 0);
    uint32_t left = e->csize, done = 0;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (e->method == 8 && inflateInit2(&zs, -MAX_WBITS) != Z_OK) { free(in); free(outb); return false; }
    bool ok = true;
    int zr = Z_OK;
    while (ok && left > 0 && zr != Z_STREAM_END) {
        size_t want = left < 65536 ? left : 65536, got = fread(in, 1, want, f);
        if (got == 0) { ok = false; break; }
        left -= (uint32_t)got;
        if (e->method == 0) {
            crc = crc32(crc, in, (uInt)got);
            if (mem) memcpy(mem + done, in, got);
            else ok = fwrite(in, 1, got, to) == got;
            done += (uint32_t)got;
            continue;
        }
        zs.next_in = in;
        zs.avail_in = (uInt)got;
        do {
            zs.next_out = outb;
            zs.avail_out = 65536;
            zr = inflate(&zs, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) { ok = false; break; }
            size_t k = 65536 - zs.avail_out;
            if (done + k > e->usize) { ok = false; break; }
            crc = crc32(crc, outb, (uInt)k);
            if (mem) memcpy(mem + done, outb, k);
            else if (fwrite(outb, 1, k, to) != k) ok = false;
            done += (uint32_t)k;
        } while (ok && zs.avail_out == 0 && zr != Z_STREAM_END);
    }
    if (e->method == 8) inflateEnd(&zs);
    if (mem && ok) mem[done] = 0;
    free(in);
    free(outb);
    return ok && done == e->usize && (uint32_t)crc == e->crc;
}
