/* Scarica file da un indirizzo: la parte senza schermata dell'app "Scarica file" (urlget).
   Strumento generico: non conosce fonti, elenchi o raccolte. L'utente scrive gli indirizzi che
   vuole in Apps/urlget/links.txt; un indirizzo può essere un file diretto o un "item" di
   archive.org (https://archive.org/download/<id>/, /details/<id> o /metadata/<id>), di cui
   l'app legge l'elenco dei file e i controlli (MD5, SHA-1).

   Regole di sicurezza:
   - solo HTTPS, anche nei reindirizzamenti (HTTP e loopback solo con URLGET_TEST=1, per le prove);
   - nessun indirizzo privato: la connessione si rifiuta dopo la risoluzione del nome, quindi
     vale anche per i reindirizzamenti e contro il DNS rebinding (loopback, 10/8, 172.16/12,
     192.168/16, 169.254/16, 100.64/10, fc00::/7, fe80::/10);
   - il file si scrive come ".nome.part" e prende il suo nome solo a download finito, con la
     dimensione e l'hash dichiarati controllati;
   - il nome del file viene ripulito (niente percorsi, niente punto iniziale, niente caratteri che
     il FAT non accetta); la cartella di arrivo la sceglie il chiamante. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define UG_NAME_MAX 200
#define UG_META_MAX (8 * 1024 * 1024)       /* oltre, l'elenco dei file non si legge */
#define UG_FILE_MAX 0xFFFFFFFELL            /* limite del FAT32: un byte meno di 4 GiB */

typedef struct {
    char name[UG_NAME_MAX + 1];   /* nome sicuro per la scheda */
    char path[512];               /* percorso dentro l'item (per l'indirizzo) */
    long long size;               /* 0 se ignota */
    char sha1[41];                /* "" se ignoto */
    char md5[33];
} ug_file_t;

enum { UG_BAD, UG_FILE, UG_ITEM };
typedef struct {
    int kind;             /* UG_BAD, UG_FILE o UG_ITEM */
    char origin[200];     /* "https://archive.org" (per un item) */
    char item[128];       /* identificatore dell'item */
    char meta[340];       /* indirizzo dei metadati */
} ug_link_t;
enum { UG_OK, UG_E_URL, UG_E_NET, UG_E_SPACE, UG_E_SIZE, UG_E_HASH, UG_E_IO, UG_E_CANCEL, UG_E_BIG, UG_E_EXISTS };

/* HTTPS e senza credenziali nell'indirizzo (e in prova anche http://127.*) */
bool ug_url_ok(const char *url);
/* UG_FILE: file diretto. UG_ITEM: item di archive.org, con origine, identificatore e indirizzo dei
   metadati. UG_BAD: da scartare */
int ug_classify(const char *url, ug_link_t *out);
/* nome adatto alla scheda dall'ultimo pezzo di path; falso se non ne resta uno valido */
bool ug_safe_name(const char *path, char *out, size_t n);
/* indirizzo del file dentro un item, con i caratteri speciali codificati */
void ug_item_url(const ug_link_t *l, const char *path, char *out, size_t n);
/* nome dell'ultimo pezzo di un indirizzo (per un file diretto), già decodificato */
bool ug_url_name(const char *url, char *out, size_t n);
/* legge il JSON dei metadati: i file originali (non derivati né di servizio), nome ripulito.
   Restituisce quanti sono (0 se nessuno o JSON non valido); *out va liberato con free */
int ug_parse_meta(const char *json, ug_file_t **out);

struct sockaddr;
/* indirizzo che non si contatta mai (privato, loopback, locale al collegamento...) */
bool ug_addr_blocked(const struct sockaddr *sa);

/* avanzamento: restituisce vero per interrompere */
typedef bool (*ug_progress_fn)(void *ud, long long done, long long total);

/* scarica un indirizzo in memoria (metadati), al massimo max byte. cacert: file dei certificati o NULL */
int ug_fetch(const char *url, const char *cacert, size_t max, char **out, ug_progress_fn cb, void *ud);
/* scarica url in dir/nome. size, sha1, md5 dichiarati (0/"" se ignoti) si controllano alla fine */
int ug_download(const char *url, const char *dir, const char *name, long long size,
                const char *sha1, const char *md5, const char *cacert, ug_progress_fn cb, void *ud);
