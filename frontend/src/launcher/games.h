/* Elenco giochi: ROM trovate nelle cartelle degli emulatori, catalogo dei giochi
   mancanti (facoltativo) e giochi Linux. */
#pragma once
#include <stdbool.h>

/* nomi delle ROM: i set No-Intro delle console superano spesso i 64 caratteri */
#define ROM_NAME 128
#define LIST_KEY 208    /* riga "core<TAB>chiave" di preferiti, recenti, collezioni, nascosti */

typedef struct {
    char name[ROM_NAME]; /* nome ROM senza estensione, o cartella del gioco Linux */
    /* identità per salvataggi, stati, statistiche e liste: di solito uguale a name; due ROM
       con lo stesso nome nello stesso emulatore (sottocartelle diverse, o .zip e ROM) ricevono
       "nome [sottocartella]" o "nome [estensione]", tranne la prima (audit prelancio R04) */
    char key[ROM_NAME + 40];
    char title[160];
    char year[8];
    char maker[64];
    char system[32];
    char genre[32];      /* da ArcadeDB (Dats/arcadedb.tsv), se lo scraper è stato eseguito */
    int players;
    char core[16];       /* id dell'emulatore ("fbneo", "mame2003p"...) oppure "linux" */
    char *path;          /* file ROM, oppure launch.sh; vuoto per i giochi mancanti */
    bool vertical;
    bool missing;        /* nel catalogo dell'emulatore ma senza ROM sulla SD */
    bool fav;
    bool hidden;         /* doppione nascosto (stesso gioco in più emulatori), o nascosto dall'utente */
    bool user_hidden;    /* nascosto dall'utente (System/hidden.txt) */
    unsigned colls;      /* collezioni di cui fa parte: bit i = COLLS[i] */
    int recent;          /* posizione tra gli ultimi giocati, 0 = mai */
    int romstatus;       /* esito della verifica ROM (RC_* in romcheck.h), 0 = mai verificata */
    int plays;           /* partite (System/stats.tsv) */
    long seconds;        /* tempo giocato */
    long last_played;    /* data dell'ultima partita (secondi dal 1970) */
    bool key_unsaved;    /* chiave nuova non ancora scritta nel registro: il gioco non si avvia */
} game_t;

/* Emulatori: un file Emulators/<id>.json per ciascuno.
   { "name": "FinalBurn Neo", "core": "fbneo_libretro", "roms": "Roms/FBNEO",
     "extensions": ["zip", "7z"], "index": "Dats/fbneo.tsv", "order": 10 }
   Tutti i campi tranne "name" sono facoltativi. Per le console di casa:
   "kind": "console" (niente ArcadeDB, doppioni né cabinato) e "thumbnails": il nome della
   raccolta di miniature libretro, es. "Sega - Mega Drive - Genesis"; "short": la sigla sulla
   cartuccia della schermata Piattaforme (predefinita: l'id in maiuscolo); "art": il disegno
   nella schermata Piattaforme, "handheld" (portatile verticale) o "handheld-wide" (portatile
   orizzontale), "disc" o "disc-white" (console a disco), "nes", "snes", "md", "sms", "pce" (console
   da casa riconoscibili), altrimenti la console da casa generica; "planned": true
   per una console non ancora supportata: solo nella schermata Piattaforme, in grigio, senza
   core né cartella delle ROM (elenco PLANNED, separato da EMUS). */
typedef struct {
    char id[16], name[48], core[64], roms[256], index[256], thumbs[64], shortname[16], art[16];
    char launch[128];   /* emulatore che non è un core libretro: script sotto Arcade/ che riceve il gioco
                           in ARCADE_ROM (Dreamcast: Cores/deadcast/launch.sh) */
    bool nomenu;        /* "menu": false, l'emulatore ha un suo menu: niente menu di Mosaik (gamewatch) */
    bool folders;       /* "folders": true, ogni cartella dentro quella dei giochi è un gioco (ScummVM) */
    char exts[8][12];
    char maker[24], year[8];   /* "maker", "year": produttore e anno della console (layout galleria) */
    char bios[4][32];   /* "bios": basta uno di questi file in Bios/ (Sega CD: uno per regione) */
    int nexts, nbios, order;
    bool console, planned;
} emulator_t;

#define MAX_EMUS 48   /* emulatori installati; TAB_EMU..TAB_COLL ne lascia 100 */
extern emulator_t EMUS[MAX_EMUS];
extern int NEMUS;
extern emulator_t PLANNED[16];
extern int NPLANNED;

int emulators_load(const char *root);
const emulator_t *emulator_find(const char *id);

/* include_missing: aggiunge i giochi del catalogo senza ROM (solo i "parent", non i cloni) */
/* true: games_scan non scrive i registri delle chiavi (un ripristino non ancora richiuso) */
extern bool games_no_write;
int games_scan(const char *root, game_t **out, bool include_missing);
/* l'ultima games_scan si è fermata per memoria finita: la libreria mostrata è incompleta */
bool games_scan_partial(void);
void games_free(game_t *g, int n);
void games_mark_lists(const char *root, game_t *g, int n);
void games_toggle_fav(const char *root, game_t *g);
void games_add_recent(const char *root, game_t *g);
const char *games_core_label(const char *core);

/* Collezioni: System/collections/<nome>.txt, una riga "core<TAB>rom" per gioco, come i
   preferiti. I giochi nascosti dall'utente sono in System/hidden.txt. */
#define MAX_COLLS 32
extern char COLLS[MAX_COLLS][48];
extern int NCOLLS;
void games_set_hidden(const char *root, game_t *g, bool hidden);
/* Generi modificati dall'utente, in file propri che lo scraper non tocca:
   System/genre-custom.tsv "rom<TAB>genere", System/genre-groups.tsv "genere<TAB>nuovo genere". */
void games_set_genre(const char *root, const char *rom, const char *genre);   /* NULL: torna automatico */
void games_group_genre(const char *root, const char *from, const char *to);   /* rinomina o accorpa */
void games_clear_genre_groups(const char *root);
void games_set_coll(const char *root, game_t *g, int ci, bool in);
/* crea la collezione (o trova quella con lo stesso nome); restituisce l'indice, -1 se fallisce */
int games_new_coll(const char *root, const char *name, game_t *g, int n);
bool games_rename_coll(const char *root, int ci, const char *name, game_t *g, int n);
bool games_delete_coll(const char *root, int ci, game_t *g, int n);
