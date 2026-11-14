/* Impostazioni a strati del runner: Generale (settings.json e JSON dell'emulatore) → Console
   (System/console/<id>.cfg) → Gioco (Saves/<console>/<gioco>.cfg). Ogni strato tiene solo le voci
   scelte lì; quello che manca si eredita dallo strato sopra. Studio: docs/STUDIO-IMPOSTAZIONI-STRATI.md.

   File: "chiave=valore" per riga, con "versione=2" in testa. I .cfg scritti prima (senza
   versione) avevano sempre formato, filtro, cornice e processore anche se non scelti: al primo
   caricamento quelli uguali al valore ereditato si considerano ereditati (gc_migrate). */
#ifndef GAMECFG_H
#define GAMECFG_H

#include <stdbool.h>

#define GC_SP 32

typedef struct {
    bool has_aspect, has_filter, has_bezel, has_cpu, has_map[16];
    int aspect, bezel, cpu, map[16];
    char filter[32];
    int nsp;                       /* parametri dello shader scelti in questo strato */
    char sp_name[GC_SP][48];
    float sp_val[GC_SP];
    int rotate, slot;              /* solo nello strato del gioco */
} gc_layer_t;

/* valori effettivi (o ereditati) delle voci a strati */
typedef struct {
    int aspect, bezel, cpu, map[16];
    char filter[32];
} gc_vals_t;

void gc_clear(gc_layer_t *l);
/* legge il file; false se non c'è. *v2 = il file ha "versione=2". legacy_filter traduce i
   filtri vecchi scritti come numero (può essere NULL) */
bool gc_read(const char *path, gc_layer_t *l, bool *v2, const char *(*legacy_filter)(int));
/* file vecchio: le voci uguali al valore ereditato si tolgono (erano copiate, non scelte) */
void gc_migrate(gc_layer_t *l, const gc_vals_t *inherited);
/* scrive con un file provvisorio; se lo strato è vuoto toglie il file. false se non riesce */
bool gc_write(const char *path, const gc_layer_t *l);
bool gc_empty(const gc_layer_t *l);
/* base con sopra lo strato l (NULL = nessuno) */
void gc_apply(gc_vals_t *v, const gc_layer_t *l);

/* parametri dello shader */
int gc_sp_find(const gc_layer_t *l, const char *name);   /* indice o -1 */
void gc_sp_set(gc_layer_t *l, const char *name, float v);
void gc_sp_del(gc_layer_t *l, const char *name);

#endif
