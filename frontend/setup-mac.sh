#!/bin/sh
# Prepara la SD simulata (./sdcard) sul Mac: temi, core, indici dei giochi, cartelle ROM.
# Le ROM non vengono copiate: si collegano le cartelle indicate in FBNEO_ROMS, MAME_ROMS,
# NES_ROMS, MD_ROMS, SNES_ROMS, GB_ROMS, GBC_ROMS e GBA_ROMS.
set -e
cd "$(dirname "$0")"
DEPS="${DEPS:-$PWD/../deps}"
SD="$PWD/sdcard"
FBNEO_ROMS="${FBNEO_ROMS:-}"
MAME_ROMS="${MAME_ROMS:-}"
NES_ROMS="${NES_ROMS:-}"
MD_ROMS="${MD_ROMS:-}"
SNES_ROMS="${SNES_ROMS:-}"
GB_ROMS="${GB_ROMS:-}"
GBC_ROMS="${GBC_ROMS:-}"
GBA_ROMS="${GBA_ROMS:-}"

mkdir -p "$SD"/Roms "$SD"/Bios "$SD"/Saves "$SD"/Cores "$SD"/Dats "$SD"/Media "$SD"/System "$SD"/Logs "$SD"/Linux "$SD"/Themes "$SD"/Music

# emulatori: un file di configurazione ciascuno (per aggiungerne uno basta un nuovo file e il core)
mkdir -p "$SD/Emulators"
cp emulators/*.json "$SD/Emulators/"

# lingue
mkdir -p "$SD/Lang"
cp lang/*.json "$SD/Lang/"

# filtri video e cornici (generati da tools/make_assets.py)
[ -d filters ] || python3 -I tools/make_assets.py .
mkdir -p "$SD/Filters" "$SD/Bezels"
cp -R filters/. "$SD/Filters/"
cp -R bezels/. "$SD/Bezels/"

# temi: copiati dal sorgente, i font condivisi restano in Themes/_shared
for t in themes/*/; do
    name=$(basename "$t")
    rm -rf "$SD/Themes/$name"
    cp -R "$t" "$SD/Themes/$name"
done

# core compilati per macOS
ln -sf "$DEPS/fbneo/src/burner/libretro/fbneo_libretro.dylib" "$SD/Cores/fbneo_libretro.dylib"
ln -sf "$DEPS/mame2003-plus/mame2003_plus_libretro.dylib" "$SD/Cores/mame2003p_libretro.dylib"
ln -sf "$DEPS/libretro-fceumm/fceumm_libretro.dylib" "$SD/Cores/fceumm_libretro.dylib"
ln -sf "$DEPS/Genesis-Plus-GX/genesis_plus_gx_libretro.dylib" "$SD/Cores/genesis_plus_gx_libretro.dylib"
ln -sf "$DEPS/snes9x/libretro/snes9x_libretro.dylib" "$SD/Cores/snes9x_libretro.dylib"
ln -sf "$DEPS/gambatte-libretro/gambatte_libretro.dylib" "$SD/Cores/gambatte_libretro.dylib"
ln -sf "$DEPS/mgba/build-mac/mgba_libretro.dylib" "$SD/Cores/mgba_libretro.dylib"
ln -sf "$DEPS/beetle-pce-fast-libretro/mednafen_pce_fast_libretro.dylib" "$SD/Cores/mednafen_pce_fast_libretro.dylib"
ln -sf "$DEPS/beetle-ngp-libretro/mednafen_ngp_libretro.dylib" "$SD/Cores/mednafen_ngp_libretro.dylib"

# indici dei titoli dai DAT ufficiali
python3 -I tools/build_index.py fbneo "$DEPS/fbneo/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat" "$SD/Dats/fbneo.tsv"
python3 -I tools/build_index.py mame2003p "$DEPS/mame2003-plus/metadata/mame2003-plus.xml" "$SD/Dats/mame2003p.tsv"

# cartelle ROM: collegamenti in sola lettura alle raccolte già presenti sul Mac
[ -n "$FBNEO_ROMS" ] && ln -sfn "$FBNEO_ROMS" "$SD/Roms/FBNEO"
[ -n "$MAME_ROMS" ] && ln -sfn "$MAME_ROMS" "$SD/Roms/MAME2003P"
[ -n "$NES_ROMS" ] && ln -sfn "$NES_ROMS" "$SD/Roms/NES"
[ -n "$MD_ROMS" ] && ln -sfn "$MD_ROMS" "$SD/Roms/MD"
[ -n "$SNES_ROMS" ] && ln -sfn "$SNES_ROMS" "$SD/Roms/SNES"
[ -n "$GB_ROMS" ] && ln -sfn "$GB_ROMS" "$SD/Roms/GB"
[ -n "$GBC_ROMS" ] && ln -sfn "$GBC_ROMS" "$SD/Roms/GBC"
[ -n "$GBA_ROMS" ] && ln -sfn "$GBA_ROMS" "$SD/Roms/GBA"
for d in FBNEO MAME2003P NES MD SMS GG PCE SNES GB GBC GBA NGP; do mkdir -p "$SD/Roms/$d" 2>/dev/null || true; done

# esempio di gioco Linux: una cartella con launch.sh (e facoltativi game.json, cover.png)
mkdir -p "$SD/Linux/esempio"
cat > "$SD/Linux/esempio/launch.sh" <<'EOF'
#!/bin/sh
# Qui va il comando che avvia il gioco Linux. Sul Mac è solo una prova.
echo "gioco Linux di esempio avviato: $(date) - ESEMPIO_SALUTO=$ESEMPIO_SALUTO"
sleep 1
EOF
cat > "$SD/Linux/esempio/game.json" <<'EOF'
{
  "title": "Gioco Linux di esempio",
  "year": "2026",
  "author": "Prototipo",
  "env": { "ESEMPIO_SALUTO": "ciao dal game.json" },
  "keys": { "A": "SPACE", "B": "ESC", "START": "ENTER", "UP": "UP", "DOWN": "DOWN", "LEFT": "LEFT", "RIGHT": "RIGHT" }
}
EOF
echo "SD simulata pronta in $SD"
