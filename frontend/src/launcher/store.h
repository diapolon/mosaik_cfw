/* Store: gli emulatori sono pacchetti da installare, aggiornare o togliere dalla console, così
   il sistema di base resta piccolo e ogni scheda ha solo quello che serve.

   Il catalogo (store.json, firmato in store.sig con la chiave degli aggiornamenti) sta allo
   stesso indirizzo degli aggiornamenti, nel canale scelto. Ogni pacchetto è uno zip con
   pacchetto.json e i file con i percorsi della scheda (solo Cores, Emulators, Dats, Apps, Themes,
   Filters, Bezels). Installazione: lo zip si scarica in System/.store (riprendendo da dove si era
   fermato), si controllano misura e SHA-256 del catalogo, i file si scrivono tutti come
   ".nome.part" e solo alla fine prendono il posto dei vecchi; un core che non si carica si toglie.
   La versione installata va in System/pacchetti/<id>.txt con l'elenco dei file, che servono per
   aggiornare e togliere. ROM, BIOS e salvataggi non si toccano mai.

   Un emulatore arrivato prima dello Store (con il sistema) risulta installato senza versione:
   "Aggiorna" lo porta alla versione del catalogo. Un pacchetto store-<id>-<versione>.zip copiato
   sulla scheda si installa senza rete (store_offline_*), solo se un catalogo firmato lo elenca con
   la stessa impronta: un pacchetto sconosciuto si rifiuta (un'app gira come root). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { ST_IDLE, ST_LOADING, ST_READY, ST_WORKING, ST_DONE, ST_FAILED };
enum { SE_NONE, SE_NETWORK, SE_SIGNATURE, SE_CATALOG, SE_SPACE, SE_VERIFY, SE_WRITE, SE_CORE, SE_CANCEL, SE_UNSIGNED, SE_SYSTEM };
/* PKG_OLD: arrivato col sistema, senza versione; PKG_ARCHIVED: archiviato (solo l'emulatore tolto,
   giochi e salvataggi restano), come sulla Switch: si riscarica con un pulsante */
enum { PKG_ABSENT, PKG_INSTALLED, PKG_UPDATE, PKG_OLD, PKG_ARCHIVED };

typedef struct {
    char id[32], name[64], label[32], version[24], file[128], sha[65];
    char systems[200];           /* "Sega Mega Drive, Master System, Game Gear"; per le app la descrizione */
    char title[64];              /* breve, per il riquadro: "Super Nintendo", "Sega MD · SMS · Game Gear" */
    char category[16];           /* arcade, console, apps, themes (in futuro linux) */
    char colors[24];             /* temi: i colori delle varianti, "RRGGBB RRGGBB RRGGBB" */
    char emu[16], art[16], shortname[16];   /* immagine: quella della prima console del pacchetto */
    bool console;
    long long size, installed;   /* byte dello zip e dei file installati */
    int state;                   /* PKG_* */
    /* stato del pacchetto nel catalogo, uguale per i canali Stabile e Sviluppo del sistema */
    bool experimental;           /* "status": "experimental": si installa, ma non è ancora provato sulle console */
    char tested[32];             /* modelli su cui è provato: "brickpro smartpro" */
    char models[32];             /* modelli su cui funziona ("" tutti): "brickpro" per Deadcast (dalla 0.9.67) */
    bool other_model;            /* non funziona su questo modello: lo Store non lo propone */
    char min_system[16];         /* versione minima del sistema, "" se va con tutte */
    bool needs_system;           /* il sistema in uso è più vecchio di min_system: prima va aggiornato */
    char inst_label[32];         /* versione installata, come la mostra la scheda ("" se non installato) */
    int nimg;                    /* immagini della scheda (dalla 0.9.67): file accanto agli zip */
    char img[4][72], img_sha[4][65];
} store_pkg_t;

