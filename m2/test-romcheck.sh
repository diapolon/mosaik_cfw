#!/bin/sh
# Prova della diagnosi di un singolo gioco, con il modulo vero e zip veri:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-romcheck.sh
set -e
apk add -q build-base sdl2-dev zlib-dev python3 >/dev/null
W=/tmp/rc; rm -rf $W; mkdir -p $W/sd/Dats $W/sd/Roms/FBNEO $W/sd/Bios $W/sd/Cores; cd $W
touch sd/Cores/fbneo_libretro.so

cat > make.py <<'P'
import zipfile, zlib
def z(name, files):
    with zipfile.ZipFile(f'sd/Roms/FBNEO/{name}.zip', 'w') as f:
        for k, v in files.items(): f.writestr(k, v)
def item(name, data, flag=''): return f'{name}|{len(data)}|{zlib.crc32(data):08x}|{flag}'
A, B, P, N = b'AAAA', b'BBBBBB', b'PPP', b'NNNNN'
z('okgame', {'a.bin': A})
z('nobios', {'a.bin': A})
z('biosinzip', {'a.bin': A, 'n.bin': N})
z('oldstyle', {'a.bin': A})
z('clone', {'a.bin': A})
z('partial', {'a.bin': A})
z('oldver', {'a.bin': b'ZZZZ'})
z('unknown', {'a.bin': A})
open('sd/Roms/FBNEO/badzip.zip', 'wb').write(b'questo non e uno zip')
open('sd/Roms/FBNEO/seven.7z', 'wb').write(b'7z')
with open('sd/Dats/fbneo.roms', 'w') as f:
    for name, romof, files in [
        ('okgame', '', [item('a.bin', A)]), ('badzip', '', [item('a.bin', A)]),
        ('nobios', 'neogeo', [item('a.bin', A), item('n.bin', N, 'm')]),
        ('biosinzip', 'neogeo', [item('a.bin', A), item('n.bin', N, 'm')]),
        ('oldstyle', 'neogeo', [item('a.bin', A)]),
        ('clone', 'parentg', [item('a.bin', A), item('p.bin', P, 'm')]),
        ('partial', '', [item('a.bin', A), item('b.bin', B)]),
        ('oldver', '', [item('a.bin', A)]),
        ('neogeo', '', [item('n.bin', N)]), ('parentg', '', [item('p.bin', P)]),
    ]: f.write(f'{name}\t{romof}\t{",".join(files)}\n')
with open('sd/Dats/fbneo.tsv', 'w') as f:
    for name, bios in [('okgame', 0), ('nobios', 0), ('clone', 0), ('neogeo', 1), ('parentg', 0)]:
        f.write(f'{name}\tTitolo\t1990\tDitta\tSistema\t0\t{bios}\t\n')
P
python3 make.py

cat > t.c <<'C'
#include "romcheck.h"
#include <stdio.h>
#include <string.h>
emulator_t EMUS[16] = { { .id = "fbneo", .name = "FinalBurn Neo", .core = "fbneo_libretro", .roms = "Roms/FBNEO", .index = "Dats/fbneo.tsv" } };
int NEMUS = 1;
const emulator_t *emulator_find(const char *id) { return strcmp(id, "fbneo") ? NULL : &EMUS[0]; }
int main(int argc, char **argv) {
    /* nome, percorso, core, 1 se il core non si è caricato */
    game_t g = { 0 };
    rdiag_t d;
    snprintf(g.name, sizeof g.name, "%s", argv[1]);
    snprintf(g.core, sizeof g.core, "fbneo");
    g.path = argv[2];
    romcheck_diagnose("sd", &g, argv[3], argc > 4, &d);
    printf("%d|%s|%s\n", d.kind, d.file, d.where);
    return 0;
}
C
gcc -std=c11 -D_DEFAULT_SOURCE -g -fsanitize=undefined -fsanitize-undefined-trap-on-error -Wall -Wextra -I/trimui/frontend/src/launcher $(sdl2-config --cflags) \
    t.c /trimui/frontend/src/launcher/romcheck.c -o t $(sdl2-config --libs) -lz

