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
emulator_t EMUS[16];
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

[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors errori"; exit 1; }
