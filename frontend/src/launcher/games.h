/* Elenco giochi: ROM trovate nelle cartelle degli emulatori, catalogo dei giochi
   mancanti (facoltativo) e giochi Linux. */
#pragma once
#include <stdbool.h>

typedef struct {
    char name[64];       /* nome ROM senza estensione, o cartella del gioco Linux */
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
} game_t;

/* Emulatori: un file Emulators/<id>.json per ciascuno.
   { "name": "FinalBurn Neo", "core": "fbneo_libretro", "roms": "Roms/FBNEO",
     "extensions": ["zip", "7z"], "index": "Dats/fbneo.tsv", "order": 10 }
   Tutti i campi tranne "name" sono facoltativi. */
typedef struct {
    char id[16], name[48], core[64], roms[256], index[256];
    char exts[8][12];
    int nexts, order;
} emulator_t;

extern emulator_t EMUS[16];
extern int NEMUS;

int emulators_load(const char *root);
const emulator_t *emulator_find(const char *id);

/* include_missing: aggiunge i giochi del catalogo senza ROM (solo i "parent", non i cloni) */
int games_scan(const char *root, game_t **out, bool include_missing);
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
void games_set_coll(const char *root, game_t *g, int ci, bool in);
/* crea la collezione (o trova quella con lo stesso nome); restituisce l'indice, -1 se fallisce */
int games_new_coll(const char *root, const char *name, game_t *g, int n);
