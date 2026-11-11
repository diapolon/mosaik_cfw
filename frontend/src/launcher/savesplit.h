#pragma once
#include "games.h"

/* Sposta i salvataggi delle console da Saves/<core>/ a Saves/<console>/ (issue #5): una copia
   per ogni console che ha un gioco con quel nome, poi il file vecchio si toglie. Restituisce
   quanti file vecchi sono rimasti per un errore (si riprova al prossimo avvio). */
int saves_split(const char *root, const game_t *games, int ngames);
