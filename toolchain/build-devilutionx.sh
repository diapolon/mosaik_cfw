#!/bin/sh
# DevilutionX (Diablo) per la cartella Giochi Linux dello Store. Come PPSSPP: C++20, quindi Debian 11
# arm64 con GCC 10 e glibc 2.31 (il firmware ha glibc 2.33 e la libstdc++ di GCC 10). Sorgenti
# "fully vendored" della release (deps/devilutionx), niente rete: tutte le librerie dentro il
# programma tranne SDL2, zlib e la libreria C, che sulla console ci sono. La build ufficiale per
# aarch64 chiede glibc 2.34, libsodium e SDL2_image: non parte sulla console.
#   docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       debian:bullseye sh /trimui/toolchain/build-devilutionx.sh
set -e
cat > /etc/apt/sources.list <<'L'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian-security bullseye-security main
L
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archivio
apt-get -qq update > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    build-essential cmake xz-utils ca-certificates pkg-config libsdl2-dev zlib1g-dev gettext > /dev/null
VER=1.5.5
TAR=/trimui/deps/devilutionx/devilutionx-src-fully-vendored-$VER.tar.xz
SRC=/build/src/devilutionx-$VER
B=/build/src/devilutionx-build
OUT=/build/out/linux/devilutionx
if [ ! -f "$SRC/CMakeLists.txt" ]; then
    rm -rf "$SRC" "$B"
    mkdir -p /build/src
    tar -xJf "$TAR" -C /build/src
    mv "/build/src/devilutionx-src-full-$VER" "$SRC"
fi
mkdir -p "$B" "$OUT"
F="-O2 -mcpu=cortex-a53"
cmake -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" \
    -DNONET=ON -DBUILD_TESTING=OFF -DDISCORD_INTEGRATION=OFF \
    -DDEVILUTIONX_SYSTEM_SDL2=ON -DDEVILUTIONX_SYSTEM_ZLIB=ON \
    -DDEVILUTIONX_SYSTEM_BZIP2=OFF -DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF -DDEVILUTIONX_SYSTEM_LIBPNG=OFF \
    -DDEVILUTIONX_SYSTEM_LIBFMT=OFF -DDEVILUTIONX_SYSTEM_SDL_AUDIOLIB=OFF -DDEVILUTIONX_SYSTEM_SIMPLEINI=OFF \
    > "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
echo "== DevilutionX (qualche minuto)"
cmake --build "$B" -j"$(nproc)" >> "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
strip -o "$OUT/devilutionx" "$B/devilutionx"
cp "$B/devilutionx.mpq" "$OUT/" 2>/dev/null || cp "$SRC/dist/devilutionx.mpq" "$OUT/"
cp "$SRC/LICENSE.md" "$OUT/LICENSE-devilutionx.md"
printf 'devilutionx: %s, glibc fino a %s, GLIBCXX fino a %s\n' "$(du -h "$OUT/devilutionx" | cut -f1)" \
    "$(objdump -T "$OUT/devilutionx" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)" \
    "$(objdump -T "$OUT/devilutionx" | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sort -Vu | tail -1)"
echo "librerie: $(objdump -p "$OUT/devilutionx" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
