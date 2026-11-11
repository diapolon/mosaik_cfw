#include "qrcode.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* correzione M, versioni 1..10: codici di correzione per blocco e numero di blocchi */
static const int ECC_PER_BLOCK[11] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
static const int NUM_BLOCKS[11] = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };

static int side;
static unsigned char mod[QR_MAX_SIDE][QR_MAX_SIDE], fn[QR_MAX_SIDE][QR_MAX_SIDE];

static int raw_modules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int na = ver / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}
static int data_codewords(int ver) { return raw_modules(ver) / 8 - ECC_PER_BLOCK[ver] * NUM_BLOCKS[ver]; }

/* ---- Reed-Solomon su GF(256), polinomio 0x11D ---- */
static unsigned gf_mul(unsigned x, unsigned y) {
    unsigned z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return z & 0xFF;
}

static void rs_divisor(int deg, unsigned char *res) {
    memset(res, 0, (size_t)deg);
    res[deg - 1] = 1;
    unsigned root = 1;
    for (int i = 0; i < deg; i++) {
        for (int j = 0; j < deg; j++) {
            res[j] = (unsigned char)gf_mul(res[j], root);
            if (j + 1 < deg) res[j] ^= res[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const unsigned char *data, int len, const unsigned char *div, int deg, unsigned char *res) {
    memset(res, 0, (size_t)deg);
    for (int i = 0; i < len; i++) {
        unsigned char f = data[i] ^ res[0];
        memmove(res, res + 1, (size_t)deg - 1);
        res[deg - 1] = 0;
        for (int j = 0; j < deg; j++) res[j] ^= (unsigned char)gf_mul(div[j], f);
    }
}

/* ---- disegno ---- */
static void set_fn(int x, int y, bool dark) {
    if (x < 0 || y < 0 || x >= side || y >= side) return;
    mod[y][x] = dark;
    fn[y][x] = 1;
}

static void finder(int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            set_fn(cx + dx, cy + dy, d != 2 && d != 4);
        }
}

static void alignment(int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            set_fn(cx + dx, cy + dy, d != 1);
        }
}

static void format_bits(int mask) {
    int data = 0 << 3 | mask;   /* livello M = 00 */
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    for (int i = 0; i <= 5; i++) set_fn(8, i, (bits >> i) & 1);
    set_fn(8, 7, (bits >> 6) & 1);
    set_fn(8, 8, (bits >> 7) & 1);
    set_fn(7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) set_fn(14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++) set_fn(side - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) set_fn(8, side - 15 + i, (bits >> i) & 1);
    set_fn(8, side - 8, true);
}

static void version_bits(int ver) {
    if (ver < 7) return;
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    long bits = (long)ver << 12 | rem;
    for (int i = 0; i < 18; i++) {
        bool b = (bits >> i) & 1;
        int a = side - 11 + i % 3, c = i / 3;
        set_fn(a, c, b);
        set_fn(c, a, b);
    }
}

static void function_patterns(int ver) {
    for (int i = 0; i < side; i++) { set_fn(6, i, i % 2 == 0); set_fn(i, 6, i % 2 == 0); }
    finder(3, 3);
    finder(side - 4, 3);
    finder(3, side - 4);
    if (ver >= 2) {
        int na = ver / 7 + 2, step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2, pos[8];
        pos[0] = 6;
        for (int i = na - 1, p = side - 7; i >= 1; i--, p -= step) pos[i] = p;
        for (int i = 0; i < na; i++)
            for (int j = 0; j < na; j++)
                if (!((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0))) alignment(pos[i], pos[j]);
    }
    format_bits(0);   /* riserva le posizioni; quelle vere dopo la scelta della maschera */
    version_bits(ver);
}

static void draw_codewords(const unsigned char *data, int len) {
    int i = 0;
    for (int right = side - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < side; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, y = ((right + 1) & 2) == 0 ? side - 1 - vert : vert;
                if (!fn[y][x] && i < len * 8) {
                    mod[y][x] = (data[i >> 3] >> (7 - (i & 7))) & 1;
                    i++;
                }
            }
    }
}

static bool mask_bit(int m, int x, int y) {
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(int m) {
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++)
            if (!fn[y][x] && mask_bit(m, x, y)) mod[y][x] ^= 1;
}

