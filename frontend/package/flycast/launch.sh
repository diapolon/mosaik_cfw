#!/bin/sh
# Dreamcast in Mosaik con Flycast (https://github.com/flyinghead/flycast): il programma a sé con
# SDL2 e GLES, compilato da toolchain/build-flycast.sh. Bozza per la prima prova su console.
# A differenza di Deadcast disegna attraverso la SDL2 della console, che gestisce lo schermo
# ruotato della Smart Pro. Come Deadcast e PPSSPP il launcher lo avvia come un gioco Linux sotto
# gamewatch e passa il gioco in ARCADE_ROM. Menu breve apre il menu di Flycast (con "Esci", che torna
# a Mosaik); Menu tenuto 1 s apre il menu di Mosaik sopra il gioco (17 novembre: a differenza di
# Deadcast, Flycast disegna con SDL e il livello del menu si vede). "Esci al menu" da lì manda
# SIGTERM, che SDL trasforma in una chiusura ordinata di Flycast.
# Dischi: .chd, .gdi, .cdi, .cue, oppure un .m3u che ne nomina uno. BIOS (dc_boot.bin,
# dc_flash.bin, naomi.zip) in Arcade/Bios/dc o Arcade/Bios; senza dc_boot.bin Flycast usa il suo
# BIOS ricostruito (HLE). Impostazioni, comandi, VMU e stati in Arcade/Saves/dc/flycast (niente
# cartelle nascoste: il backup le salta; i file di Deadcast in Saves/dc restano separati).
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/mnt/SDCARD/Arcade}"
ROM="${ARCADE_ROM:-$1}"
echo "flycast: $ROM"

case "$ROM" in
*.m3u | *.M3U)
	# Flycast non legge i .m3u: il primo disco elencato, accanto al .m3u se il percorso non è
	# assoluto (il cambio disco si fa dal menu di Flycast)
	DISC="$(tr -d '\r' <"$ROM" | grep -v '^[[:space:]]*$' | grep -v '^#' | head -n 1)"
	case "$DISC" in /*) ;; *) DISC="$(dirname "$ROM")/$DISC" ;; esac
	;;
*) DISC="$ROM" ;;
esac
[ -f "$DISC" ] || { echo "flycast: nessun disco in $DISC"; exit 1; }

# Flycast su Linux mette le impostazioni in $XDG_CONFIG_HOME/flycast (emu.cfg, mappings/) e i
# dati in $XDG_DATA_HOME/flycast (VMU, flash, stati): tutti e due in Saves/dc/flycast.
# I BIOS li cerca anche nelle cartelle di FLYCAST_BIOS_PATH (separate da ":").
export HOME="$ROOT/Saves/dc"
export XDG_CONFIG_HOME="$HOME"
export XDG_DATA_HOME="$HOME"
export FLYCAST_BIOS_PATH="$ROOT/Bios/dc:$ROOT/Bios"
CONF="$HOME/flycast"
mkdir -p "$CONF/mappings" "$ROOT/Bios/dc"

# impostazioni pensate per l'A133P alla prima partita (poi restano quelle scelte nel suo menu):
# GLES (pvr.rend 0; 4 è Vulkan), 640x480 interni, audio SDL, niente rete, copertine né obiettivi
# (la build non ha HTTPS)
[ -f "$CONF/emu.cfg" ] || cat >"$CONF/emu.cfg" <<'CFG'
[achievements]
Enabled = no

[audio]
backend = sdl2

[config]
Dreamcast.Broadcast = 0
Dreamcast.Cable = 0
Dreamcast.Language = 1
Dreamcast.Region = 1
Dynarec.Enabled = yes
FetchBoxart = no
UploadCrashLogs = no
pvr.rend = 0
rend.DelayFrameSwapping = yes
rend.PerStripSorting = no
rend.Resolution = 480
rend.ShowFPS = no
rend.ThreadedRendering = yes
rend.vsync = yes

[network]
Enable = no

[window]
fullscreen = yes
CFG

# Comandi: i tasti della console sono un pad "Xbox 360 Controller" (trimui_inputd), con i
# numeri dei tasti di Linux: 0 A (in basso), 1 B, 2 X, 3 Y, 4 L1, 5 R1, 6 Select, 7 Start,
# 8 Menu; croce sul cappello (256-259), levette sugli assi 0-1 e 3-4, L2/R2 sugli assi 2 e 5
# (gli stessi numeri del file di Deadcast). Come il pad del Dreamcast per posizione; Menu e
# Select aprono il menu di Flycast. Se il nome del pad è un altro, il file non si usa e valgono
# i comandi predefiniti di Flycast (Select apre il menu).
MAPPING="$CONF/mappings/SDL_Xbox 360 Controller.cfg"
[ -f "$MAPPING" ] || cat >"$MAPPING" <<'MAP'
[analog]
bind0 = 0-:axis_left
bind1 = 0+:axis_right
bind2 = 1-:axis_up
bind3 = 1+:axis_down
bind4 = 2+:axis_trigger_left
bind5 = 3-:axis2_left
bind6 = 3+:axis2_right
bind7 = 4-:axis2_up
bind8 = 4+:axis2_down
bind9 = 5+:axis_trigger_right

[digital]
bind0 = 0:btn_a
bind1 = 1:btn_b
bind2 = 2:btn_x
bind3 = 3:btn_y
bind4 = 4:btn_trigger_left
bind5 = 5:btn_trigger_right
bind6 = 6:btn_menu
bind7 = 7:btn_start
bind8 = 8:btn_menu
bind9 = 256:btn_dpad1_up
bind10 = 257:btn_dpad1_down
bind11 = 258:btn_dpad1_left
bind12 = 259:btn_dpad1_right

[emulator]
dead_zone = 10
mapping_name = Mosaik
rumble_power = 100
saturation = 100
version = 3
MAP

# i messaggi del kernel sulla console rallentano il cambio dei fotogrammi: spenti mentre si gioca
PRINTK="$(cut -f1 /proc/sys/kernel/printk 2>/dev/null)"
cleanup() { [ -n "$PRINTK" ] && echo "$PRINTK" >/proc/sys/kernel/printk; sync; }
trap cleanup EXIT INT TERM HUP QUIT
echo 1 >/proc/sys/kernel/printk 2>/dev/null

cd "$DIR" || exit 1
# a schermo intero sempre, anche se nel suo menu si è tolto (valore che non si salva)
"$DIR/flycast" -config window:fullscreen=yes "$DISC"
code=$?
echo "flycast: uscita $code"
exit 0
