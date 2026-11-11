#pragma once
#include "games.h"

/* Copia i salvataggi delle console da Saves/<core>/ a Saves/<console>/ (issue #5): una copia per
   ogni console che ha un gioco con quel nome; gli originali restano, le copie fatte e i
   conflitti si registrano in Saves/<core>/.spostati-per-console.txt. Restituisce quante copie
   non sono riuscite (si riprova al prossimo avvio). */
int saves_split(const char *root, const game_t *games, int ngames);
