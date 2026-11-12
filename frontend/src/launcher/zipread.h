#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { char name[512]; uint32_t crc, csize, usize, off; unsigned method; } zent_t;

/* voci della directory centrale in *out (da liberare); -1 se non è uno zip leggibile */
int zip_dir(FILE *f, zent_t **out);
/* dati di una voce (salvata o deflate): in un file (to), oppure in memoria (to NULL, al massimo
   n-1 byte in mem, più lo zero finale); vero solo se completa e con il CRC giusto */
bool zip_entry(FILE *f, const zent_t *e, FILE *to, char *mem, size_t n);
