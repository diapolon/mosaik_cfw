/* Misure di un'immagine lette dall'intestazione, prima di decodificarla: un'immagine enorme in
   pixel, anche se piccola su disco, finirebbe la memoria. Le stesse funzioni del Viewer
   (apps/viewer.c, audit APP-003 e V26-02), per le copertine del Lettore musicale (M61-006). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* misure dall'intestazione (PNG, GIF, BMP, JPEG), prima di decodificare: un'immagine enorme in
   pixel, anche se piccola su disco, finirebbe la memoria (audit APP-003). Falso se ignote. */
static inline uint32_t imgd_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline bool img_dims(const unsigned char *b, size_t n, long *w, long *h) {
    if (n >= 24 && !memcmp(b, "\x89PNG", 4)) { *w = imgd_be32(b + 16); *h = imgd_be32(b + 20); return true; }
    if (n >= 10 && !memcmp(b, "GIF8", 4)) { *w = b[6] | b[7] << 8; *h = b[8] | b[9] << 8; return true; }
    if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
        uint32_t hs = (uint32_t)(b[14] | b[15] << 8 | b[16] << 16 | (uint32_t)b[17] << 24);
        if (hs == 12) { *w = b[18] | b[19] << 8; *h = b[20] | b[21] << 8; return true; }   /* intestazione vecchia (OS/2) */
        if (hs < 40) return false;
        int32_t x = (int32_t)(b[18] | b[19] << 8 | b[20] << 16 | (uint32_t)b[21] << 24), y = (int32_t)(b[22] | b[23] << 8 | b[24] << 16 | (uint32_t)b[25] << 24);
        *w = labs((long)x); *h = labs((long)y); return true;
    }
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {   /* JPEG: si cerca il segmento SOF */
        size_t i = 2;
        while (i + 9 < n) {
            if (b[i] != 0xFF) { i++; continue; }
            unsigned m = b[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
            size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
            if (len < 2) return false;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { *h = b[i + 5] << 8 | b[i + 6]; *w = b[i + 7] << 8 | b[i + 8]; return true; }
            i += 2 + len;
        }
    }
    return false;
}
/* JPEG su file: i segmenti uno dopo l'altro fino a quello con le misure, anche oltre i primi 64 KB
   (metadati grandi: verifica audit V26-02) */
static inline bool jpeg_dims_file(FILE *f, long *w, long *h) {
    unsigned char m[9];
    if (fseek(f, 2, SEEK_SET) != 0) return false;
    for (int guard = 0; guard < 4096; guard++) {
        int c = fgetc(f);
        if (c == EOF) return false;
        if (c != 0xFF) continue;
        int mk;
        do mk = fgetc(f); while (mk == 0xFF);
        if (mk == EOF) return false;
        if (mk == 0xD8 || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) continue;
        if (fread(m, 1, 2, f) != 2) return false;
        long len = m[0] << 8 | m[1];
        if (len < 2) return false;
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            if (fread(m, 1, 5, f) != 5) return false;
            *h = m[1] << 8 | m[2];
            *w = m[3] << 8 | m[4];
            return true;
        }
        if (fseek(f, len - 2, SEEK_CUR) != 0) return false;
    }
    return false;
}

