/* ROM zippate per i core che non leggono gli zip (vedi romzip.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { ROMZIP_OK, ROMZIP_OPEN, ROMZIP_FORMAT, ROMZIP_NOT_FOUND, ROMZIP_DAMAGED, ROMZIP_MEMORY };

bool romzip_is_zip(const char *path);
/* "nes|fds|unf" (come valid_extensions di libretro): l'estensione di name è nell'elenco? */
bool romzip_ext_ok(const char *name, const char *exts);
/* Prima voce dello zip con un'estensione di exts, decompressa e con il CRC giusto: *data (da
   liberare con free), *size e il nome della voce senza cartelle. ROMZIP_OK o un errore. */
int romzip_extract(const char *zip, const char *exts, void **data, size_t *size, char *name, size_t namen);
