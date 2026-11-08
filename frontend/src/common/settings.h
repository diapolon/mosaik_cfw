/* Impostazioni della console, condivise da launcher e runner. */
#pragma once
#include <stdbool.h>

enum { CPU_SAVE, CPU_BALANCED, CPU_PERF };
enum { ASPECT_ORIGINAL, ASPECT_INTEGER, ASPECT_FULL };
enum { XK_NONE, XK_SAVE, XK_LOAD, XK_SHOT };        /* tasti F1 e F2 durante il gioco */
enum { XH_LAUNCHER, XH_MENU, XH_NONE };             /* tasto HOME laterale durante il gioco */

typedef struct {
    char theme[64];
    char variant[16];
    char device[16];     /* "smartpro" | "brickpro" */
    char lang[8];        /* file Lang/<lang>.json */
    int brightness;      /* 1..10 */
    int volume;          /* 0..20 */
    int cpu_profile;
    int autosleep_min;   /* 0 = mai */
    int autosuspend_min; /* sospensione nel launcher, 0 = mai */
    int update_channel;  /* aggiornamenti: 0 stabile, 1 sviluppo */
    bool update_notify;  /* controllo automatico una volta al giorno, con avviso in home */
    bool setup_done;     /* primo avvio guidato già fatto */
    int key_f1, key_f2;  /* XK_* */
    int key_home;        /* XH_* */
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
    int led_mode;        /* LED: 0 spenti, 1 fissi, 2 respiro, 3 pulsazione */
    int led_color;       /* indice in LED_COLORS (0 = colore del tema) */
    int led_bright;      /* 1..10 */
    int grid_columns;    /* miniature per riga in libreria: 0 = quelle del tema, altrimenti 3..7 */
    int home_size;       /* copertine in home: 0 piccole, 1 normali, 2 grandi */
    int log_level;       /* registro diagnostico: 0 spento, 1 essenziale, 2 prestazioni, 3 completo */
    char tz[40];         /* fuso orario, nome come "Europe/Rome" (vedi TIMEZONES in settings.c) */
} settings_t;

void settings_defaults(settings_t *s);
/* Fusi orari: l'orologio della console è in UTC, l'ora mostrata è quella del fuso scelto.
   Regole POSIX (variabile TZ), così non servono i file zoneinfo. */
typedef struct { const char *key; unsigned rgb; } led_color_t;   /* key: nome nei file di lingua */
extern const led_color_t LED_COLORS[];
extern const int NLED_COLORS;
typedef struct { const char *id, *posix; } timezone_t;
extern const timezone_t TIMEZONES[];
extern const int NTIMEZONES;
int settings_tz_index(const settings_t *s);
void settings_apply_tz(const settings_t *s);   /* imposta TZ per questo processo e i figli */
bool settings_load(settings_t *s, const char *root);
bool settings_save(const settings_t *s, const char *root);

/* Risoluzione dello schermo del modello. */
void device_size(const char *device, int *w, int *h);
const char *device_label(const char *device);
