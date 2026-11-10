/* Sostituzione di un file che sopravvive a uno spegnimento brusco.
 *
 * Il file provvisorio, già passato da fsync, prende il posto di quello vero; poi si scrive
 * sulla scheda anche il cambio della cartella. Senza quest'ultimo passo, sul FAT di Linux il
 * rename resta in memoria fino a 30 secondi, mentre una fsync di un altro file (il log, per
 * esempio) può scrivere prima la parte della tabella che libera i blocchi del file vecchio.
 * Uno spegnimento in quel momento lascia la voce del file vecchio su blocchi liberi e il
 * controllo all'avvio la svuota: si perdono la versione nuova e la vecchia (visto su console
 * con settings.json e wifi.conf, Logs/fsck.txt).
 *
 * sync() e non syncfs(): sulla console l'unico filesystem scrivibile su disco è la SD, e
 * sync() c'è anche sul Mac e nelle prove. */
#ifndef DURABLE_H
#define DURABLE_H

#include <stdio.h>
#include <unistd.h>

/* 0 se il file vero è stato sostituito, come rename() */
static inline int durable_replace(const char *tmp, const char *path) {
    if (rename(tmp, path) != 0) return -1;
    sync();
    return 0;
}

#endif
