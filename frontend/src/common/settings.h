/* Impostazioni della console, condivise da launcher e runner. */
#pragma once
#include <stdbool.h>

enum { CPU_SAVE, CPU_BALANCED, CPU_PERF };
enum { ASPECT_ORIGINAL, ASPECT_INTEGER, ASPECT_FULL };

typedef struct {
    char theme[64];
    char variant[16];
    char device[16];     /* "smartpro" | "brickpro" */
    char lang[8];        /* file Lang/<lang>.json */
    int brightness;      /* 1..10 */
    int volume;          /* 0..20 */
    int cpu_profile;
    int autosleep_min;   /* 0 = mai */
    int saver;           /* salvaschermo: 0 no, 1-2 schermate dopo 2/5 min, 3-4 schermo spento dopo 2/5 min */
    int aspect;
    char filter[32];     /* cartella Filters/<filter> */
    bool bezel;          /* cornici attorno ai giochi */
    int sort_mode;       /* libreria: 0 nome, 1 anno, 2 più giocati, 3 ultimi giocati */
    bool show_missing;   /* libreria: anche i giochi del catalogo senza ROM */
    int dup_mode;        /* giochi doppi: 0 tutti, 1 preferisci FBNeo, 2 preferisci MAME */
    bool resume_last;
    bool wifi;
    int music_vol;       /* musica del launcher: 0 spenta, 1..10 */
    bool ext_p1;         /* controller esterno come giocatore 1 (la console diventa il 2) */
} settings_t;

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s, const char *root);
bool settings_save(const settings_t *s, const char *root);

/* Risoluzione dello schermo del modello. */
void device_size(const char *device, int *w, int *h);
const char *device_label(const char *device);
