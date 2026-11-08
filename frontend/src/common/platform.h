/* Hardware della console: TrimUI Smart Pro (TG5040) e Brick Pro (TG4040) sul firmware
   originale. Percorsi e comandi come in MinUI (workspace/tg5040). Sul Mac le funzioni non
   fanno nulla e la luminosità resta simulata scurendo l'immagine. */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "settings.h"

bool plat_is_device(void);
/* modello letto dallo script di avvio (TRIMUI_MODEL): "smartpro", "brickpro" o NULL */
const char *plat_model(void);
/* applica luminosità, volume e profilo CPU se sono cambiati (si chiama a ogni fotogramma) */
void plat_apply(const settings_t *s);
/* frequenza decisa dal launcher (kHz), al posto del profilo: i profili valgono per i giochi */
void plat_cpu_fixed(int khz);
/* retroilluminazione spenta finché dura (la luminosità scelta non cambia) */
void plat_screen_off(bool off);
/* si esce dal launcher: torna subito il profilo, che i giochi Linux non impostano da soli */
void plat_cpu_release(const settings_t *s);
/* frequenza attuale e temperatura, per il registro */
void plat_cpu_status(int *mhz, int *temp);
/* batteria in percentuale, -1 se non disponibile */
int plat_battery(bool *charging);
/* tasti della Brick Pro che SDL non vede come pulsanti: HOME (laterale), F1/F2 (le due
   barrette) e l'interruttore FN. Letti dal gamepad "TRIMUI Player1" di trimui_inputd. */
typedef struct {
    bool home, f1, f2;     /* premuti in questo fotogramma */
    int vol;               /* tasti volume: +1, -1 o 0 */
    bool power;            /* tasto di accensione: pressione breve (rilasciato entro 1,2 s) */
    bool power_long;       /* tasto di accensione tenuto premuto 1,2 s */
    int fn;                /* posizione dell'interruttore FN: -1 ignota, 0 o 1 */
    bool fn_changed;
} plat_keys_t;
void plat_poll(plat_keys_t *k);
/* sistema autonomo su SD (non app del firmware TrimUI): niente "Esci" */
bool plat_standalone(void);
/* nome di un dispositivo /dev/input/eventN (EVIOCGNAME); false se non leggibile */
bool plat_device_name(const char *path, char *out, int n);
/* imposta data e ora del sistema e le scrive nell'orologio della console (RTC), che le
   conserva da spenta; false se non riesce (sul Mac non fa nulla) */
bool plat_set_time(time_t t);
/* colore principale del tema, per i LED con "colore del tema" (0xRRGGBB) */
void plat_led_theme(unsigned rgb);
/* sospende la console (m2/arcade-suspend) e torna al risveglio; poi plat_apply riapplica
   luminosità, volume, CPU e LED. false se la sospensione non è riuscita */
bool plat_suspend(void);
