#!/bin/sh
# Nintendo 64 in Mosaik: mupen64plus (https://mupen64plus.org), programma a sé con SDL2 e GLES2,
# compilato da toolchain/build-mupen64plus.sh. Come PPSSPP e Deadcast: il launcher lo avvia come
# un gioco Linux sotto gamewatch e passa il gioco in ARCADE_ROM. Non ha un suo menu: Menu tenuto
# apre quello di Mosaik, e uscendo da lì l'emulatore si ferma e scrive i salvataggi.
# Giochi: .z64, .n64, .v64 (non zippati). Impostazioni, salvataggi della cartuccia e stati in
# Arcade/Saves/n64 (niente cartelle nascoste: il backup le salta).
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/mnt/SDCARD/Arcade}"
ROM="${ARCADE_ROM:-$1}"
echo "mupen64plus: $ROM"
[ -f "$ROM" ] || { echo "mupen64plus: nessun gioco in $ROM"; exit 1; }

CONF="$ROOT/Saves/n64/mupen64plus"
mkdir -p "$CONF" "$ROOT/Saves/n64/save" "$ROOT/Saves/n64/state" "$ROOT/Screenshots"
# comandi e impostazioni di partenza alla prima partita (poi restano quelli di mupen64plus.cfg)
[ -f "$CONF/mupen64plus.cfg" ] || cp "$DIR/mupen64plus.cfg" "$CONF/mupen64plus.cfg"

# misura dello schermo del modello: Brick Pro 1024x768, Smart Pro 1280x720
RES=1280x720
case "$(readlink /run/fw 2>/dev/null)" in *tg4040*) RES=1024x768 ;; esac
# motore grafico: Rice è il più leggero; "glide64mk2" in Saves/n64/video.txt sceglie l'altro
GFX=mupen64plus-video-rice
[ "$(cat "$ROOT/Saves/n64/video.txt" 2>/dev/null)" = glide64mk2 ] && GFX=mupen64plus-video-glide64mk2

# i messaggi del kernel sulla console rallentano il cambio dei fotogrammi: spenti mentre si gioca
PRINTK="$(cut -f1 /proc/sys/kernel/printk 2>/dev/null)"
cleanup() { [ -n "$PRINTK" ] && echo "$PRINTK" >/proc/sys/kernel/printk; sync; }
trap cleanup EXIT
echo 1 >/proc/sys/kernel/printk 2>/dev/null

cd "$DIR" || exit 1
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
# in secondo piano con wait: SIGTERM di gamewatch si passa all'emulatore, che ferma il gioco e
# scrive i salvataggi prima di uscire
"$DIR/mupen64plus" --corelib "$DIR/lib/libmupen64plus.so.2" --plugindir "$DIR/lib" --datadir "$DIR/share" \
    --configdir "$CONF" --sshotdir "$ROOT/Screenshots" --fullscreen --resolution "$RES" --noosd \
    --gfx "$GFX" --audio mupen64plus-audio-sdl --input mupen64plus-input-sdl --rsp mupen64plus-rsp-hle \
    --set "Core[SaveSRAMPath]=$ROOT/Saves/n64/save/" --set "Core[SaveStatePath]=$ROOT/Saves/n64/state/" \
    --set "Core[R4300Emulator]=2" "$ROM" &
PID=$!
trap 'kill -TERM $PID 2>/dev/null' TERM INT HUP QUIT
wait $PID
code=$?
# dopo un segnale wait torna subito: si aspetta che l'emulatore abbia finito di scrivere
kill -0 $PID 2>/dev/null && wait $PID
echo "mupen64plus: uscita $code"
exit 0
