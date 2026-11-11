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
mkdir -p "$DATA/Cores" "$DATA/Roms/FBNEO" "$DATA/Roms/MAME2003P" "$DATA/Roms/MAME2010" "$DATA/Roms/NES" "$DATA/Roms/MD" "$DATA/Roms/SMS" "$DATA/Roms/GG" "$DATA/Roms/PCE" "$DATA/Roms/SNES" \
         "$DATA/Roms/GB" "$DATA/Roms/GBC" "$DATA/Roms/GBA" "$DATA/Roms/NGP" "$DATA/Roms/PS" "$DATA/Bios" "$DATA/Saves" "$DATA/System" \
         "$DATA/Logs" "$DATA/Music" "$DATA/Linux" "$DATA/Media"
cp "$DIST/cores/fbneo_libretro.so" "$DATA/Cores/fbneo_libretro.so"
cp "$DIST/cores/mame2003_plus_libretro.so" "$DATA/Cores/mame2003p_libretro.so"
for c in mame2010 fceumm genesis_plus_gx snes9x gambatte mgba mednafen_pce_fast mednafen_ngp pcsx_rearmed; do cp "$DIST/cores/${c}_libretro.so" "$DATA/Cores/"; done
cp -RL "$FE/sdcard/Themes" "$FE/sdcard/Filters" "$FE/sdcard/Bezels" "$DATA/"
mkdir -p "$DATA/Lang" "$DATA/Emulators" "$DATA/Dats"
cp "$FE/lang/"*.json "$DATA/Lang/"
cp "$FE/emulators/"*.json "$DATA/Emulators/"
# solo gli indici arcade fatti dai DAT (build_index.py: <id>.tsv con accanto <id>.roms). Il resto
# lo scrive lo scraper sulla console: generi e storie (arcadedb.tsv, history), indici delle
# console e dati libretro (dati di terzi, nell'immagine no; quelli della SD del Mac sono di prova)
for r in "$FE/sdcard/Dats/"*.roms; do
    cp -L "$r" "${r%.roms}.tsv" "$DATA/Dats/"
done
mkdir -p "$DATA/Licenses"
cp "$TOP/licenses/"* "$DATA/Licenses/"
[ "$1" = "--media" ] && cp -RL "$FE/sdcard/Media/." "$DATA/Media/"

# certificati per lo scraper (HTTPS), elenco Mozilla distribuito da curl
[ -f "$TOP/deps/cacert.pem" ] || curl -fsSL https://curl.se/ca/cacert.pem -o "$TOP/deps/cacert.pem"
cp "$TOP/deps/cacert.pem" "$DATA/System/cacert.pem"

cat > "$DATA/Roms/LEGGIMI.txt" <<'EOF'
FBNEO: ROM per FinalBurn Neo (romset FBNeo attuale), anche in sottocartelle (neogeo/, cps2/...).
MAME2003P: ROM per MAME 2003-Plus (romset 0.78-plus).
MAME2010: ROM per MAME 2010 (romset MAME 0.139), per i giochi che mancano agli altri due.
NES: giochi NES/Famicom (.nes, .fds, .unf), anche zippati, uno per zip.
MD: giochi Mega Drive/Genesis (.md, .gen, .smd, .bin), anche zippati, uno per zip.
SMS, GG: giochi Master System (.sms, .sg) e Game Gear (.gg), anche zippati, uno per zip.
PCE: giochi PC Engine su HuCard (.pce), anche zippati; i CD-ROM non ancora.
NGP: giochi Neo Geo Pocket e Pocket Color (.ngp, .ngc), anche zippati.
SNES: giochi Super Nintendo/Super Famicom (.sfc, .smc), anche zippati, uno per zip.
GB, GBC, GBA: giochi Game Boy (.gb), Game Boy Color (.gbc) e Game Boy Advance (.gba),
anche zippati, uno per zip. Il BIOS GBA (gba_bios.bin) in Bios/ non serve, ma se c'è si usa.
PS: giochi PlayStation (.chd consigliato, .cue con i suoi .bin, .pbp). Più dischi: un file .m3u
con i nomi dei dischi, uno per riga, nella stessa cartella; in elenco compare solo il .m3u.
Il BIOS PlayStation (scph5501.bin, scph1001.bin...) in Bios/ è consigliato ma non obbligatorio.
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
