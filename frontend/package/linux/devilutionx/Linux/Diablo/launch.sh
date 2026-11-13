#!/bin/sh
# Diablo con DevilutionX (https://github.com/diasurgical/devilutionX, Sustainable Use License:
# gratis e non commerciale), compilato da toolchain/build-devilutionx.sh. Pacchetto della
# cartella Giochi Linux dello Store, senza i dati del gioco: DIABDAT.MPQ del gioco originale (CD
# o GOG), o spawn.mpq della versione shareware, va copiato in questa cartella; per Hellfire anche
# hellfire.mpq, hfmonk.mpq, hfmusic.mpq e hfvoice.mpq. Sono file dell'utente: togliendo il
# pacchetto restano. Partite e impostazioni in Arcade/Saves/diablo. Il pad della console è un
# controller Xbox 360 per SDL: DevilutionX lo usa da sé.
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/mnt/SDCARD/Arcade}"
SAVE="$ROOT/Saves/diablo"
mkdir -p "$SAVE"
if [ ! -f "$DIR/DIABDAT.MPQ" ] && [ ! -f "$DIR/diabdat.mpq" ] && [ ! -f "$DIR/spawn.mpq" ] && [ ! -f "$DIR/SPAWN.MPQ" ]; then
    echo "diablo: mancano i dati del gioco (DIABDAT.MPQ o spawn.mpq) in $DIR"
    # un messaggio sullo schermo, nella lingua della console (solo lettere maiuscole e cifre)
    if grep -q '"lang": *"it"' "$ROOT/System/settings.json" 2>/dev/null; then
        FB_TITLE="DIABLO" fbmsg show rosso 10 "COPIA DIABDAT.MPQ" "IN ARCADE/LINUX/DIABLO" 2>/dev/null
    else
        FB_TITLE="DIABLO" fbmsg show rosso 10 "COPY DIABDAT.MPQ" "TO ARCADE/LINUX/DIABLO" 2>/dev/null
    fi
    exit 1
fi
# impostazioni pensate per la console alla prima partita (poi restano quelle scelte nel menu)
[ -f "$SAVE/diablo.ini" ] || cp "$DIR/diablo.ini" "$SAVE/diablo.ini"
cd "$DIR" || exit 1
"$DIR/devilutionx" --data-dir "$DIR" --save-dir "$SAVE" --config-dir "$SAVE"
echo "diablo: uscita $?"
sync
exit 0
