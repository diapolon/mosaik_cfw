/* Scraper: stessa logica di tools/scrape.py, in un thread separato. Fonti: ArcadeDB per nome ROM,
   miniature libretro per titolo come riserva. Dalla 0.9.48 il lavoro sta nell'app dello Store
   Apps/scraper (src/apps/scraper.c con scraper.c); nel launcher la stessa interfaccia la dà
   scrape_client.c, che avvia l'app e ne legge l'avanzamento. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "games.h"

enum { SCRAPE_IDLE, SCRAPE_INFO, SCRAPE_IMAGES, SCRAPE_DONE, SCRAPE_FAILED };

typedef struct {
    _Atomic int phase;
    _Atomic int done, total;        /* giochi elaborati / da elaborare */
    _Atomic int saved, missing;     /* immagini scaricate / non trovate */
    _Atomic bool cancel;
} scrape_status_t;

/* only: gli emulatori da completare, id separati da spazi ("fbneo snes"); NULL o "" = tutti */
bool scraper_start(const char *root, const game_t *games, int n, const char *only);
void scraper_cancel(void);
const scrape_status_t *scraper_status(void);
/* attende la fine del thread (dopo DONE o FAILED) */
void scraper_join(void);

/* solo nel launcher (scrape_client.c): l'app è installata? e dove sta */
bool scraper_installed(const char *root);
void scraper_app_path(const char *root, char *out, size_t n);
