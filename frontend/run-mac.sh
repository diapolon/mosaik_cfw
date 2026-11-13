#!/bin/sh
# Ciclo launcher -> gioco -> launcher, come farà lo script di avvio sulla console.
# Uso: ./run-mac.sh            (SD simulata in ./sdcard)
#      ROOT=/percorso ./run-mac.sh
cd "$(dirname "$0")" || exit 1
ROOT="${ROOT:-$PWD/sdcard}"
BIN="$PWD/build"
# Scarica immagini è un'app dello Store: sul Mac, se non è installata nella SD simulata, quella compilata
[ -x "$ROOT/Apps/scraper/scraper" ] || export ARCADE_SCRAPER_APP="${ARCADE_SCRAPER_APP:-$BIN/scraper}"

while :; do
    "$BIN/launcher" --root "$ROOT"
    rc=$?
    [ "$rc" -eq 10 ] || exit 0

    # next.txt: tipo <TAB> core <TAB> rom/script <TAB> titolo [<TAB> resume]
    IFS="$(printf '\t')" read -r kind core rom title resume system key < "$ROOT/System/next.txt"
    rm -f "$ROOT/System/next.txt"

    if [ "$kind" = "linux" ]; then
        # variabili comuni (System/linux-env.sh) e del gioco (next.env, da game.json);
        # gamewatch chiude il gioco se si tiene premuto Menu (sulla console)
        (
            cd "$(dirname "$rom")" || exit 1
            [ -f "$ROOT/System/linux-env.sh" ] && . "$ROOT/System/linux-env.sh"
            [ -f "$ROOT/System/next.env" ] && . "$ROOT/System/next.env"
            exec "$BIN/gamewatch" -- sh "./$(basename "$rom")"
        ) >> "$ROOT/Logs/linux-games.log" 2>&1
        rm -f "$ROOT/System/next.env"
        continue
    fi

    set -- --root "$ROOT" --core "$core" --rom "$rom" --title "$title"
    [ "$resume" = "resume" ] && set -- "$@" --resume
    # console del gioco: salvataggi in Saves/<console> (issue #5)
    case "$system" in ""|-) ;; *) set -- "$@" --system "$system" ;; esac
    # identità del gioco per i suoi file (diversa dal nome solo per due ROM omonime)
    case "$key" in ""|-) ;; *) set -- "$@" --key "$key" ;; esac
    "$BIN/runner" "$@"
    [ $? -eq 20 ] && exit 0
done
