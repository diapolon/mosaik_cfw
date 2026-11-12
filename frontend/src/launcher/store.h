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
   sulla scheda si installa senza rete (store_offline_*). */
#pragma once
#include <stdbool.h>

enum { ST_IDLE, ST_LOADING, ST_READY, ST_WORKING, ST_DONE, ST_FAILED };
enum { SE_NONE, SE_NETWORK, SE_SIGNATURE, SE_CATALOG, SE_SPACE, SE_VERIFY, SE_WRITE, SE_CORE, SE_CANCEL };
enum { PKG_ABSENT, PKG_INSTALLED, PKG_UPDATE, PKG_OLD };   /* PKG_OLD: arrivato col sistema, senza versione */

typedef struct {
    char id[32], name[64], label[32], version[24], file[128], sha[65];
    char systems[200];           /* "Sega Mega Drive, Master System, Game Gear" */
    long long size, installed;   /* byte dello zip e dei file installati */
    int state;                   /* PKG_* */
} store_pkg_t;

typedef struct {
    _Atomic int phase;           /* ST_*: del catalogo (LOADING, READY, FAILED) o di un lavoro */
    _Atomic int error;           /* SE_* */
    _Atomic long long done, total;
    _Atomic bool online;         /* catalogo letto ora dalla rete (altrimenti la copia sulla scheda) */
    _Atomic bool cancel;
    char working[64];            /* nome del pacchetto in lavorazione */
} store_status_t;

/* carica subito il catalogo tenuto sulla scheda e, in un thread, quello in rete (se online) */
void store_open(const char *root, int development, bool online);
const store_status_t *store_status(void);
int store_count(void);
const store_pkg_t *store_pkg(int i);   /* copia stabile finché non si richiama store_open/store_* */
void store_install(int i);              /* in un thread: installa o aggiorna */
void store_remove(int i);               /* in un thread: toglie i file del pacchetto */
void store_cancel(void);
void store_join(void);
/* dopo un'installazione o una rimozione: emulatori cambiati, la libreria va riletta */
bool store_changed(void);
/* stato dei pacchetti ricalcolato dai file della scheda (dopo store_join) */
void store_refresh(void);

/* pacchetti copiati sulla scheda (store-*.zip nella cartella principale o in Arcade): quello da
   proporre all'avvio, se c'è (nome e versione), e la sua installazione (bloccante) */
bool store_offline_find(const char *root, char *path, int pn, char *name, int nn, char *label, int ln);
bool store_offline_install(const char *root, const char *path);
