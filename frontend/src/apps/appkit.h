/* Parti comuni delle app dello Store (Arcade/Apps/<id>): argomenti, scritte proprie in
   lang/<lingua>.json accanto al programma (con l'inglese di riserva e poi i testi del sistema),
   finestre con fondo pieno e misure leggibili.

   Ogni app parte da launch.sh come un gioco Linux: --root la cartella Arcade (o $ROOT), --app la
   sua cartella; sul Mac: build/<id> --root sdcard --app package/apps/<id> [--auto "..."]. */
#pragma once
#include "../common/ui.h"
#include <stdbool.h>
#include <stddef.h>

extern char app_root[1024];   /* cartella Arcade */
extern char app_dir[1024];    /* cartella dell'app */

/* legge gli argomenti, apre finestra, tema, lingua e registro (Logs/<id>.log) */
bool app_start(int argc, char **argv, const char *id);
const char *FT(const char *key);                  /* scritta dell'app, poi del sistema, poi la chiave */
void app_panel(int w, int h, int *x, int *y);     /* finestra centrata, con ombra e fondo pieno */
void app_size(long long bytes, char *out, size_t n);
void app_exit_begin(void);                        /* da qui l'uscita ha al massimo 5 s */
void app_quit(void);                              /* chiusura di SDL (dentro gli stessi 5 s) */
