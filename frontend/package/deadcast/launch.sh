#!/bin/sh
# Dreamcast in Mosaik: Deadcast (https://github.com/tyrannotorus/emulator-dreamcast-deadcast), una
# versione di Flycast per l'A133P che disegna con Vulkan direttamente sullo schermo. Non è un core
# libretro: il launcher lo avvia come un gioco Linux sotto gamewatch e passa il gioco in ARCADE_ROM.
# Menu apre il menu di Deadcast; il menu di Mosaik non c'è ("menu": false in dc.json,
# GAMEWATCH_NOMENU), perché chiudere Deadcast da fuori perderebbe i salvataggi della VMU.
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
# A, B, X e Y del Dreamcast per etichetta (18 novembre, come Flycast): i tasti della console sono
# 0 in basso (B), 1 a destra (A), 2 a sinistra (Y), 3 in alto (X); il file di Deadcast è per
# posizione. Si cambia solo se ha ancora tutti e quattro i tasti di partenza (Deadcast riscrive il
# file in un altro ordine, quindi si guardano le righe; uno cambiato dal menu resta com'è)
M="$DEADCAST_HOME/mappings/$MAPPING"
if grep -qx 'bind0 = 0:btn_a' "$M" && grep -qx 'bind1 = 1:btn_b' "$M" && grep -qx 'bind2 = 2:btn_x' "$M" && grep -qx 'bind3 = 3:btn_y' "$M"; then
    sed -i 's/^bind0 = 0:btn_a$/bind0 = 1:btn_a/; s/^bind1 = 1:btn_b$/bind1 = 0:btn_b/;
            s/^bind2 = 2:btn_x$/bind2 = 3:btn_x/; s/^bind3 = 3:btn_y$/bind3 = 2:btn_y/' "$DEADCAST_HOME/mappings/$MAPPING"
    echo "deadcast: comandi per etichetta"
fi
export DEADCAST_ROM="$ROM"

# modo dello schermo com'era, se Deadcast lo cambia e non lo rimette (per esempio fermato a forza)
FB=/sys/class/graphics/fb0
VSIZE="$(cat $FB/virtual_size 2>/dev/null)"
BPP="$(cat $FB/bits_per_pixel 2>/dev/null)"
# i messaggi del kernel sulla console rallentano il cambio dei fotogrammi: spenti mentre si gioca
PRINTK="$(cut -f1 /proc/sys/kernel/printk 2>/dev/null)"
cleanup() {
	[ -n "$FBSET_BACK" ] && fbset -g $FBSET_BACK 2>/dev/null
	[ -n "$PRINTK" ] && echo "$PRINTK" >/proc/sys/kernel/printk
	[ -n "$BPP" ] && echo "$BPP" >$FB/bits_per_pixel 2>/dev/null
	[ -n "$VSIZE" ] && echo "$VSIZE" >$FB/virtual_size 2>/dev/null
	sync
}
trap cleanup EXIT INT TERM HUP QUIT
echo 1 >/proc/sys/kernel/printk 2>/dev/null

# Deadcast mette le sue 5 pagine dopo 16 pagine dello schermo (come serve a NextUI), alte quanto
# lo schermo: sulla Smart Pro (1280x720, fb0 da 64 MB) non ci stanno e si ferma con "fb0 has too
# few pages". I giochi li disegna comunque a 640x480: se le pagine non bastano lo schermo parte
# già in 640x480 (pagine da 480 righe) e alla fine torna com'era. Sulla Brick Pro ci stanno: nulla
# cambia.
GEOM="$(fbset 2>/dev/null | sed -n 's/^[[:space:]]*geometry[[:space:]]*//p')"
echo "deadcast: schermo (fbset geometry): ${GEOM:-non letto}"
set -- $GEOM
if [ $# -ge 5 ] && [ $((21 * $2)) -gt "$4" ]; then
	# Smart Pro: in 640x480 Deadcast parte (si sente) ma lo schermo resta nero, perché il pannello
	# è ruotato e Deadcast scrive direttamente in fb0 (15 novembre). Solo per le prove
	# (System/prova-deadcast-640.txt); altrimenti si torna subito al menu.
	if [ ! -f "$ROOT/System/prova-deadcast-640.txt" ]; then
		echo "deadcast: schermo ${1}x${2}: Deadcast non funziona ancora su questa console"
		exit 1
	fi
	echo "deadcast: schermo ${1}x${2}, righe $4: troppo poche per 21 pagine, parto in 640x480"
	fbset -g 640 480 640 "$4" "$5" 2>/dev/null || echo "deadcast: fbset non riuscito"
	FBSET_BACK="$GEOM"
fi

cd "$DIR" || exit 1
# uscita 86: nel suo menu si è scelto un altro "core" (emulatore o modulo recomp), si riparte
for run in 1 2 3 4 5; do
	"$DIR/deadcast" "$DISC"
	code=$?
	echo "deadcast: uscita $code"
	[ "$code" = 86 ] || break
done
