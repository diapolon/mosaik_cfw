#!/bin/sh
# Giochi della cartella Giochi Linux dello Store, compilati con il kit della console come il
# launcher: SDL2 del firmware (dinamica), SDL2_mixer statica di /build/prefix (build-deps.sh),
# così il gioco non porta librerie sue. Uscite in /build/out/linux/<gioco>.
#   docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       trimui-tg5040 sh /trimui/toolchain/build-linux-games.sh
set -e
SDK=/opt/sdk/usr
PREFIX=/build/prefix
TCLIBC=/opt/aarch64-linux-gnu-7.5.0-linaro/aarch64-linux-gnu/libc
OUT=/build/out/linux
mkdir -p "$OUT"

# ---------------------------------------------------------------- Crispy Doom
# Doom con i miglioramenti di Crispy (alta risoluzione, widescreen); musica con l'OPL emulato, senza
# rete (SDL2_net non c'è) né FluidSynth. I dati del gioco sono Freedoom (BSD) o i WAD dell'utente.
SRC=/build/src/crispy-doom
B=/build/src/crispy-doom-build
want=$(git -C /trimui/deps/crispy-doom rev-parse HEAD)
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$want" ]; then
    rm -rf "$SRC" "$B"
    cp -a /trimui/deps/crispy-doom "$SRC"
fi
mkdir -p "$B" "$OUT/crispy-doom"
cat > "$B/toolchain.cmake" <<T
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_FIND_ROOT_PATH $SDK $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
T
# l'SDK ha una SDL_mixer.h vecchia nella stessa cartella di SDL.h: intestazioni unite, con quella
# di SDL2_mixer 2.8 sopra
mkdir -p "$B/inc"
cp "$SDK"/include/SDL2/*.h "$B/inc/"
cp "$PREFIX"/include/SDL2/SDL_mixer.h "$B/inc/"
# SDL2_mixer statica: i suoi codec (stb_vorbis, dr_mp3...) sono dentro; SDL2 dinamica del firmware
cmake -S "$SRC" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$B/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-O2 -mcpu=cortex-a53" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$TCLIBC/lib -Wl,-rpath-link,$TCLIBC/usr/lib -Wl,-rpath-link,$SDK/lib" \
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=OFF -DENABLE_SDL2_NET=OFF -DENABLE_SDL2_MIXER=ON \
    -DSDL2_INCLUDE_DIR="$B/inc" -DSDL2_LIBRARY="$SDK/lib/libSDL2.so" -DSDL2_MAIN_LIBRARY="$SDK/lib/libSDL2.so" \
    -DSDL2_MIXER_INCLUDE_DIR="$B/inc" -DSDL2_MIXER_LIBRARY="$PREFIX/lib/libSDL2_mixer.a" \
    -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON -DCMAKE_DISABLE_FIND_PACKAGE_SampleRate=ON -DCMAKE_DISABLE_FIND_PACKAGE_FluidSynth=ON \
    > "$OUT/crispy-doom/build.log" 2>&1 || { tail -40 "$OUT/crispy-doom/build.log"; exit 1; }
echo "== Crispy Doom"
cmake --build "$B" --target crispy-doom -j"$(nproc)" >> "$OUT/crispy-doom/build.log" 2>&1 || { tail -40 "$OUT/crispy-doom/build.log"; exit 1; }
aarch64-linux-gnu-strip -o "$OUT/crispy-doom/crispy-doom" "$B/src/crispy-doom"
printf 'crispy-doom: %s, glibc fino a %s\n' "$(du -h "$OUT/crispy-doom/crispy-doom" | cut -f1)" \
    "$(aarch64-linux-gnu-objdump -T "$OUT/crispy-doom/crispy-doom" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)"
echo "librerie: $(aarch64-linux-gnu-objdump -p "$OUT/crispy-doom/crispy-doom" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
