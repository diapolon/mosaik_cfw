#!/bin/sh
# Arcade: app per il firmware originale TrimUI (Smart Pro TG5040, Brick Pro TG4040).
# Programmi in questa cartella (Apps/Arcade); dati, temi, core e ROM in /mnt/SDCARD/Arcade.
# Ciclo: launcher -> gioco -> launcher. Il launcher esce con 10 (avvia il gioco scritto in
# System/next.txt), 20 (spegni) o 0 (torna al menu TrimUI).
APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ARCADE_ROOT:-/mnt/SDCARD/Arcade}"
BIN="$APP/bin"
mkdir -p "$ROOT/Logs" "$ROOT/System"
LOG="$ROOT/Logs/avvio.log"
# l'ultimo avvio resta leggibile anche se la console si blocca
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 200000 ] && mv -f "$LOG" "$LOG.old"

export LD_LIBRARY_PATH="$APP/lib:/usr/trimui/lib:/usr/lib:$LD_LIBRARY_PATH"
export HOME="$ROOT"
# modello: il nome sta dentro il programma del menu originale (come fa MinUI)
TRIMUI_MODEL="$(grep -ao 'Trimui [A-Za-z ]*' /usr/trimui/bin/MainUI 2>/dev/null | head -n 1)"
export TRIMUI_MODEL
# variabili per prove sul dispositivo: TRIMUI_JOYMAP, TRIMUI_JOYDEBUG=1, GAMEWATCH_PAD...
[ -f "$ROOT/System/device-env.sh" ] && . "$ROOT/System/device-env.sh"

{
    echo "=== $(date) · $TRIMUI_MODEL · $(uname -r)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
} >> "$LOG"
OLD_GOV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
# come le app del firmware: niente sospensione automatica mentre gira l'app
# (lo spegnimento automatico è gestito dal launcher)
echo 1 > /tmp/stay_awake

finish() {
    rm -f /tmp/stay_awake
    # al ritorno al menu originale si rimette il governor di prima
    [ -n "$OLD_GOV" ] && echo "$OLD_GOV" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
    sync
}

while :; do
    "$BIN/launcher" --root "$ROOT" >> "$LOG" 2>&1
    rc=$?
    echo "launcher: uscita $rc" >> "$LOG"
    case $rc in
    10) ;;
    20) finish; poweroff; exit 0 ;;
    *) finish; exit 0 ;;
    esac

    # next.txt: tipo <TAB> core <TAB> rom/script <TAB> titolo [<TAB> resume]
    IFS="$(printf '\t')" read -r kind core rom title resume < "$ROOT/System/next.txt"
    rm -f "$ROOT/System/next.txt"

    if [ "$kind" = "linux" ]; then
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
    "$BIN/runner" "$@" >> "$LOG" 2>&1
    rc=$?
    echo "runner: uscita $rc" >> "$LOG"
    [ $rc -eq 20 ] && { finish; poweroff; exit 0; }
done
