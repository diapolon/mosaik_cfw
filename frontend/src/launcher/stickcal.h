/* Calibrazione delle levette: scrive i file che trimui_inputd legge da /mnt/UDISK.
   In Mosaik quella cartella è in memoria: la copia che resta è in Arcade/System, e all'avvio
   system-init la rimette a posto. La memoria interna della console non viene mai toccata. */
#pragma once
#include <stdbool.h>

enum { SC_LX, SC_LY, SC_RX, SC_RY, SC_AXES };

typedef struct { int min[SC_AXES], max[SC_AXES], zero[SC_AXES]; } stickcal_t;

/* durante la misura: valore grezzo (0..4095) da quello che arriva a SDL; -1 se fuori scala */
int stickcal_raw(int axis_value);
/* quanto deve allontanarsi una levetta dal centro perché la misura valga: dipende dal modello */
void stickcal_set_min_span(int span);
bool stickcal_axis_ok(const stickcal_t *c, int axis);
bool stickcal_ok(const stickcal_t *c);

/* mette i valori larghi per misurare, conservando quelli di prima */
bool stickcal_begin(const char *udisk, bool brick);
/* rimette i valori di prima */
void stickcal_cancel(const char *udisk, bool brick);
/* salva sulla SD e nella cartella in memoria; falso se non è riuscito (restano quelli di prima) */
bool stickcal_save(const char *sysdir, const char *udisk, const stickcal_t *c, bool brick);
bool stickcal_has_custom(const char *sysdir);
void stickcal_reset(const char *sysdir, const char *udisk, bool brick);
