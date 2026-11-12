#!/bin/sh
# PSP in Mosaik: PPSSPP (https://www.ppsspp.org), programma a sé con SDL2 e GLES, compilato da
# toolchain/build-ppsspp.sh. Come Deadcast: il launcher lo avvia come un gioco Linux sotto
# gamewatch e passa il gioco in ARCADE_ROM. Menu breve apre il menu di PPSSPP (con "Esci", che
# torna a Mosaik), Menu tenuto quello di Mosaik.
# Giochi: .iso, .cso, .chd, .pbp. Impostazioni, salvataggi e stati in Arcade/Saves/psp/ppsspp
# (niente cartelle nascoste: il backup le salta).
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/mnt/SDCARD/Arcade}"
ROM="${ARCADE_ROM:-$1}"
echo "ppsspp: $ROM"
[ -f "$ROM" ] || { echo "ppsspp: nessun gioco in $ROM"; exit 1; }

export XDG_CONFIG_HOME="$ROOT/Saves/psp"
export HOME="$ROOT/Saves/psp"
CONF="$XDG_CONFIG_HOME/ppsspp/PSP/SYSTEM"
mkdir -p "$CONF"
# impostazioni pensate per l'A133P alla prima partita (poi restano quelle scelte nel suo menu)
[ -f "$CONF/ppsspp.ini" ] || cp "$DIR/ppsspp.ini" "$CONF/ppsspp.ini"

# i messaggi del kernel sulla console rallentano il cambio dei fotogrammi: spenti mentre si gioca
PRINTK="$(cut -f1 /proc/sys/kernel/printk 2>/dev/null)"
cleanup() { [ -n "$PRINTK" ] && echo "$PRINTK" >/proc/sys/kernel/printk; sync; }
trap cleanup EXIT INT TERM HUP QUIT
echo 1 >/proc/sys/kernel/printk 2>/dev/null

cd "$DIR" || exit 1
"$DIR/PPSSPPSDL" --fullscreen --pause-menu-exit "$ROM"
code=$?
echo "ppsspp: uscita $code"
exit 0
