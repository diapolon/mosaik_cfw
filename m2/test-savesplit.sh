#!/bin/sh
# Salvataggi delle console separati per console (frontend/src/launcher/savesplit.c, issue #5):
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-savesplit.sh
set -e
apk add -q build-base >/dev/null
W=/tmp/split; rm -rf $W; mkdir -p $W/src; cd $W
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
F=/trimui/frontend/src
cp $F/launcher/savesplit.c $F/launcher/savesplit.h $F/launcher/games.h src/
mkdir -p common && cp $F/common/durable.h common/
sed -i 's|"../common/durable.h"|"../common/durable.h"|' src/savesplit.c

cat > src/t.c <<'C'
#include "savesplit.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
void ui_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
emulator_t EMUS[MAX_EMUS];
int NEMUS;
static void emu(const char *id, const char *core, bool console) {
    emulator_t *e = &EMUS[NEMUS++];
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->core, sizeof e->core, "%s", core);
    e->console = console;
}
static game_t G[16];
static int ng;
static void game(const char *core, const char *name) {
    memset(&G[ng], 0, sizeof G[ng]);
    snprintf(G[ng].core, sizeof G[ng].core, "%s", core);
    snprintf(G[ng].name, sizeof G[ng].name, "%s", name);
    ng++;
}
int main(int c, char **v) {
    (void)c;
    emu("fbneo", "fbneo_libretro", false);
    emu("md", "genesis_plus_gx_libretro", true);
    emu("sms", "genesis_plus_gx_libretro", true);
    emu("gg", "genesis_plus_gx_libretro", true);
    emu("gb", "gambatte_libretro", true);
    emu("gbc", "gambatte_libretro", true);
    game("fbneo", "sf2");
    game("md", "Sonic");
    game("sms", "Sonic");
    game("md", "Gunstar");
    game("gb", "Tetris");
    game("gbc", "Zelda");
    game("md", "A");
    game("gg", "Mio");
    game("md", "Uguale");
    printf("%d\n", saves_split(v[1], G, ng));
    return 0;
}
C
gcc -O2 -Wall -Wextra -Isrc -o t src/t.c src/savesplit.c

S=$W/sd; mkdir -p $S/Saves/genesis_plus_gx $S/Saves/gambatte $S/Saves/fbneo $S/Saves/gg
put() { printf '%s' "$2" > "$1"; }
put $S/Saves/genesis_plus_gx/Sonic.srm sonic-srm
put $S/Saves/genesis_plus_gx/Sonic.auto.state sonic-auto
put $S/Saves/genesis_plus_gx/Gunstar.srm gunstar
put $S/Saves/genesis_plus_gx/Gunstar.2.state.png anteprima
put $S/Saves/genesis_plus_gx/A.B.srm altro-gioco
put $S/Saves/genesis_plus_gx/Tolto.srm gioco-tolto
put $S/Saves/genesis_plus_gx/Mio.srm vecchio
put $S/Saves/gg/Mio.srm nuovo
put $S/Saves/gambatte/Tetris.srm tetris
put $S/Saves/gambatte/Tetris.rtc orologio
put $S/Saves/gambatte/Zelda.srm zelda
put $S/Saves/fbneo/sf2.state arcade