typedef struct {
    _Atomic int phase;           /* ST_*: del catalogo (LOADING, READY, FAILED) o di un lavoro */
    _Atomic int error;           /* SE_* */
    _Atomic long long done, total;
    _Atomic bool online;         /* catalogo letto ora dalla rete (altrimenti la copia sulla scheda) */
    _Atomic bool cancel;
    char working[64];            /* nome del pacchetto in lavorazione */
    _Atomic int batch_done, batch_total;   /* pacchetti di un backup: fatti e quanti (0: uno solo) */
} store_status_t;

/* lingua dei nomi e delle descrizioni delle app nel catalogo ("texts": {"it": {...}}), prima di store_open */
void store_lang(const char *lang);
/* versione del sistema in uso ("0.9.45"), per i pacchetti che ne chiedono una minima; prima di store_open */
void store_system(const char *version);
bool store_system_older(const char *min);   /* app e giochi: "min_system" del loro app.json/game.json */
/* modello della console ("smartpro", "brickpro"; NULL sul Mac: vanno bene tutti); prima di store_open */
void store_model(const char *model);
/* carica subito il catalogo tenuto sulla scheda e, in un thread, quello in rete (se online) */
void store_open(const char *root, int development, bool online);
const store_status_t *store_status(void);
int store_count(void);
const store_pkg_t *store_pkg(int i);   /* copia stabile finché non si richiama store_open/store_* */
void store_install(int i);              /* in un thread: installa o aggiorna */
/* pacchetti di un backup ripristinato (System/ripristino-pacchetti.txt) ancora da rimettere;
   0 toglie il file. store_install_restore li installa in un thread, dopo store_open */
int store_restore_pending(const char *root);
void store_install_restore(void);
/* in un thread: toglie le parti scelte (Elimina, dalla 0.9.68). STORE_RM_EMU i file del pacchetto;
   STORE_RM_SAVES salvataggi, stati e impostazioni dei giochi (Saves/<console>); STORE_RM_GAMES giochi
   e immagini delle sue console (cartelle dentro Roms, Media/<tipo>/<console>). I BIOS mai. Senza
   l'emulatore ma con giochi o salvataggi rimasti il registro lo ricorda come archiviato ("Scarica di
   nuovo"); senza più niente il registro si toglie. App, temi e giochi Linux: solo STORE_RM_EMU */
#define STORE_RM_PARTS 1   /* per le prove con lo Store di prima */
enum { STORE_RM_EMU = 1, STORE_RM_SAVES = 2, STORE_RM_GAMES = 4, STORE_RM_ALL = 7 };
void store_remove(int i, int parts);
/* anteprima di Archivia ed Elimina: giochi, salvataggi e immagini delle console del pacchetto */
typedef struct { int games, saves, media; long long games_bytes, saves_bytes; } store_usage_t;
void store_usage(int i, store_usage_t *u);
/* immagine k del pacchetto i per la scheda: true con il percorso se è già nella cache
   (System/.store-img); altrimenti, con la rete, la scarica in un thread e la controlla con lo
   SHA-256 del catalogo (si richiama a ogni fotogramma finché non c'è) */
bool store_image(int i, int k, char *out, size_t n);
void store_cancel(void);
void store_join(void);
/* dopo un'installazione o una rimozione: emulatori cambiati, la libreria va riletta */
bool store_changed(void);
/* stato dei pacchetti ricalcolato dai file della scheda (dopo store_join) */
void store_refresh(void);

/* pacchetti copiati sulla scheda (store-*.zip nella cartella principale o in Arcade): quello da
   proporre all'avvio, se c'è (nome e versione), e la sua installazione (bloccante) */
/* rimette i file di un'installazione interrotta (o la conclude): una volta all'avvio */
/* true se non resta nessuna installazione interrotta da chiudere; false: lo Store non scrive */
bool store_recover(const char *root);
/* l'utente sceglie di proseguire: il giornale irrisolto va in store-giornale-guasto.txt */
bool store_recover_force(const char *root);
bool store_offline_find(const char *root, char *path, int pn, char *name, int nn, char *label, int ln);
/* SE_NONE se installato; SE_UNSIGNED se il pacchetto non è in un catalogo firmato (quello sulla
   scheda, o store.json e store.sig copiati accanto allo zip); SE_VERIFY se l'impronta non torna */
int store_offline_install(const char *root, const char *path);
