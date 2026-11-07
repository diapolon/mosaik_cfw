/* envtool: cambia una variabile dell'ambiente di U-Boot sulla SD, in modo sicuro.

     envtool ENV ENV_REDUND NOME VALORE      es. envtool /dev/mmcblk0p2 /dev/mmcblk0p3 loglevel 4

   L'ambiente è "ridondante": due copie (partizioni env ed env-redund), ognuna con
   CRC32 (4 byte) + numero di versione (1 byte) + variabili "nome=valore\0...\0\0".
   U-Boot usa la copia valida con il numero più alto. Si legge la copia in uso, si cambia la
   variabile e si scrive sull'ALTRA copia con il numero successivo: se la scrittura non va a
   buon fine, U-Boot continua a usare quella di prima, che resta intatta.

   Uscita: 0 cambiato, 1 già con quel valore, 2 errore (nessuna scrittura). */
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIZE 131072   /* misura dell'ambiente nel firmware TrimUI (env.fex) */

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

static bool valid(const uint8_t *e) {
    uint32_t c = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
    return crc32(e + 5, SIZE - 5) == c;
}

static bool load(const char *dev, uint8_t *buf) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return false;
    bool ok = read(fd, buf, SIZE) == SIZE;
    close(fd);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "uso: envtool ENV ENV_REDUND NOME VALORE\n"); return 2; }
    static uint8_t a[SIZE], b[SIZE], out[SIZE];
    bool va = load(argv[1], a) && valid(a), vb = load(argv[2], b) && valid(b);
    if (!va && !vb) { fprintf(stderr, "envtool: nessuna copia valida dell'ambiente\n"); return 2; }
    /* copia in uso: la valida con il numero più alto (255 -> 0 conta come più nuovo) */
    bool use_b = vb && (!va || ((uint8_t)(b[4] - a[4]) < 128 && b[4] != a[4]));
    const uint8_t *cur = use_b ? b : a;
    const char *other = use_b ? argv[1] : argv[2];

    /* variabili: nome=valore separate da \0, fine con \0\0 */
    const char *name = argv[3], *value = argv[4];
    size_t nl = strlen(name), o = 5;
    bool found = false, same = false;
    memset(out, 0, SIZE);
    for (size_t i = 5; i < SIZE && cur[i];) {
        const char *var = (const char *)cur + i;
        size_t l = strnlen(var, SIZE - i);
        char tmp[4096];
        if (!strncmp(var, name, nl) && var[nl] == '=') {
            found = true;
            same = !strcmp(var + nl + 1, value);
            snprintf(tmp, sizeof tmp, "%s=%s", name, value);
        } else snprintf(tmp, sizeof tmp, "%.*s", (int)(l < sizeof tmp - 1 ? l : sizeof tmp - 1), var);
        size_t tl = strlen(tmp);
        if (o + tl + 2 > SIZE) { fprintf(stderr, "envtool: ambiente troppo grande\n"); return 2; }
        memcpy(out + o, tmp, tl + 1);
        o += tl + 1;
        i += l + 1;
    }
    if (!found) { fprintf(stderr, "envtool: variabile %s assente\n", name); return 2; }
    if (same) { printf("%s=%s già impostato\n", name, value); return 1; }
    out[4] = (uint8_t)(cur[4] + 1);
    uint32_t c = crc32(out + 5, SIZE - 5);
    out[0] = c & 255; out[1] = c >> 8 & 255; out[2] = c >> 16 & 255; out[3] = c >> 24 & 255;
    int fd = open(other, O_WRONLY | O_SYNC);
    if (fd < 0 || write(fd, out, SIZE) != SIZE) { fprintf(stderr, "envtool: scrittura non riuscita\n"); return 2; }
    fsync(fd);
    close(fd);
    /* verifica: si rilegge e si controlla */
    if (!load(other, b) || memcmp(b, out, SIZE)) { fprintf(stderr, "envtool: verifica non riuscita\n"); return 2; }
    printf("%s=%s scritto in %s (versione %d)\n", name, value, other, out[4]);
    return 0;
}
