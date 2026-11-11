#!/bin/sh
# Prova delle ROM zippate per i core che non leggono gli zip (frontend/src/runner/romzip.c):
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-romzip.sh
set -e
apk add -q build-base zlib-dev python3 >/dev/null
W=/tmp/romzip; rm -rf $W; mkdir -p $W; cd $W
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }

# zip di prova (python: voci salvate e compresse, cartelle, CRC rovinato)
python3 -I - <<'P'
import zipfile, struct
rom = bytes(range(256)) * 512                      # 128 KB
with zipfile.ZipFile('deflate.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('leggimi.txt', 'non una ROM')
    z.writestr('Gioco (Europe).md', rom)
with zipfile.ZipFile('stored.zip', 'w', zipfile.ZIP_STORED) as z:
    z.writestr('Gioco.nes', rom[:40960])
with zipfile.ZipFile('cartella.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('roms/', '')
    z.writestr('roms/Gioco.GEN', rom)
with zipfile.ZipFile('due.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('Primo.md', b'A' * 1000)
    z.writestr('Secondo.md', b'B' * 1000)
with zipfile.ZipFile('nessuna.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('copertina.png', b'x' * 100)
open('rovinato.zip', 'wb').write(open('stored.zip', 'rb').read())
d = bytearray(open('rovinato.zip', 'rb').read())
i = d.find(rom[:64])                               # un byte dei dati, non delle intestazioni
d[i + 100] ^= 0xFF
open('rovinato.zip', 'wb').write(d)
open('nonzip.zip', 'wb').write(b'questo non e uno zip' * 10)
open('rom.bin', 'wb').write(rom)
P

cat > t.c <<'C'
#include "romzip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    void *d; size_t n; char name[256] = "";
    if (!strcmp(argv[1], "-n")) { printf("%d\n", romzip_candidates(argv[2], argv[3])); return 0; }
    int rc = romzip_extract(argv[1], argv[2], &d, &n, name, sizeof name);
    printf("%d %zu %s\n", rc, n, name);
    if (rc == ROMZIP_OK && argc > 3) { FILE *f = fopen(argv[3], "wb"); fwrite(d, 1, n, f); fclose(f); }
    if (rc == ROMZIP_OK) free(d);
    return 0;
}
C
gcc -O2 -Wall -Wextra -I/trimui/frontend/src/runner -o t t.c /trimui/frontend/src/runner/romzip.c -lz

r() { ./t "$@"; }
[ "$(r deflate.zip 'md|gen|bin' out.md)" = "0 131072 Gioco (Europe).md" ] && cmp -s out.md rom.bin && ok "voce compressa: la ROM giusta, byte per byte" || ko "deflate: $(r deflate.zip 'md|gen|bin')"
[ "$(r stored.zip 'fds|nes|unf' out.nes)" = "0 40960 Gioco.nes" ] && ok "voce salvata senza compressione" || ko "stored: $(r stored.zip 'fds|nes|unf')"
[ "$(r cartella.zip 'md|gen')" = "0 131072 Gioco.GEN" ] && ok "dentro una cartella, estensione in maiuscolo: nome senza cartella" || ko "cartella: $(r cartella.zip 'md|gen')"
[ "$(r due.zip 'md')" = "0 1000 Primo.md" ] && ok "più ROM: la prima" || ko "due: $(r due.zip md)"
[ "$(r nessuna.zip 'md|gen')" = "3 0 " ] && ok "nessun file adatto all'emulatore" || ko "nessuna: $(r nessuna.zip 'md|gen')"
[ "$(r rovinato.zip 'nes')" = "4 0 " ] && ok "dati rovinati: il CRC lo scopre" || ko "rovinato: $(r rovinato.zip nes)"
[ "$(r nonzip.zip 'nes')" = "2 0 " ] && ok "file che non è uno zip" || ko "nonzip: $(r nonzip.zip nes)"
[ "$(r manca.zip 'nes')" = "1 0 " ] && ok "file che non c'è" || ko "manca: $(r manca.zip nes)"

echo "--- quante ROM nello zip (romzip_candidates, R08)"
[ "$(r -n due.zip md)" = 2 ] && ok "due ROM adatte: 2 (il runner lo scrive nel log, lo scraper non cerca dati)" || ko "due: $(r -n due.zip md)"
[ "$(r -n deflate.zip 'md|gen|bin')" = 1 ] && ok "una ROM e un file di testo: 1" || ko "deflate: $(r -n deflate.zip 'md|gen|bin')"
[ "$(r -n cartella.zip 'md|gen')" = 1 ] && ok "la cartella non conta" || ko "cartella: $(r -n cartella.zip 'md|gen')"
[ "$(r -n nessuna.zip 'md|gen')" = 0 ] && ok "nessuna adatta: 0" || ko "nessuna: $(r -n nessuna.zip 'md|gen')"
[ "$(r -n nonzip.zip nes)" = -1 ] && [ "$(r -n manca.zip nes)" = -1 ] && ok "non zip o assente: -1" || ko "nonzip/manca: $(r -n nonzip.zip nes) $(r -n manca.zip nes)"

[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors errori"; exit 1; }