put $S/Saves/genesis_plus_gx/Uguale.srm stesso
mkdir -p $S/Saves/md; put $S/Saves/md/Uguale.srm stesso
rc=$(./t $S 2>$W/log)
same() { [ "$(cat "$2" 2>/dev/null || echo '(manca)')" = "$3" ] && ok "$1" || ko "$1: $(cat "$2" 2>/dev/null || echo '(manca)')"; }
J=$S/Saves/genesis_plus_gx/.spostati-per-console.txt
[ "$rc" = 0 ] && ok "nessuna copia fallita" || ko "copie fallite: $rc"
same "Mega Drive: .srm nella sua cartella" $S/Saves/md/Gunstar.srm gunstar
same "Mega Drive: anche l'anteprima dello slot" $S/Saves/md/Gunstar.2.state.png anteprima
same "stesso nome su Mega Drive e Master System: copia al Mega Drive" $S/Saves/md/Sonic.srm sonic-srm
same "… e al Master System" $S/Saves/sms/Sonic.srm sonic-srm
same "… anche il salvataggio automatico" $S/Saves/sms/Sonic.auto.state sonic-auto
same "l'originale resta (si può tornare alla versione di prima)" $S/Saves/genesis_plus_gx/Sonic.srm sonic-srm
same "Game Boy" $S/Saves/gb/Tetris.srm tetris
same "Game Boy: anche l'orologio" $S/Saves/gb/Tetris.rtc orologio
same "Game Boy Color" $S/Saves/gbc/Zelda.srm zelda
same "Zelda non va al Game Boy" $S/Saves/gb/Zelda.srm "(manca)"
same "il gioco A non prende i file di A.B" $S/Saves/md/A.B.srm "(manca)"
same "gioco non più sulla scheda: il file resta solo dov'era" $S/Saves/genesis_plus_gx/Tolto.srm gioco-tolto
same "conflitto: il file della console non si sovrascrive" $S/Saves/gg/Mio.srm nuovo
same "… e l'originale resta" $S/Saves/genesis_plus_gx/Mio.srm vecchio
grep -q "^gg	Mio.srm	conflitto" $J && ok "conflitto registrato" || ko "conflitto non registrato: $(cat $J)"
grep -q "conflitto per Mio.srm" $W/log && ok "conflitto nel log" || ko "conflitto non nel log"
grep -q "^md	Uguale.srm	uguale" $J && ! grep -q "conflitto per Uguale" $W/log && ok "file uguale: nessun conflitto" || ko "file uguale trattato male"
grep -q "^md	Sonic.srm$" $J && grep -q "^sms	Sonic.srm$" $J && ok "copie registrate per console" || ko "registro: $(cat $J)"
same "arcade: nulla cambia" $S/Saves/fbneo/sf2.state arcade
[ -z "$(find $S -name '*.tmp')" ] && ok "nessun file provvisorio" || ko "provvisori rimasti"
before=$(cd $S && find . -type f | sort | xargs md5sum | md5sum)
rc=$(./t $S 2>$W/log2)
[ "$rc" = 0 ] && [ "$(cd $S && find . -type f | sort | xargs md5sum | md5sum)" = "$before" ] && ok "secondo avvio: niente cambia" || ko "secondo avvio: $rc, $(cat $W/log2)"
rm $S/Saves/md/Sonic.srm   # "Reimposta il gioco" sulla console nuova
./t $S >/dev/null 2>&1
same "dopo Reimposta il gioco il salvataggio non torna" $S/Saves/md/Sonic.srm "(manca)"
same "… e il Master System tiene il suo" $S/Saves/sms/Sonic.srm sonic-srm

