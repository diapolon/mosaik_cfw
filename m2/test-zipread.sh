#!/bin/sh
# Prova del lettore zip condiviso (frontend/src/launcher/zipread.c: Store, backup, fumetti .cbz)
# con archivi malformati (audit 12 novembre, BUG-001). Il buffer di destinazione finisce contro
# una pagina protetta: una scrittura oltre il limite fa cadere il programma invece di passare.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-zipread.sh
# ZIPREAD=percorso prova un'altra versione del lettore (per esempio quella di prima della correzione).
set -e
apk add -q build-base zlib-dev python3 >/dev/null
W=/tmp/zr; rm -rf $W; mkdir -p $W; cd $W
ok=0; ko=0
check() { if [ "$2" = "$3" ]; then ok=$((ok+1)); echo "  ok: $1"; else ko=$((ko+1)); echo "  ERRORE: $1: atteso [$2], ottenuto [$3]"; fi; }
L=/trimui/frontend/src/launcher
cp "${ZIPREAD:-$L/zipread.c}" zipread.c; cp $L/zipread.h .
cat > t.c <<'C'
#include "zipread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
/* n byte che finiscono esattamente prima di una pagina inaccessibile */
static char *guarded(size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t pages = (n + (size_t)pg - 1) / (size_t)pg + 1;
    char *m = mmap(NULL, pages * (size_t)pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(m + (pages - 1) * (size_t)pg, (size_t)pg, PROT_NONE);
    return m + (pages - 1) * (size_t)pg - n;
}
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb");
    zent_t *z;
    if (!f || zip_dir(f, &z) < 1) { puts("dir"); return 0; }
    size_t cap = (size_t)atoi(v[2]);
    char *buf = guarded(cap);
    bool m = zip_entry(f, &z[0], NULL, buf, cap);
    FILE *o = tmpfile();
    bool fl = zip_entry(f, &z[0], o, NULL, 0);
    printf("%s %s", m ? "si" : "no", fl ? "si" : "no");
    if (m) printf(" %s", buf);
    puts("");
    return 0;
}
C
gcc -O1 -g -Wall -o t t.c zipread.c -lz
python3 - <<'P'
import zipfile, struct, zlib
def write(name, data, method, patch=None, trunc=0):
    z = zipfile.ZipFile(name, "w", method); z.writestr("dentro.txt", data); z.close()
    b = bytearray(open(name, "rb").read())
    cd = b.rfind(b"PK\x01\x02")
    if patch: patch(b, cd)
    if trunc: b = b[:30 + len("dentro.txt") + trunc] + b[cd:]   # dati tagliati, directory spostata
    open(name, "wb").write(bytes(b))
write("salvata.zip", b"ciao", zipfile.ZIP_STORED)
write("compressa.zip", b"ciao " * 200, zipfile.ZIP_DEFLATED)
# salvata con 5000 byte ma la directory centrale dichiara usize=1: il vecchio lettore copiava 5000 byte
write("discorde.zip", b"x" * 5000, zipfile.ZIP_STORED, lambda b, cd: b.__setitem__(slice(cd + 24, cd + 28), struct.pack("<I", 1)))
write("crc.zip", b"ciao", zipfile.ZIP_STORED, lambda b, cd: b.__setitem__(slice(cd + 16, cd + 20), struct.pack("<I", 12345)))
# compressa con usize più piccolo del vero
# nome di 600 caratteri: non sta nella voce, lo zip si rifiuta invece di tagliarlo
z = zipfile.ZipFile("lungo.zip", "w"); z.writestr("d/" * 299 + "x.txt", b"ciao"); z.close()
write("gonfia.zip", b"a" * 100000, zipfile.ZIP_DEFLATED, lambda b, cd: b.__setitem__(slice(cd + 24, cd + 28), struct.pack("<I", 10)))
P
check "voce salvata valida" "si si ciao" "$(./t salvata.zip 16 2>&1)"
check "voce compressa valida" "si si" "$(./t compressa.zip 2000 2>&1 | cut -d' ' -f1,2)"
check "voce salvata più grande dello spazio: rifiutata" "no si" "$(./t salvata.zip 4 2>&1)"
check "misure discordanti (usize 1, dati 5000): rifiutata senza scrivere oltre" "no no" "$(./t discorde.zip 16 2>&1 || echo caduto)"
check "CRC sbagliato: rifiutata" "no no" "$(./t crc.zip 16 2>&1)"
check "compressa più grande del dichiarato: rifiutata" "no no" "$(./t gonfia.zip 64 2>&1)"
check "nome più lungo di 511 caratteri: zip rifiutato, niente nome tagliato" "dir" "$(./t lungo.zip 16 2>&1)"
echo
if [ $ko -eq 0 ]; then echo "TUTTO OK ($ok prove)"; else echo "$ko PROVE FALLITE"; exit 1; fi
