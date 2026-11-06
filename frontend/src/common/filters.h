/* Filtri video: cartelle Filters/<id>/filter.json sulla SD.

   {
     "name": { "it": "CRT", "en": "CRT" },   // oppure una stringa
     "order": 40,                            // posizione nell'elenco
     "smooth": false,                        // filtro lineare sull'immagine del gioco
     "mask": "mask.png",                     // facoltativa, ripetuta sull'immagine
     "mask_scale": "pixel",                  // "pixel": una ripetizione per pixel del gioco; "screen": dello schermo
     "mask_strength": 0.6,                   // 0..1, quanto la maschera scurisce
     "vignette": 0.25,                       // 0..1, bordi più scuri
     "shader": null                          // riservato agli shader GLSL (rendering OpenGL ES)
   } */
#pragma once
#include <stdbool.h>

typedef struct {
    char id[32], name[64], mask[1100], shader[1100];
    bool smooth, mask_per_pixel;
    float strength, vignette;
    int order;
} filter_t;

int filters_list(const char *root, const char *lang, filter_t *out, int max);
bool filter_load(const char *root, const char *lang, const char *id, filter_t *f);
/* impostazioni vecchie: 0 nitido, 1 morbido, 2 scanline */
const char *filter_legacy_id(int n);
