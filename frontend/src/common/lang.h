/* Lingue: testi in Lang/<codice>.json, per chiave con punti ("settings.brightness").
   Se una chiave manca si usa l'inglese, poi la chiave stessa. */
#pragma once
#include <stdbool.h>

bool lang_load(const char *root, const char *code);
const char *T(const char *key);
/* come T, ma NULL se la chiave non c'è in nessuna lingua */
const char *T_opt(const char *key);
int lang_list(const char *root, char codes[][8], char names[][32], int max);
