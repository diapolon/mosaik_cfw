/* Sottotitoli .srt dell'app Video: lettura e pulizia del testo, senza SDL né FFmpeg, così
   m2/test-srt.sh li prova da soli. Incluso una volta sola, da video.c (e dalla prova). */
#ifndef SRT_H
#define SRT_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double a, b; char *text; } sub_t;

static bool srt_time(const char *s, double *t) {   /* 00:01:02,345 (anche con il punto) */
    int h, m, sec, ms;
    if (sscanf(s, "%d:%d:%d%*[,.]%d", &h, &m, &sec, &ms) != 4) return false;
    *t = h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    return true;
}

static bool utf8_ok(const unsigned char *p) {
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        p++;
        while (n--) { if ((*p & 0xC0) != 0x80) return false; p++; }
    }
    return true;
}

static char *utf8_put(char *o, unsigned c) {
    if (c < 0x80) *o++ = (char)c;
    else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    else { *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | (c >> 12 & 0x3F)); *o++ = (char)(0x80 | (c >> 6 & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    return o;
}

/* Windows-1252, la codifica dei .srt "ANSI" italiani: 0x80-0x9F sono apostrofi, virgolette, trattini,
   euro (con Latin-1 sparivano: «l'uomo» diventava «luomo», audit 0.9.88 APP-02); 0 = non usato */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

/* una riga Windows-1252 (o Latin-1) in UTF-8, in memoria nuova */
static char *cp1252_to_utf8(const char *in) {
    char *out = malloc(strlen(in) * 3 + 1), *o = out;
    if (!out) return NULL;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned c = *p >= 0x80 && *p < 0xA0 ? cp1252_hi[*p - 0x80] : *p;
        if (c) o = utf8_put(o, c);
    }
    *o = 0;
    return out;
}

/* UTF-16 (dopo il BOM) in UTF-8, in memoria nuova; le coppie surrogate diventano un carattere solo */
static char *utf16_to_utf8(const unsigned char *b, size_t len, bool be) {
    char *out = malloc(len / 2 * 3 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i + 1 < len; i += 2) {
        unsigned c = be ? (unsigned)b[i] << 8 | b[i + 1] : (unsigned)b[i + 1] << 8 | b[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
            unsigned d = be ? (unsigned)b[i + 2] << 8 | b[i + 3] : (unsigned)b[i + 3] << 8 | b[i + 2];
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
        }
        if (c && (c < 0xD800 || c >= 0xE000)) o = utf8_put(o, c);   /* surrogate spaiate e zeri: via */
    }
    *o = 0;
    return out;
}

/* testo di una battuta (già UTF-8): senza i tag (<i>, {\an8}), righe unite con \n */
static char *sub_clean(const char *in) {
    size_t n = strlen(in);
    char *out = malloc(n + 1), *o = out;
    if (!out) return NULL;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (*p == '<') { const char *e = strchr((const char *)p, '>'); if (e) { p = (const unsigned char *)e; continue; } }
        if (*p == '{' && p[1] == '\\') { const char *e = strchr((const char *)p, '}'); if (e) { p = (const unsigned char *)e; continue; } }
        if (*p == '\r') continue;
        *o++ = (char)*p;
    }
    while (o > out && (o[-1] == '\n' || o[-1] == ' ')) o--;
    *o = 0;
    return out;
}

static int sub_cmp(const void *x, const void *y) {
    double a = ((const sub_t *)x)->a, b = ((const sub_t *)y)->a;
    return a < b ? -1 : a > b;
}

/* riga "inizio --> fine" */
static bool srt_timing(const char *line, double *a, double *b) {
    const char *arrow = strstr(line, "-->");
    return arrow && srt_time(line + strspn(line, " \t"), a) && srt_time(arrow + 3 + strspn(arrow + 3, " \t"), b);
}

static bool srt_blank(const char *line) { return line[strspn(line, " \t\r")] == 0; }

static bool srt_number(const char *line) {
    const char *p = line + strspn(line, " \t");
    size_t n = strspn(p, "0123456789");
    return n > 0 && srt_blank(p + n);
}