C=sd/Cores/fbneo_libretro.so
F=Arcade/Roms/FBNEO
N="FinalBurn Neo"
expect() { [ "$2" = "$3" ] && echo "  ok: $1" || { echo "  ERRORE: $1: atteso [$3], ottenuto [$2]"; exit 1; }; }
r() { ./t "$1" "sd/Roms/FBNEO/$1.$2" "$C"; }

expect "gioco completo"        "$(r okgame zip)"  "0||$F"
expect "core assente"          "$(./t okgame sd/Roms/FBNEO/okgame.zip sd/Cores/manca_libretro.so)" "1|manca_libretro.so|$F"
expect "core che non si carica" "$(./t okgame sd/Roms/FBNEO/okgame.zip $C 1)" "2|fbneo_libretro.so|$F"
expect "file del gioco sparito" "$(r sparito zip)" "3|sparito.zip|$F"
expect "zip illeggibile"       "$(r badzip zip)"  "4|badzip.zip|$F"
expect "BIOS mancante"         "$(r nobios zip)"  "5|neogeo.zip|$F"
expect "BIOS dentro lo zip del gioco" "$(r biosinzip zip)" "0||$F"
expect "elenco senza i file del BIOS" "$(r oldstyle zip)" "5|neogeo.zip|$F"
expect "gioco principale mancante" "$(r clone zip)" "6|parentg.zip|$F"
expect "file mancanti"         "$(r partial zip)" "7|b.bin|$N"
expect "altra versione"        "$(r oldver zip)"  "8|a.bin|$N"
expect "nome fuori catalogo"   "$(r unknown zip)" "9|unknown.zip|$N"
expect "formato 7z"            "$(r seven 7z)"    "10||$F"

# il BIOS copiato in Arcade/Bios risolve il problema: resta da controllare il gioco e basta
python3 -c "
import zipfile
with zipfile.ZipFile('sd/Bios/neogeo.zip','w') as f: f.writestr('n.bin', b'NNNNN')"
expect "BIOS trovato in Bios"  "$(r nobios zip)"  "0||$F"

# verifica completa: zip salvati e compressi, sani e con i dati rovinati (l'indice resta giusto)
cat > rovina.py <<'P'
import zipfile, sys
def z(path, method, data):
    with zipfile.ZipFile(path, 'w', method) as f: f.writestr('dir/a.bin', data); f.writestr('b.bin', b'B' * 5000)
data = bytes(range(256)) * 400
for m, tag in ((zipfile.ZIP_STORED, 'st'), (zipfile.ZIP_DEFLATED, 'df')):
    z(f'sano-{tag}.zip', m, data)
    z(f'rotto-{tag}.zip', m, data)
    b = bytearray(open(f'rotto-{tag}.zip', 'rb').read())
    b[200] ^= 0x55                      # un byte dentro i dati del primo file
    open(f'rotto-{tag}.zip', 'wb').write(b)
P
python3 rovina.py
cat > v.c <<'C'
#include "romcheck.h"
#include <stdio.h>
#include <string.h>
emulator_t EMUS[16];
int NEMUS = 0;
const emulator_t *emulator_find(const char *id) { (void)id; return NULL; }
int main(int argc, char **argv) {
    (void)argc;
    char bad[200];
    int v = romcheck_zip_verify(argv[1], bad, sizeof bad);
    printf("%d|%s\n", v, bad);
    return 0;
}
C
gcc -std=c11 -D_DEFAULT_SOURCE -g -fsanitize=undefined -fsanitize-undefined-trap-on-error -Wall -Wextra -I/trimui/frontend/src/launcher $(sdl2-config --cflags) \
    v.c /trimui/frontend/src/launcher/romcheck.c -o v $(sdl2-config --libs) -lz
