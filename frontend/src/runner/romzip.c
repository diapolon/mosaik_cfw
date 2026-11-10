/* ROM zippate per i core che non leggono gli zip (NES, Mega Drive...): si prende la prima voce
   con un'estensione che il core accetta, si decomprime in memoria e si controlla il CRC.
   Solo zip normali (non zip64), voci salvate o compresse con deflate: è quanto usano i set di
   ROM per console. */
#include "romzip.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define MAX_ZIP (64u << 20)   /* una ROM per console sta molto sotto: oltre è un altro tipo di file */

static uint32_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool romzip_is_zip(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".zip");
}

/* "nes|fds|unf": l'estensione di name è nell'elenco? */
bool romzip_ext_ok(const char *name, const char *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || !exts) return false;
    size_t n = strlen(dot + 1);
    for (const char *p = exts; *p; ) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncasecmp(p, dot + 1, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

int romzip_extract(const char *zip, const char *exts, void **out, size_t *out_size, char *name, size_t namen) {
    *out = NULL;
    *out_size = 0;
    FILE *f = fopen(zip, "rb");
    if (!f) return ROMZIP_OPEN;
    fseek(f, 0, SEEK_END);
    long fs = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fs < 22 || (unsigned long)fs > MAX_ZIP) { fclose(f); return ROMZIP_FORMAT; }
    unsigned char *z = malloc((size_t)fs);
    if (!z) { fclose(f); return ROMZIP_MEMORY; }
    size_t got = fread(z, 1, (size_t)fs, f);
    fclose(f);
    int rc = ROMZIP_FORMAT;
    if (got != (size_t)fs) goto done;

    /* fine della directory centrale: in fondo, prima di un commento di al più 64 KB */
    long eocd = -1;
    for (long i = fs - 22; i >= 0 && i >= fs - 22 - 65535; i--)
        if (le32(z + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) goto done;
    uint32_t count = le16(z + eocd + 10), cd = le32(z + eocd + 16);
    rc = ROMZIP_NOT_FOUND;
    for (uint32_t k = 0, p = cd; k < count; k++) {
        if ((long)p + 46 > fs || le32(z + p) != 0x02014b50) { rc = ROMZIP_FORMAT; goto done; }
        uint32_t method = le16(z + p + 10), crc = le32(z + p + 16), csize = le32(z + p + 20), usize = le32(z + p + 24);
        uint32_t nlen = le16(z + p + 28), xlen = le16(z + p + 30), clen = le16(z + p + 32), lho = le32(z + p + 42);
        if ((long)p + 46 + nlen > fs) { rc = ROMZIP_FORMAT; goto done; }
        char ename[256];
        snprintf(ename, sizeof ename, "%.*s", (int)(nlen < 255 ? nlen : 255), (const char *)z + p + 46);
        p += 46 + nlen + xlen + clen;
        if (!nlen || ename[strlen(ename) - 1] == '/' || !romzip_ext_ok(ename, exts)) continue;
        /* la voce scelta: dati dopo l'intestazione locale */
        if ((long)lho + 30 > fs || le32(z + lho) != 0x04034b50) { rc = ROMZIP_FORMAT; goto done; }
        long data = (long)lho + 30 + le16(z + lho + 26) + le16(z + lho + 28);
        if (data + (long)csize > fs || usize > MAX_ZIP || (method != 0 && method != 8)) { rc = ROMZIP_FORMAT; goto done; }
        unsigned char *buf = malloc(usize ? usize : 1);
        if (!buf) { rc = ROMZIP_MEMORY; goto done; }
        if (method == 0) {
            if (csize != usize) { free(buf); rc = ROMZIP_FORMAT; goto done; }
            memcpy(buf, z + data, usize);
        } else {
            z_stream s;
            memset(&s, 0, sizeof s);
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(buf); rc = ROMZIP_MEMORY; goto done; }
            s.next_in = z + data; s.avail_in = csize;
            s.next_out = buf; s.avail_out = usize;
            int zr = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (zr != Z_STREAM_END || s.total_out != usize) { free(buf); rc = ROMZIP_DAMAGED; goto done; }
        }
        if ((uint32_t)crc32(crc32(0, NULL, 0), buf, usize) != crc) { free(buf); rc = ROMZIP_DAMAGED; goto done; }
        const char *slash = strrchr(ename, '/');
        snprintf(name, namen, "%s", slash ? slash + 1 : ename);
        *out = buf;
        *out_size = usize;
        rc = ROMZIP_OK;
        goto done;
    }
done:
    free(z);
    return rc;
}
