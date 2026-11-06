/* Verifica delle ROM: confronta i file dentro ogni zip con quelli attesi dal DAT
   (Dats/<emulatore>.roms, generato da tools/build_index.py). Legge solo l'indice dello
   zip (nome, dimensione, CRC), senza decomprimere. Risultati in System/romcheck.tsv. */
#pragma once
#include <stdbool.h>
#include "games.h"

enum {
    RC_NONE,        /* mai verificato */
    RC_OK,
    RC_MISSING,     /* mancano file */
    RC_BADCRC,      /* file presenti ma diversi: versione del romset diversa */
    RC_NOPARENT,    /* servono il gioco "parent" o il BIOS, che non ci sono */
    RC_NOTINDAT,    /* il nome non è nel DAT dell'emulatore */
    RC_UNCHECKED,   /* formato non verificabile (7z) o emulatore senza elenco dei file */
    RC_COUNT
};

typedef struct {
    volatile bool running, finished, cancel;
    volatile int done, total;
    volatile int count[RC_COUNT];
} romcheck_status_t;

bool romcheck_start(const char *root, const game_t *games, int n);
void romcheck_cancel(void);
const romcheck_status_t *romcheck_status(void);
void romcheck_join(void);

/* stato e dettaglio salvati di un gioco (dettaglio: file mancanti o parent) */
int romcheck_load(const char *root, game_t *g, int n);
bool romcheck_detail(const char *root, const game_t *g, char *out, int outn);
