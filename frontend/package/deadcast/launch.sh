#!/bin/sh
# Dreamcast in Mosaik: Deadcast (https://github.com/tyrannotorus/emulator-dreamcast-deadcast), una
# versione di Flycast per l'A133P che disegna con Vulkan direttamente sullo schermo. Non è un core
# libretro: il launcher lo avvia come un gioco Linux (sotto gamewatch: Menu breve apre il menu di
# Deadcast, Menu tenuto quello di Mosaik) e passa il gioco in ARCADE_ROM.
# Dischi: .chd, .gdi, .cdi, oppure un .m3u che ne nomina uno. BIOS (dc_boot.bin, naomi.zip) in
# Arcade/Bios/dc o Arcade/Bios. Impostazioni, VMU e stati in Arcade/Saves/dc.
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/mnt/SDCARD/Arcade}"
ROM="${ARCADE_ROM:-$1}"
echo "deadcast: $ROM"

case "$ROM" in
*.m3u | *.M3U)
	# il primo disco elencato, accanto al .m3u se il percorso non è assoluto
	DISC="$(tr -d '\r' <"$ROM" | grep -v '^[[:space:]]*$' | grep -v '^#' | head -n 1)"
	case "$DISC" in /*) ;; *) DISC="$(dirname "$ROM")/$DISC" ;; esac
	;;
*) DISC="$ROM" ;;
esac
[ -f "$DISC" ] || { echo "deadcast: nessun disco in $DISC"; exit 1; }

export DEADCAST_HOME="$ROOT/Saves/dc"
export HOME="$DEADCAST_HOME"
mkdir -p "$DEADCAST_HOME/mappings" "$ROOT/Bios/dc"
export DEADCAST_BIOS_PATH="$ROOT/Bios/dc:$ROOT/Bios"
# impostazioni e comandi predefiniti alla prima partita (i tasti della console sono un pad Xbox 360)
[ -f "$DEADCAST_HOME/emu.cfg" ] || cp "$DIR/config/emu.cfg" "$DEADCAST_HOME/emu.cfg"
MAPPING="SDL_Xbox 360 Controller.cfg"
[ -f "$DEADCAST_HOME/mappings/$MAPPING" ] || cp "$DIR/config/$MAPPING" "$DEADCAST_HOME/mappings/$MAPPING"
export DEADCAST_ROM="$ROM"

# modo dello schermo com'era, se Deadcast lo cambia e non lo rimette (per esempio fermato a forza)
FB=/sys/class/graphics/fb0
VSIZE="$(cat $FB/virtual_size 2>/dev/null)"
BPP="$(cat $FB/bits_per_pixel 2>/dev/null)"
# i messaggi del kernel sulla console rallentano il cambio dei fotogrammi: spenti mentre si gioca
PRINTK="$(cut -f1 /proc/sys/kernel/printk 2>/dev/null)"
cleanup() {
	[ -n "$PRINTK" ] && echo "$PRINTK" >/proc/sys/kernel/printk
	[ -n "$BPP" ] && echo "$BPP" >$FB/bits_per_pixel 2>/dev/null
	[ -n "$VSIZE" ] && echo "$VSIZE" >$FB/virtual_size 2>/dev/null
	sync
}
trap cleanup EXIT INT TERM HUP QUIT
echo 1 >/proc/sys/kernel/printk 2>/dev/null

cd "$DIR" || exit 1
# uscita 86: nel suo menu si è scelto un altro "core" (emulatore o modulo recomp), si riparte
for run in 1 2 3 4 5; do
	"$DIR/deadcast" "$DISC"
	code=$?
	echo "deadcast: uscita $code"
	[ "$code" = 86 ] || break
done
