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
    sh -c 'cp /build/out/bin/* /dist/SD/Apps/Arcade/bin/ && mkdir -p /dist/cores /dist/apps && cp /build/out/cores/*.so /dist/cores/ && cp /build/out/apps/* /dist/apps/'
cp "$FE/package/trimui/launch.sh" "$FE/package/trimui/config.json" "$FE/package/trimui/icon.png" "$APP/"
chmod +x "$APP/launch.sh" "$APP/bin/"*

# dati del sistema (i file dell'utente, come settings.json e i salvataggi, non ci sono:
# aggiornando non vengono sovrascritti)
mkdir -p "$DATA/Cores" "$DATA/Roms/FBNEO" "$DATA/Roms/MAME2003P" "$DATA/Roms/MAME2010" "$DATA/Roms/NES" "$DATA/Roms/MD" "$DATA/Roms/SMS" "$DATA/Roms/GG" "$DATA/Roms/PCE" "$DATA/Roms/SNES" \
         "$DATA/Roms/GB" "$DATA/Roms/GBC" "$DATA/Roms/GBA" "$DATA/Roms/NGP" "$DATA/Roms/PS" "$DATA/Roms/DC" "$DATA/Bios" "$DATA/Saves" "$DATA/System" \
         "$DATA/Logs" "$DATA/Music" "$DATA/Linux" "$DATA/Apps" "$DATA/Media"
# Emulatori: non sono nella base, sono pacchetti dello Store (un pacchetto per core, con i suoi
# Emulators/<id>.json e gli indici arcade). Qui si preparano in dist/store-src/<pacchetto>/ con i
# percorsi della scheda; toolchain/make-store.py li impacchetta e firma il catalogo.
PKG="$DIST/store-src"
rm -rf "$PKG"
pkg() {   # pkg PACCHETTO FILE_DEL_CORE NOME_SULLA_SCHEDA EMULATORE...
    id=$1 so=$2 dst=$3; shift 3
    mkdir -p "$PKG/$id/Cores" "$PKG/$id/Emulators"
    [ "$so" = - ] || cp "$DIST/cores/$so" "$PKG/$id/Cores/$dst"
    for e in "$@"; do
        cp "$FE/emulators/$e.json" "$PKG/$id/Emulators/"
        # indice arcade fatto dai DAT (build_index.py: <id>.tsv con accanto <id>.roms)
        if [ -f "$FE/sdcard/Dats/$e.roms" ]; then
            mkdir -p "$PKG/$id/Dats"
            cp -L "$FE/sdcard/Dats/$e.roms" "$FE/sdcard/Dats/$e.tsv" "$PKG/$id/Dats/"
        fi
    done
}
pkg fbneo fbneo_libretro.so fbneo_libretro.so fbneo
pkg mame2003p mame2003_plus_libretro.so mame2003p_libretro.so mame2003p
pkg mame2010 mame2010_libretro.so mame2010_libretro.so mame2010
pkg fceumm fceumm_libretro.so fceumm_libretro.so nes
pkg genesis_plus_gx genesis_plus_gx_libretro.so genesis_plus_gx_libretro.so md sms gg
pkg snes9x snes9x_libretro.so snes9x_libretro.so snes
pkg gambatte gambatte_libretro.so gambatte_libretro.so gb gbc
pkg mgba mgba_libretro.so mgba_libretro.so gba
pkg pce_fast mednafen_pce_fast_libretro.so mednafen_pce_fast_libretro.so pce
pkg ngp mednafen_ngp_libretro.so mednafen_ngp_libretro.so ngp
pkg pcsx pcsx_rearmed_libretro.so pcsx_rearmed_libretro.so ps
# Dreamcast: Deadcast, programma a sé (non un core libretro), dal pacchetto del suo autore
# scaricato a versione fissa da toolchain/fetch-deadcast.sh; lo avvia Cores/deadcast/launch.sh
DZ=$(ls "$TOP"/deps/deadcast/DC-DEADCAST-*.pak.zip 2>/dev/null | tail -1)
if [ -n "$DZ" ]; then
    pkg deadcast - - dc
    mkdir -p "$PKG/deadcast/Cores/deadcast"
    unzip -q -o "$DZ" deadcast 'config/*' 'licenses/*' -d "$PKG/deadcast/Cores/deadcast"
    cp "$FE/package/deadcast/launch.sh" "$PKG/deadcast/Cores/deadcast/launch.sh"
    chmod +x "$PKG/deadcast/Cores/deadcast/deadcast" "$PKG/deadcast/Cores/deadcast/launch.sh"
else
    echo "attenzione: manca Deadcast (sh toolchain/fetch-deadcast.sh): niente pacchetto Dreamcast" >&2
fi
# App (cartella App dello Store): Apps/<app> con il programma, launch.sh, app.json, icona e lingue
app() {   # app NOME: frontend/package/apps/NOME più il programma compilato dist/apps/NOME
    mkdir -p "$PKG/$1/Apps"
    cp -R "$FE/package/apps/$1" "$PKG/$1/Apps/$1"
    cp "$DIST/apps/$1" "$PKG/$1/Apps/$1/$1"
    chmod +x "$PKG/$1/Apps/$1/$1" "$PKG/$1/Apps/$1/launch.sh"
}
app files
app viewer
app music
app sdtest
cp -RL "$FE/sdcard/Themes" "$FE/sdcard/Filters" "$FE/sdcard/Bezels" "$DATA/"
mkdir -p "$DATA/Lang" "$DATA/Emulators" "$DATA/Dats"
cp "$FE/lang/"*.json "$DATA/Lang/"
# Emulators e Dats restano vuoti: li riempiono i pacchetti dello Store. Gli altri indici li
# scrive lo scraper sulla console (generi e storie, indici delle console, dati libretro).
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
DC: giochi Dreamcast (.chd consigliato, .gdi con le sue tracce, .cdi), anche .m3u. Serve il BIOS
dc_boot.bin in Bios/dc/ (oppure in Bios/). Emulatore: Deadcast.
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
