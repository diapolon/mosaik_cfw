/* Audio adattato allo schermo. Un gioco a 60,0 Hz su uno schermo a circa 59,4 Hz produce suono
   per 60 fotogrammi ma ne mostra 59,4: la scorta di audio si esaurisce e si sentono scricchiolii.
   Qui il suono si allunga o si accorcia di pochissimo (massimo 2%, non si sente) per tenere
   la scorta intorno all'obiettivo: stessa idea del "dynamic rate control" di RetroArch.
   Interpolazione lineare, con la posizione frazionaria conservata tra una chiamata e l'altra. */
#pragma once
#include <stdint.h>

#define RATE_MAX_DEV 0.02   /* scostamento massimo della velocità dell'audio */

typedef struct {
    double pos;       /* posizione nel blocco in ingresso, 0 = ultimo campione del blocco prima */
    double err;       /* errore della scorta, mediato */
    int16_t last[2];  /* ultimo campione del blocco precedente */
    int ready;
} ratectl_t;

/* in: frames campioni stereo; out: spazio per almeno frames * 1.03 + 4 campioni. queued e target
   sono la scorta attuale e quella voluta, nella stessa unità. Restituisce i campioni scritti. */
static inline int ratectl_run(ratectl_t *r, const int16_t *in, int frames, double queued, double target, int16_t *out) {
    if (frames <= 0) return 0;
    if (!r->ready) { r->last[0] = in[0]; r->last[1] = in[1]; r->pos = 0; r->err = 0; r->ready = 1; }
    double e = target > 0 ? (queued - target) / target : 0;
    if (e > 1) e = 1;
    if (e < -1) e = -1;
    /* media lenta: la velocità non deve oscillare da un fotogramma all'altro */
    r->err += (e - r->err) * 0.05;
    double step = 1.0 + RATE_MAX_DEV * r->err;
    int n = 0;
    double pos = r->pos;
    while (pos < frames) {
        int i = (int)pos;
        double f = pos - i;
        /* i = 0 interpola tra l'ultimo campione del blocco precedente e il primo di questo */
        const int16_t *a = i == 0 ? r->last : in + (i - 1) * 2, *b = in + i * 2;
        out[n * 2] = (int16_t)(a[0] + (b[0] - a[0]) * f);
        out[n * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
        n++;
        pos += step;
    }
    r->pos = pos - frames;
    r->last[0] = in[(frames - 1) * 2];
    r->last[1] = in[(frames - 1) * 2 + 1];
    return n;
}
