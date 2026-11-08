#!/bin/sh
# Prepara la cartella da copiare sulla SD della console (firmware originale TrimUI):
#   dist/SD/Apps/Arcade   app per il menu TrimUI (icona, launch.sh, programmi)
#   dist/SD/Arcade        temi, lingue, filtri, core, indici dei giochi; qui vanno ROM e BIOS
# e lo comprime in dist/Arcade-trimui.zip. Va lanciato sul Mac, dopo build-deps.sh,
# Makefile.tg5040 e build-cores.sh. Con --media copia anche le immagini scaricate.
set -e
cd "$(dirname "$0")/.."
TOP="$PWD"
FE="$TOP/frontend"
DIST="$TOP/dist"
SD="$DIST/SD"
APP="$SD/Apps/Arcade"
DATA="$SD/Arcade"

rm -rf "$SD"
mkdir -p "$APP/bin" "$APP/lib" "$DATA"

# programmi e core compilati, dal volume Docker
docker run --rm -v trimui-build:/build -v "$DIST":/dist alpine:3.20 \
    sh -c 'cp /build/out/bin/* /dist/SD/Apps/Arcade/bin/ && mkdir -p /dist/cores && cp /build/out/cores/*.so /dist/cores/'
cp "$FE/package/trimui/launch.sh" "$FE/package/trimui/config.json" "$FE/package/trimui/icon.png" "$APP/"
chmod +x "$APP/launch.sh" "$APP/bin/"*

# dati del sistema (i file dell'utente, come settings.json e i salvataggi, non ci sono:
# aggiornando non vengono sovrascritti)
mkdir -p "$DATA/Cores" "$DATA/Roms/FBNEO" "$DATA/Roms/MAME2003P" "$DATA/Bios" "$DATA/Saves" "$DATA/System" \
         "$DATA/Logs" "$DATA/Music" "$DATA/Linux" "$DATA/Media"
cp "$DIST/cores/fbneo_libretro.so" "$DATA/Cores/fbneo_libretro.so"
cp "$DIST/cores/mame2003_plus_libretro.so" "$DATA/Cores/mame2003p_libretro.so"
cp -RL "$FE/sdcard/Themes" "$FE/sdcard/Filters" "$FE/sdcard/Bezels" "$DATA/"
mkdir -p "$DATA/Lang" "$DATA/Emulators" "$DATA/Dats"
cp "$FE/lang/"*.json "$DATA/Lang/"
cp "$FE/emulators/"*.json "$DATA/Emulators/"
cp -RL "$FE/sdcard/Dats/." "$DATA/Dats/"
# generi e storie dei giochi li scarica lo scraper sulla console: dati di terzi, nell'immagine no
rm -rf "$DATA/Dats/arcadedb.tsv" "$DATA/Dats/history"
mkdir -p "$DATA/Licenses"
cp "$TOP/site/assets/licenses/"* "$DATA/Licenses/"
[ "$1" = "--media" ] && cp -RL "$FE/sdcard/Media/." "$DATA/Media/"

# certificati per lo scraper (HTTPS), elenco Mozilla distribuito da curl
[ -f "$TOP/deps/cacert.pem" ] || curl -fsSL https://curl.se/ca/cacert.pem -o "$TOP/deps/cacert.pem"
cp "$TOP/deps/cacert.pem" "$DATA/System/cacert.pem"

cat > "$DATA/Roms/LEGGIMI.txt" <<'EOF'
FBNEO: ROM per FinalBurn Neo (romset FBNeo attuale), anche in sottocartelle (neogeo/, cps2/...).
MAME2003P: ROM per MAME 2003-Plus (romset 0.78-plus).
Il BIOS Neo Geo (neogeo.zip) va in Bios/ oppure accanto ai giochi Neo Geo.
Le ROM possono stare anche altrove: in Emulators/<id>.json "roms" accetta un percorso
assoluto, per esempio "/mnt/SDCARD/Roms/FBNEO".
EOF
cat > "$DATA/System/device-env.sh.esempio" <<'EOF'
# Rinominare in device-env.sh per attivare. Lo leggono Apps/Arcade/launch.sh (app sul firmware
# originale) e il sistema su SD all'avvio.
# Scrive nel log (Logs/Arcade.log) il numero di ogni pulsante premuto:
# export TRIMUI_JOYDEBUG=1
# Sistema su SD: salva in Logs/gioco.png ciò che il gioco ha disegnato dopo 5 secondi:
# export ARCADE_DEBUG_SHOT=1
# Pulsanti dei tasti integrati, se diversi da quelli previsti (numeri del joystick SDL):
# export TRIMUI_JOYMAP="a=1,b=0,x=3,y=2,l=4,r=5,select=6,start=7,menu=8,l2=a2,r2=a5"
EOF

rm -rf "$DIST/cores"
find "$SD" -name .DS_Store -delete
cd "$SD" && rm -f "$DIST/Arcade-trimui.zip" && zip -qr "$DIST/Arcade-trimui.zip" Apps Arcade
echo "pronto: $DIST/Arcade-trimui.zip ($(du -h "$DIST/Arcade-trimui.zip" | cut -f1))"
