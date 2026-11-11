#!/bin/sh
# Scraper delle console (frontend/src/launcher/scraper.c, audit R08) contro un server locale:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-scraper.sh
# CRC noto e sconosciuto, zip con una e con più ROM, file del database sostituito da una pagina
# d'errore, rete che non risponde, errore di scrittura sulla scheda.
set -e
apk add -q build-base sdl2-dev sdl2_image-dev curl-dev zlib-dev python3 >/dev/null
W=/tmp/scraper; rm -rf $W; mkdir -p $W; cd $W
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
cp -r /trimui/frontend/src src

cat > src/launcher/t.c <<'C'
#include "scraper.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
void ui_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
emulator_t EMUS[16];
int NEMUS;
const emulator_t *emulator_find(const char *id) {
    for (int i = 0; i < NEMUS; i++) if (!strcmp(EMUS[i].id, id)) return &EMUS[i];
    return NULL;
}
static void emu(const char *id, const char *name, const char *thumbs, const char *ext) {
    emulator_t *e = &EMUS[NEMUS++];
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->thumbs, sizeof e->thumbs, "%s", thumbs);
    snprintf(e->index, sizeof e->index, "Dats/%s.tsv", id);
    snprintf(e->exts[0], sizeof e->exts[0], ".%s", ext);
    snprintf(e->exts[1], sizeof e->exts[1], ".zip");
    e->nexts = 2;
    e->console = true;
}
/* argomenti: radice, poi "console|nome|percorso" per ogni gioco */
int main(int argc, char **argv) {
    SDL_Init(0);
    emu("md", "Mega Drive", "Sega - Mega Drive - Genesis", "md");
    emu("gb", "Game Boy", "Nintendo - Game Boy", "gb");
    static game_t G[16];
    int n = 0;
    for (int i = 2; i < argc; i++, n++) {
        char core[16], name[128], path[512];
        sscanf(argv[i], "%15[^|]|%127[^|]|%511[^\n]", core, name, path);
        snprintf(G[n].core, sizeof G[n].core, "%s", core);
        snprintf(G[n].name, sizeof G[n].name, "%s", name);
        snprintf(G[n].key, sizeof G[n].key, "%s", name);
        snprintf(G[n].title, sizeof G[n].title, "%s", name);
        G[n].path = strdup(path);
    }
    if (!scraper_start(argv[1], G, n)) return 2;
    const scrape_status_t *s = scraper_status();
    while (s->phase != SCRAPE_DONE && s->phase != SCRAPE_FAILED) SDL_Delay(20);
    printf("%s %d %d\n", s->phase == SCRAPE_DONE ? "DONE" : "FAILED", s->saved, s->missing);
    scraper_join();
    return 0;
}
C
cd src/launcher
gcc -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o $W/t t.c scraper.c ../common/json.c ../runner/romzip.c \
    $(pkg-config --cflags --libs sdl2 SDL2_image libcurl zlib)
cd $W

