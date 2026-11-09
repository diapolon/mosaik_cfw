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
    _Atomic bool running, finished, cancel;
    _Atomic int done, total;
    _Atomic int count[RC_COUNT];
} romcheck_status_t;

bool romcheck_start(const char *root, const game_t *games, int n);
void romcheck_cancel(void);
const romcheck_status_t *romcheck_status(void);
void romcheck_join(void);

/* stato e dettaglio salvati di un gioco (dettaglio: file mancanti o parent) */
int romcheck_load(const char *root, game_t *g, int n);
bool romcheck_detail(const char *root, const game_t *g, char *out, int outn);

/* Diagnosi di un solo gioco: cosa impedisce di avviarlo e quale file serve. Non modifica nulla.
   I valori sono anche i numeri delle voci diag.headN e diag.bodyN delle lingue. */
enum {
    DG_OK,          /* nessun problema trovato dal controllo rapido */
    DG_NO_CORE,     /* il file del core non c'è */
    DG_CORE_BAD,    /* il core c'è ma non si carica */
    DG_NO_ROM,      /* il file del gioco non c'è più */
    DG_BAD_ZIP,     /* lo zip non si legge */
    DG_NO_BIOS,     /* file: lo zip del BIOS */
    DG_NO_PARENT,   /* file: lo zip del gioco principale */
    DG_MISSING,     /* file: i file mancanti nello zip */
    DG_BAD_VERSION, /* file: i file di un'altra versione */
    DG_NOT_IN_DAT,  /* il nome non è nel catalogo dell'emulatore */
    DG_UNKNOWN,     /* 7z, o emulatore senza elenco dei file */
    DG_COUNT
};
typedef struct {
    int kind;
    char file[300];
    char where[300];   /* cartella delle ROM, o nome dell'emulatore se si parla di romset */
} rdiag_t;
void romcheck_diagnose(const char *root, const game_t *g, const char *core_path, bool core_failed, rdiag_t *out);
