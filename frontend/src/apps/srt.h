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

/* testo di una battuta: senza i tag (<i>, {\an8}), Latin-1 portato in UTF-8, righe unite con \n */
static char *sub_clean(const char *in, bool latin1) {
    size_t n = strlen(in);
    char *out = malloc(n * 2 + 1), *o = out;
    if (!out) return NULL;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (*p == '<') { const char *e = strchr((const char *)p, '>'); if (e) { p = (const unsigned char *)e; continue; } }
        if (*p == '{' && p[1] == '\\') { const char *e = strchr((const char *)p, '}'); if (e) { p = (const unsigned char *)e; continue; } }
        if (*p == '\r') continue;
        if (latin1 && *p >= 0x80) { *o++ = (char)(0xC0 | (*p >> 6)); *o++ = (char)(0x80 | (*p & 0x3F)); }
        else *o++ = (char)*p;
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
   free() di ogni testo e dell'elenco; *latin1 dice se il file non era UTF-8.
   Un blocco: numero (facoltativo), "inizio --> fine", righe di testo, fino a una riga vuota (anche
   con spazi o \r) o alla riga dei tempi del blocco dopo. Prima la fine del testo era solo una riga
   che cominciava con \r: con gli a capo Unix tutto il file finiva in una battuta (audit 0.9.88, APP-01) */
static int srt_parse(char *s, sub_t **out, bool *latin1_out) {
    sub_t *subs = NULL;
    int nsubs = 0, cap = 0;
    *out = NULL;
    if (latin1_out) *latin1_out = false;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;   /* BOM */
    bool latin1 = !utf8_ok((const unsigned char *)s);
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
        char *clean = sub_clean(text, latin1);
        if (!clean || !clean[0]) { free(clean); continue; }
        subs[nsubs++] = (sub_t){ a, b, clean };
    }
    free(L);
    if (nsubs > 1) qsort(subs, (size_t)nsubs, sizeof *subs, sub_cmp);
    *out = subs;
    if (latin1_out) *latin1_out = latin1;
    return nsubs;
}

#endif
