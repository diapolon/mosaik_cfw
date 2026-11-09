/* Scraper nel launcher: stessa logica di tools/scrape.py, in un thread separato.
   Fonti: ArcadeDB per nome ROM, miniature libretro per titolo come riserva. */
#pragma once
#include <stdbool.h>
#include "games.h"

enum { SCRAPE_IDLE, SCRAPE_INFO, SCRAPE_IMAGES, SCRAPE_DONE, SCRAPE_FAILED };

typedef struct {
    _Atomic int phase;
    _Atomic int done, total;        /* giochi elaborati / da elaborare */
    _Atomic int saved, missing;     /* immagini scaricate / non trovate */
    _Atomic bool cancel;
} scrape_status_t;

bool scraper_start(const char *root, const game_t *games, int n);
void scraper_cancel(void);
const scrape_status_t *scraper_status(void);
/* attende la fine del thread (dopo DONE o FAILED) */
void scraper_join(void);