/* legge un .srt intero (s viene modificato): battute in *out, ordinate per tempo, da liberare con
   free() di ogni testo e dell'elenco; *latin1 dice se almeno una riga non era UTF-8 (letta come
   Windows-1252).
   Un blocco: numero (facoltativo), "inizio --> fine", righe di testo, fino a una riga vuota (anche
   con spazi o \r) o alla riga dei tempi del blocco dopo. Prima la fine del testo era solo una riga
   che cominciava con \r: con gli a capo Unix tutto il file finiva in una battuta (audit 0.9.88, APP-01) */
static int srt_parse(char *s, sub_t **out, bool *latin1_out) {
    sub_t *subs = NULL;
    int nsubs = 0, cap = 0;
    *out = NULL;
    if (latin1_out) *latin1_out = false;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;   /* BOM */
    bool latin1 = false;
    if (!strchr(s, '\n')) for (char *p = s; *p; p++) if (*p == '\r') *p = '\n';   /* a capo del vecchio Mac */
    /* le righe, separate una volta sola */
    size_t nl = 1;
    for (const char *p = s; *p; p++) nl += *p == '\n';
    char **L = malloc(nl * sizeof *L);
    if (!L) return 0;
    size_t n = 0;
    for (char *p = s; p; ) {
        L[n++] = p;
        p = strchr(p, '\n');
        if (p) *p++ = 0;
    }
    /* codifica decisa riga per riga: un solo byte non UTF-8 faceva leggere come Latin-1 tutto il
       file, e le lettere accentate giuste diventavano «PerchÃ©» (APP-02) */
    char **conv = calloc(n, sizeof *conv);
    for (size_t i = 0; conv && i < n; i++)
        if (!utf8_ok((const unsigned char *)L[i]) && (conv[i] = cp1252_to_utf8(L[i]))) { L[i] = conv[i]; latin1 = true; }
    for (size_t i = 0; i < n; ) {
        double a, b;
        if (!srt_timing(L[i], &a, &b)) { i++; continue; }
        char text[600] = "";
        size_t tl = 0, j = i + 1;
        for (; j < n && !srt_blank(L[j]) && !srt_timing(L[j], &(double){0}, &(double){0}); j++) {
            /* blocchi senza riga vuota in mezzo: il numero del blocco dopo non è testo */
            if (srt_number(L[j]) && j + 1 < n && srt_timing(L[j + 1], &(double){0}, &(double){0})) break;
            if (tl < sizeof text - 2) tl += (size_t)snprintf(text + tl, sizeof text - tl, "%s%s", tl ? "\n" : "", L[j]);
            if (tl >= sizeof text) tl = sizeof text - 1;
        }
        i = j;
        if (nsubs == cap) {
            int nc = cap ? cap * 2 : 512;
            if (nc > 20000) break;
            sub_t *ns = realloc(subs, (size_t)nc * sizeof *ns);
            if (!ns) break;
            subs = ns;
            cap = nc;
        }
        char *clean = sub_clean(text);
        if (!clean || !clean[0]) { free(clean); continue; }
        subs[nsubs++] = (sub_t){ a, b, clean };
    }
    for (size_t i = 0; conv && i < n; i++) free(conv[i]);
    free(conv);
    free(L);
    if (nsubs > 1) qsort(subs, (size_t)nsubs, sizeof *subs, sub_cmp);
    *out = subs;
    if (latin1_out) *latin1_out = latin1;
    return nsubs;
}

/* come srt_parse, con la misura del file: riconosce anche UTF-16 (BOM FF FE o FE FF), che ha byte a
   zero e con srt_parse dava 0 battute (APP-02). buf deve avere un byte in più per lo zero finale. */
#define SRT_HAS_BUF 1
static int srt_parse_buf(char *buf, size_t len, sub_t **out, bool *latin1_out) {
    const unsigned char *b = (const unsigned char *)buf;
    if (len >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        char *u = utf16_to_utf8(b + 2, len - 2, b[0] == 0xFE);
        *out = NULL;
        if (latin1_out) *latin1_out = false;
        if (!u) return 0;
        int n = srt_parse(u, out, latin1_out);
        free(u);
        return n;
    }
    buf[len] = 0;
    return srt_parse(buf, out, latin1_out);
}

#endif
