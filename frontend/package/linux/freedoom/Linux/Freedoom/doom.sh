#!/bin/sh
# Doom in Mosaik: Crispy Doom (https://github.com/fabiangreffrath/crispy-doom, GPL 2) compilato da
# toolchain/build-linux-games.sh, con i dati di Freedoom (https://freedoom.github.io, BSD).
# Pacchetto della cartella Giochi Linux dello Store: lo usano Freedoom/launch.sh e
# Freedoom2/launch.sh, con il file WAD come argomento. Impostazioni e partite salvate in
# Arcade/Saves/doom (niente cartelle nascoste: il backup le salta), che restano anche togliendo il
# pacchetto. I tasti della console arrivano come tasti della tastiera (game.json, "keys").
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/mnt/SDCARD/Arcade}"
WAD="$1"
echo "doom: $WAD"
[ -f "$WAD" ] || { echo "doom: manca $WAD"; exit 1; }

export XDG_DATA_HOME="$ROOT/Saves/doom"
CONF="$XDG_DATA_HOME/crispy-doom"
mkdir -p "$CONF"
# impostazioni pensate per la console alla prima partita (poi restano quelle scelte nel menu)
[ -f "$CONF/default.cfg" ] || cp "$DIR/config/default.cfg" "$CONF/default.cfg"
[ -f "$CONF/crispy-doom.cfg" ] || cp "$DIR/config/crispy-doom.cfg" "$CONF/crispy-doom.cfg"

cd "$DIR" || exit 1
"$DIR/crispy-doom" -iwad "$WAD"
echo "doom: uscita $?"
sync
exit 0