echo "--- identità dei giochi omonimi nello stesso emulatore (R04)"
awk '/^static int path_depth/,/^static bool has_ext/' $F/launcher/games.c | sed '$d' > src/keys.inc
cat > src/k.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "games.h"
#include "durable.h"
static void ui_log(const char *f, ...) { (void)f; }
#include "keys.inc"
static game_t G[20000];
static int n;
static void add(const char *name, const char *path, const char *title) {
    memset(&G[n], 0, sizeof G[n]);
    snprintf(G[n].name, sizeof G[n].name, "%s", name);
    snprintf(G[n].title, sizeof G[n].title, "%s", title);
    G[n].path = (char *)path;
    n++;
}
int main(void) {
    add("Sonic", "/sd/Roms/MD/EU/Sonic.md", "Sonic");
    add("Sonic", "/sd/Roms/MD/Sonic.md", "Sonic");
    add("Tetris", "/sd/Roms/MD/Tetris.md", "Tetris");
    add("Sonic", "/sd/Roms/MD/USA/Hack/Sonic.md", "Sonic");
    add("Golf", "/sd/Roms/MD/Golf.md", "Golf");
    add("Golf", "/sd/Roms/MD/Golf.zip", "Golf");
    /* audit BUG-003: chiavi che coincidevano */
    add("Game", "/sd/Roms/MD/Game.md", "Game");
    add("Game", "/sd/Roms/MD/Region/Game.zip", "Game");
    add("Game", "/sd/Roms/MD/Region/Game.md", "Game");
    add("X", "/sd/Roms/MD/A/B/X.md", "X");
    add("X", "/sd/Roms/MD/A-B/X.md", "X");
    add("X", "/sd/Roms/MD/X.md", "X");
    add("Pac [EU]", "/sd/Roms/MD/Pac [EU].md", "Pac [EU]");
    add("Pac", "/sd/Roms/MD/EU/Pac.md", "Pac");
    add("Pac", "/sd/Roms/MD/Pac.md", "Pac");
    assign_keys(G, n, "/sd/Roms/MD", NULL);
    for (int i = 0; i < n; i++) printf("%s|%s\n", G[i].key, G[i].title);
    /* BUG-009: 20000 ROM, molte omonime in cartelle diverse; chiavi tutte diverse */
    static char paths[20000][64];
    n = 0;
    for (int i = 0; i < 20000; i++) {
        snprintf(paths[i], sizeof paths[i], "/sd/Roms/MD/%s%d/G%d.md", i % 3 ? "D" : "D-", i % 7, i % 9000);
        char nm[16]; snprintf(nm, sizeof nm, "G%d", i % 9000);
        add(nm, paths[i], nm);
    }
    clock_t t0 = clock();
    assign_keys(G, n, "/sd/Roms/MD", NULL);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    int same = 0;
    char (*k)[sizeof G[0].key] = malloc(sizeof G[0].key * 20000);
    for (int i = 0; i < n; i++) memcpy(k[i], G[i].key, sizeof G[0].key);
    qsort(k, 20000, sizeof G[0].key, (int (*)(const void *, const void *))strcmp);
    for (int i = 1; i < n; i++) same += !strcmp(k[i], k[i - 1]);
    fprintf(stderr, "grande: %d chiavi uguali, %.2f s\n", same, secs);
    return 0;
}
C
# registro delle identità (audit A38-001/002): k2 REGISTRO percorso... -> "chiave|titolo" per riga
sed -e '/^int main(void) {$/,$d' src/k.c > src/k2.c
cat >> src/k2.c <<'C'
int main(int c, char **v) {
    static char names[64][64];
    for (int i = 2; i < c && i < 66; i++) {
        const char *b = strrchr(v[i], '/') + 1, *dot = strrchr(b, '.');
        snprintf(names[i - 2], sizeof names[0], "%.*s", (int)(dot - b), b);
        add(names[i - 2], v[i], names[i - 2]);
    }
    if (!assign_keys(G, n, "/sd/Roms/MD", v[1])) { puts("ERRORE"); return 0; }
    for (int i = 0; i < n; i++) printf("%s|%s%s\n", G[i].key, G[i].title, G[i].key_unsaved ? "|ATTESA" : "");
    return 0;
}
C
gcc -O2 -Wall -Wextra -Wno-unused-function -Isrc -I$F/common -o k src/k.c && ./k > $W/keys.txt 2>$W/keys.err
gcc -O2 -Wall -Wextra -Wno-unused-function -DKEYS_TEST -Isrc -I$F/common -o k2 src/k2.c
line() { sed -n "${1}p" $W/keys.txt; }
[ "$(line 2)" = "Sonic|Sonic" ] && ok "il gioco nella cartella principale tiene il nome" || ko "principale: $(line 2)"
[ "$(line 1)" = "Sonic [EU]|Sonic [EU]" ] && ok "omonimo in sottocartella: nome [sottocartella], anche nel titolo" || ko "EU: $(line 1)"
[ "$(line 4)" = "Sonic [USA-Hack]|Sonic [USA-Hack]" ] && ok "sottocartella annidata con il trattino" || ko "USA/Hack: $(line 4)"
[ "$(line 3)" = "Tetris|Tetris" ] && ok "nome unico: nulla cambia" || ko "Tetris: $(line 3)"
{ [ "$(line 5)" = "Golf|Golf" ] && [ "$(line 6)" = "Golf [zip]|Golf [zip]" ]; } || { [ "$(line 6)" = "Golf|Golf" ] && [ "$(line 5)" = "Golf [md]|Golf [md]" ]; } && ok "zip e ROM nella stessa cartella: distinti dall'estensione" || ko "Golf: $(line 5) / $(line 6)"

[ "$(line 7)" = "Game|Game" ] && [ "$(line 9)" = "Game [Region]|Game [Region]" ] && [ "$(line 8)" = "Game [Region zip]|Game [Region zip]" ] && ok "Game, Region/Game.md e Region/Game.zip: tre chiavi, la .md tiene la sua" || ko "Region: $(line 7) / $(line 8) / $(line 9)"
[ "$(line 12)" = "X|X" ] && [ "$(line 11)" = "X [A-B]|X [A-B]" ] && [ "$(line 10)" = "X [A-B md]|X [A-B md]" ] && ok "A/B e A-B: chiavi diverse, la meno annidata tiene la sua" || ko "A/B: $(line 10) / $(line 11) / $(line 12)"
[ "$(line 13)" = "Pac [EU]|Pac [EU]" ] && [ "$(line 15)" = "Pac|Pac" ] && [ "$(line 14)" = "Pac [EU md]|Pac [EU md]" ] && ok "un file chiamato davvero \"Pac [EU]\" tiene la chiave" || ko "Pac: $(line 13) / $(line 14) / $(line 15)"
[ "$(cut -d'|' -f1 $W/keys.txt | sort | uniq -d)" = "" ] && ok "nessuna chiave doppia" || ko "doppie: $(cut -d'|' -f1 $W/keys.txt | sort | uniq -d)"
grep -q "^grande: 0 chiavi uguali" $W/keys.err && ok "$(cat $W/keys.err)" || ko "grande: $(cat $W/keys.err)"