# ROM, zip, file del database e immagini
python3 -I - <<'P'
import os, zlib, zipfile, struct
def png(path):
    w = h = 24
    raw = b''.join(b'\x00' + bytes([x * 10 % 256, y * 10 % 256, 128] * 1)[:3] * 1 for y in range(h) for x in range(w))
    raw = b''.join(b'\x00' + b''.join(bytes([x * 10 % 256, y * 10 % 256, 90]) for x in range(w)) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(data)
os.makedirs('sd/Roms/MD', exist_ok=True); os.makedirs('sd/Roms/GB', exist_ok=True)
a = bytes(range(256)) * 256                 # Gioco A, nel database
b = bytes(reversed(range(256))) * 256       # Gioco B, sconosciuto
gb = b'\x11' * 32768                        # Game Boy, nel database (dalla copia sulla scheda)
open('sd/Roms/MD/Gioco A.md', 'wb').write(a)
open('sd/Roms/MD/Gioco B.md', 'wb').write(b)
open('sd/Roms/MD/Gioco E.md', 'wb').write(b'\x22' * 4096)
with zipfile.ZipFile('sd/Roms/MD/Gioco C.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('leggimi.txt', 'non una ROM'); z.writestr('Gioco C.md', a)
with zipfile.ZipFile('sd/Roms/MD/Gioco D.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('Uno.md', a); z.writestr('Due.md', b)
open('sd/Roms/GB/Tetris.gb', 'wb').write(gb)
ref = open('/trimui/frontend/src/launcher/scraper.c').read().split('#define LR_REF "')[1].split('"')[0]
def dat(system, field, key, val, entries):
    d = f'www/libretro/libretro-database/{ref}/metadat/{field}'
    os.makedirs(d, exist_ok=True)
    body = f'clrmamepro (\n\tname "{system}"\n)\n\n'
    for name, crc in entries:
        body += f'game (\n\tcomment "{name}"\n\t{key} "{val}"\n\trom ( crc {crc:08X} )\n)\n\n'
    open(f'{d}/{system}.dat', 'w').write(body)
md = 'Sega - Mega Drive - Genesis'
for field, key, val in [('releaseyear', 'releaseyear', '1991'), ('genre', 'genre', 'Action'), ('maxusers', 'users', '2'),
                        ('publisher', 'publisher', 'Prova'), ('developer', 'developer', 'Prova')]:
    dat(md, field, key, val, [('Gioco A (Europe)', zlib.crc32(a))])
    # Game Boy: in rete una pagina d'errore con risposta 200 al posto del file
    d = f'www/libretro/libretro-database/{ref}/metadat/{field}'
    open(f'{d}/Nintendo - Game Boy.dat', 'w').write('<html><body>Accedi alla rete Wi-Fi dell\'albergo per continuare la navigazione ' + 'x' * 200 + '</body></html>')
    # e sulla scheda una copia vecchia ma buona
    c = f'sd/Arcade/Dats/libretro/{field}'
    os.makedirs(c, exist_ok=True)
    open(f'{c}/Nintendo - Game Boy.dat', 'w').write(f'clrmamepro (\n\tname "Nintendo - Game Boy"\n)\n\ngame (\n\tcomment "Tetris (World)"\n\t{key} "{val}"\n\trom ( crc {zlib.crc32(gb):08X} )\n)\n')
png(f'www/{md}/Named_Boxarts/Gioco A (Europe).png')
png(f'www/{md}/Named_Snaps/Gioco A (Europe).png')
P
# copie vecchie: si scaricano di nuovo (più di 30 giorni)
find sd/Arcade/Dats/libretro -name '*.dat' -exec touch -d '@1600000000' {} +
cp "sd/Arcade/Dats/libretro/genre/Nintendo - Game Boy.dat" gb-genre.dat
# errore di scrittura: al posto del file provvisorio c'è una cartella
mkdir -p "sd/Arcade/Media/box/md/Gioco C.png.tmp"

(cd www && exec python3 -m http.server 8765 --bind 127.0.0.1 >/dev/null 2>&1) & SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do wget -q -O /dev/null http://127.0.0.1:8765/ 2>/dev/null && break; sleep 0.3; done
R=$W/sd/Arcade
export ARCADE_SCRAPER_TEST=http://127.0.0.1:8765
out=$(./t $R "md|Gioco A|$W/sd/Roms/MD/Gioco A.md" "md|Gioco B|$W/sd/Roms/MD/Gioco B.md" "md|Gioco C|$W/sd/Roms/MD/Gioco C.zip" \
        "md|Gioco D|$W/sd/Roms/MD/Gioco D.zip" "gb|Tetris|$W/sd/Roms/GB/Tetris.gb" 2>$W/log1)
echo "--- primo giro: $out"
IX=$R/Dats/md.tsv
NF=$R/Media/non-trovate.txt
grep -q "^Gioco A	Gioco A (Europe)	1991	Prova	Mega Drive	" $IX && ok "CRC noto: titolo, anno ed editore nell'indice" || ko "Gioco A: $(cat $IX)"
grep -q "^md/Gioco A	Action	2$" $R/Dats/arcadedb.tsv && ok "CRC noto: genere e giocatori" || ko "meta: $(cat $R/Dats/arcadedb.tsv)"
[ -s "$R/Media/box/md/Gioco A.png" ] && [ -s "$R/Media/snap/md/Gioco A.png" ] && ok "scatola e schermata scaricate" || ko "immagini di Gioco A mancanti"
grep -q "^title/md/Gioco A$" $NF && ok "titolo assente sul server (404): segnato come non trovato" || ko "non-trovate: $(cat $NF)"
grep -q "^Gioco B	" $IX && ko "CRC sconosciuto finito nell'indice" || ok "CRC sconosciuto: nessuna riga nell'indice"
grep -q "^md/Gioco B	" $R/Dats/arcadedb.tsv && ko "CRC sconosciuto con genere" || ok "CRC sconosciuto: nessun genere inventato"
grep -q "^box/md/Gioco B$" $NF && ok "CRC sconosciuto: immagini cercate per nome e segnate come non trovate" || ko "Gioco B non segnato: $(cat $NF)"
grep -q "^Gioco C	Gioco A (Europe)	" $IX && ok "zip con una ROM: stessi dati della ROM non zippata" || ko "Gioco C: $(cat $IX)"
grep -q "^box/md/Gioco C$" $NF && ko "errore di scrittura segnato come immagine non trovata" || ok "errore di scrittura: non segnato, si riprova al prossimo giro"
[ -s "$R/Media/snap/md/Gioco C.png" ] && ok "… e le altre immagini dello stesso gioco si salvano" || ko "schermata di Gioco C mancante"
grep -q "^Gioco D	" $IX && ko "zip con più ROM: dati di una ROM a caso" || ok "zip con più ROM: nessun dato"
grep -q "1 zip con più ROM" $W/log1 && ok "zip con più ROM nel log" || ko "zip con più ROM non nel log"
cmp -s "$R/Dats/libretro/genre/Nintendo - Game Boy.dat" gb-genre.dat && ok "pagina d'errore al posto del database: resta la copia sulla scheda" || ko "copia del database sostituita"
grep -q "non è un file del database" $W/log1 && ok "… e il log lo dice" || ko "pagina d'errore non nel log"
grep -q "^Tetris	Tetris (World)	" $R/Dats/gb.tsv && ok "… e i dati vengono dalla copia sulla scheda" || ko "Tetris: $(cat $R/Dats/gb.tsv 2>&1)"
[ "${out%% *}" = DONE ] && ok "primo giro completato" || ko "primo giro: $out"

echo "--- secondo giro: rete che non risponde"
cp $NF nf-prima.txt
kill $SRV
export ARCADE_SCRAPER_TEST=http://127.0.0.1:8765
out=$(./t $R "md|Gioco E|$W/sd/Roms/MD/Gioco E.md" "md|Gioco B|$W/sd/Roms/MD/Gioco B.md" 2>$W/log2)
[ "${out%% *}" = FAILED ] && ok "rete assente: il giro si ferma con un errore" || ko "rete assente: $out"
grep -q "la rete non risponde più" $W/log2 && ok "… e il log lo dice" || ko "log: $(cat $W/log2)"
cmp -s nf-prima.txt $NF && ok "nessuna immagine segnata come non trovata per un errore di rete" || ko "non-trovate cambiato: $(diff nf-prima.txt $NF)"
grep -q "^Gioco A	Gioco A (Europe)	" $IX && ok "i dati già scaricati restano" || ko "indice cambiato"

[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors errori"; exit 1; }