expect "zip salvato sano"      "$(./v sano-st.zip)"  "1|"
expect "zip compresso sano"    "$(./v sano-df.zip)"  "1|"
expect "zip salvato rovinato"  "$(./v rotto-st.zip)" "0|a.bin"
expect "zip compresso rovinato" "$(./v rotto-df.zip)" "0|a.bin"
expect "non uno zip"           "$(./v sd/Roms/FBNEO/badzip.zip)" "-1|"
expect "zip del gioco intero"  "$(./v sd/Roms/FBNEO/okgame.zip)" "1|"
# il gioco con l'indice giusto ma i dati rovinati: la diagnosi lo dice
python3 -c "
b = bytearray(open('sd/Roms/FBNEO/okgame.zip','rb').read()); b[b.find(b'AAAA')] ^= 1; open('sd/Roms/FBNEO/okgame.zip','wb').write(b)"
expect "diagnosi: dati rovinati" "$(r okgame zip)" "4|okgame.zip|$F"

# verifica di tutte le ROM, completa: un arcade rovinato, uno sano, una console senza elenco
mkdir -p sd/Roms/NES sd/System
cp sano-df.zip sd/Roms/NES/buono.zip; cp rotto-df.zip sd/Roms/NES/cattivo.zip
cat > b.c <<'C'
#include "romcheck.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
emulator_t EMUS[16] = { { .id = "fbneo", .name = "FinalBurn Neo", .core = "fbneo_libretro", .roms = "Roms/FBNEO", .index = "Dats/fbneo.tsv" },
                        { .id = "nes", .name = "NES", .core = "fceumm_libretro", .roms = "Roms/NES", .index = "Dats/nes.tsv" } };
int NEMUS = 2;
const emulator_t *emulator_find(const char *id) { return !strcmp(id, "nes") ? &EMUS[1] : !strcmp(id, "fbneo") ? &EMUS[0] : NULL; }
int main(int argc, char **argv) {
    game_t g[4];
    memset(g, 0, sizeof g);
    (void)argv;
    const char *nm[4] = { "okgame", "biosinzip", "buono", "cattivo" }, *co[4] = { "fbneo", "fbneo", "nes", "nes" };
    char path[4][200];
    for (int i = 0; i < 4; i++) {
        snprintf(g[i].name, sizeof g[i].name, "%s", nm[i]);
        snprintf(g[i].core, sizeof g[i].core, "%s", co[i]);
        snprintf(path[i], sizeof path[i], "sd/Roms/%s/%s.zip", i < 2 ? "FBNEO" : "NES", nm[i]);
        g[i].path = path[i];
    }
    romcheck_start("sd", g, 4, argc > 1);
    while (!romcheck_status()->finished) usleep(10000);
    romcheck_join();
    return 0;
}
C
gcc -std=c11 -D_DEFAULT_SOURCE -g -fsanitize=undefined -fsanitize-undefined-trap-on-error -Wall -Wextra -I/trimui/frontend/src/launcher $(sdl2-config --cflags) \
    b.c /trimui/frontend/src/launcher/romcheck.c -o b $(sdl2-config --libs) -lz
./b
expect "rapida: arcade rovinato non visto" "$(grep okgame sd/System/romcheck.tsv | cut -f3)" "1"
expect "rapida: console non controllabile" "$(grep buono sd/System/romcheck.tsv | cut -f3)" "6"
./b completa
expect "completa: arcade rovinato"   "$(grep okgame sd/System/romcheck.tsv | cut -f3-4)" "$(printf '7\ta.bin')"
expect "completa: arcade sano"       "$(grep biosinzip sd/System/romcheck.tsv | cut -f3)" "1"
expect "completa: console sana"      "$(grep buono sd/System/romcheck.tsv | cut -f3)" "1"
expect "completa: console rovinata"  "$(grep cattivo sd/System/romcheck.tsv | cut -f3-4)" "$(printf '7\ta.bin')"

rm sd/Dats/fbneo.roms
expect "emulatore senza elenco" "$(./t biosinzip sd/Roms/FBNEO/biosinzip.zip $C)" "10||$F"
echo "TUTTO OK"