echo "--- registro delle identità: le chiavi non cambiano con la libreria (A38-001/002)"
M=/sd/Roms/MD; RG=$W/chiavi/md.tsv; rm -rf $W/chiavi
k2() { ./k2 $RG "$@" | tr '\n' ' ' | sed 's/ $//'; }
[ "$(k2 $M/Region/Game.md)" = "Game|Game" ] && ok "una sola ROM: la chiave è il nome" || ko "sola: $(k2 $M/Region/Game.md)"
r=$(k2 $M/Region/Game.md $M/Game.md)
[ "${r%% *}" = "Game|Game [Region]" ] || [ "${r%% *}" = "Game|Game" ] && [ "$(echo "$r" | cut -d' ' -f2- | cut -d'|' -f1)" != "Game" ] && ok "aggiunto un omonimo più in alto: la ROM di prima tiene la sua chiave ($r)" || ko "omonimo aggiunto: $r"
nuova=$(./k2 $RG $M/Region/Game.md $M/Game.md | sed -n 2p | cut -d'|' -f1)
[ "$(./k2 $RG $M/Game.md | cut -d'|' -f1)" = "$nuova" ] && ok "tolta la prima: l'altra non eredita la sua chiave" || ko "tolta la prima: $(./k2 $RG $M/Game.md)"
[ "$(./k2 $RG $M/Region/Game.md $M/Game.md | sed -n 1p | cut -d'|' -f1)" = "Game" ] && ok "rimessa la prima: ritrova la sua chiave" || ko "rimessa: $(k2 $M/Region/Game.md $M/Game.md)"
before=$(md5sum < $RG); ./k2 $RG > /dev/null
[ "$(md5sum < $RG)" = "$before" ] && ok "emulatore senza giochi (archiviato): il registro resta" || ko "registro cambiato senza giochi"
rm -rf $W/chiavi
./k2 $RG $M/X.md > /dev/null
[ "$(k2 $M/Dir/X.md)" = "X|X" ] && ! grep -q "^X.md" $RG && ok "ROM spostata in una cartella: tiene chiave e salvataggi" || ko "spostata: $(k2 $M/Dir/X.md) / $(cat $RG)"
rm -rf $W/chiavi
r=$(./k2 $RG $M/A/Game.md $M/B/game.md | cut -d'|' -f1 | tr 'A-Z' 'a-z' | sort | uniq -d)
[ -z "$r" ] && ok "nomi uguali tranne le maiuscole: chiavi diverse anche sulla FAT ($(k2 $M/A/Game.md $M/B/game.md))" || ko "maiuscole: $(k2 $M/A/Game.md $M/B/game.md)"
rm -rf $W/chiavi
./k2 $RG $M/Game.md > /dev/null
r=$(KEYS_FAIL_SAVE=1 ./k2 $RG $M/Game.md $M/Nuovo.md | tr '\n' ' ' | sed 's/ $//')
[ "$r" = "Game|Game Nuovo|Nuovo|ATTESA" ] && ok "registro non scritto: la ROM nuova aspetta, quella già registrata si gioca (R39-01)" || ko "non scritto: $r"
[ "$(k2 $M/Game.md $M/Nuovo.md)" = "Game|Game Nuovo|Nuovo" ] && ok "… e appena il registro si scrive parte anche la nuova" || ko "dopo: $(k2 $M/Game.md $M/Nuovo.md)"
rm -rf $W/chiavi; mkdir -p $RG
[ "$(k2 $M/Game.md)" = "ERRORE" ] && ok "registro illeggibile: nessuna chiave assegnata" || ko "illeggibile: $(k2 $M/Game.md)"
rm -rf $W/chiavi

[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors errori"; exit 1; }