static int penalty(void) {
    int p = 0, dark = 0;
    for (int pass = 0; pass < 2; pass++)   /* righe, poi colonne */
        for (int a = 0; a < side; a++) {
            int run = 1;
            for (int b = 1; b <= side; b++) {
                bool end = b == side;
                int cur = end ? -1 : (pass ? mod[b][a] : mod[a][b]), prev = pass ? mod[b - 1][a] : mod[a][b - 1];
                if (!end && cur == prev) run++;
                else { if (run >= 5) p += 3 + run - 5; run = 1; }
            }
            /* motivo simile al riquadro di ricerca: 1011101 con 4 chiari da un lato */
            for (int b = 0; b + 11 <= side; b++) {
                static const unsigned char f1[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 }, f2[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };
                bool m1 = true, m2 = true;
                for (int k = 0; k < 11; k++) {
                    int v = pass ? mod[b + k][a] : mod[a][b + k];
                    m1 = m1 && v == f1[k];
                    m2 = m2 && v == f2[k];
                }
                p += 40 * (m1 + m2);
            }
        }
    for (int y = 0; y + 1 < side; y++)
        for (int x = 0; x + 1 < side; x++) {
            int c = mod[y][x];
            if (c == mod[y][x + 1] && c == mod[y + 1][x] && c == mod[y + 1][x + 1]) p += 3;
        }
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) dark += mod[y][x];
    int total = side * side, k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    return p + k * 10;
}

int qr_encode(const char *text, unsigned char out[QR_MAX_SIDE * QR_MAX_SIDE]) {
    int len = (int)strlen(text), ver;
    for (ver = 1; ver <= 10; ver++) {
        int cc = ver < 10 ? 8 : 16;
        if (4 + cc + 8 * len <= data_codewords(ver) * 8) break;
    }
    if (ver > 10) return 0;
    side = ver * 4 + 17;
    /* dati: modo byte, lunghezza, testo, terminatore, riempimento */
    int dcw = data_codewords(ver), cc = ver < 10 ? 8 : 16;
    unsigned char data[400] = { 0 };
    int bit = 0;
#define PUT(v, n) do { for (int _i = (n) - 1; _i >= 0; _i--, bit++) if (((v) >> _i) & 1) data[bit >> 3] |= (unsigned char)(0x80 >> (bit & 7)); } while (0)
    PUT(4, 4);
    PUT(len, cc);
    for (int i = 0; i < len; i++) PUT((unsigned char)text[i], 8);
    int term = dcw * 8 - bit < 4 ? dcw * 8 - bit : 4;
    bit += term;
    bit = (bit + 7) / 8 * 8;
    for (int pad = 0xEC; bit < dcw * 8; pad ^= 0xEC ^ 0x11) PUT(pad, 8);
#undef PUT
    /* blocchi con la loro correzione, poi intrecciati */
    int nb = NUM_BLOCKS[ver], eccl = ECC_PER_BLOCK[ver], raw = raw_modules(ver) / 8;
    int nshort = nb - raw % nb, shortlen = raw / nb;
    unsigned char blocks[8][160], div[32], ecc[32], res[400];
    rs_divisor(eccl, div);
    for (int i = 0, k = 0; i < nb; i++) {
        int dl = shortlen - eccl + (i < nshort ? 0 : 1);
        rs_remainder(data + k, dl, div, eccl, ecc);
        memcpy(blocks[i], data + k, (size_t)dl);
        k += dl;
        if (i < nshort) { blocks[i][dl] = 0; dl++; }
        memcpy(blocks[i] + dl, ecc, (size_t)eccl);
    }
    int n = 0;
    for (int i = 0; i < shortlen + 1; i++)
        for (int j = 0; j < nb; j++)
            if (i != shortlen - eccl || j >= nshort) res[n++] = blocks[j][i];
    memset(mod, 0, sizeof mod);
    memset(fn, 0, sizeof fn);
    function_patterns(ver);
    draw_codewords(res, n);
    int best = -1, bestp = 0;
    for (int m = 0; m < 8; m++) {
        apply_mask(m);
        format_bits(m);
        int p = penalty();
        if (best < 0 || p < bestp) { best = m; bestp = p; }
        apply_mask(m);   /* la maschera si toglie rifacendola */
    }
    apply_mask(best);
    format_bits(best);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) out[y * side + x] = mod[y][x];
    return side;
}
