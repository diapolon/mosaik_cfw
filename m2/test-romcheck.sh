#!/bin/sh
# Prova della diagnosi di un singolo gioco, con il modulo vero e zip veri:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-romcheck.sh
set -e
apk add -q build-base sdl2-dev python3 >/dev/null
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
    t.c /trimui/frontend/src/launcher/romcheck.c -o t $(sdl2-config --libs)

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

rm sd/Dats/fbneo.roms
expect "emulatore senza elenco" "$(r okgame zip)" "10||$F"
echo "TUTTO OK"
