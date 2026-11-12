#!/bin/sh
# PPSSPP (PSP) per la console: programma a sé con SDL2, come Deadcast per il Dreamcast, avviato da
# Cores/ppsspp/launch.sh. Il kit tg5040 ha GCC 7, troppo vecchio per PPSSPP 1.20 (C++17): si
# compila in Debian 11 arm64, che sul Mac gira nativo, con GCC 10 e glibc 2.31. Il firmware ha
# glibc 2.33 e la libstdc++ di GCC 10 (GLIBCXX 3.4.28), quindi il programma parte; a runtime usa
# la SDL2 e la GLES (PowerVR) della console. Niente X11, Wayland, Discord, UPnP, SDL_ttf.
#   docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       debian:bullseye sh /trimui/toolchain/build-ppsspp.sh
set -e
# Debian 11 è negli archivi: pacchetti da archive.debian.org (firme vecchie accettate per data)
cat > /etc/apt/sources.list <<'L'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian-security bullseye-security main
L
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archivio
apt-get -qq update > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config python3 \
    libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev zlib1g-dev > /dev/null
SRC=/build/src/ppsspp
B=/build/src/ppsspp-build
OUT=/build/out/ppsspp
want=$(git -C /trimui/deps/ppsspp rev-parse HEAD)
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$want" ]; then
    rm -rf "$SRC" "$B"
    cp -a /trimui/deps/ppsspp "$SRC"
fi
mkdir -p "$B" "$OUT"
F="-O2 -mcpu=cortex-a53"
cmake -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" \
    -DUSING_GLES2=ON -DUSING_EGL=OFF -DUSING_FBDEV=OFF \
    -DUSING_X11_VULKAN=OFF -DUSE_WAYLAND_WSI=OFF -DUSE_VULKAN_DISPLAY_KHR=OFF \
    -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_FFMPEG=OFF \
    -DUSE_CCACHE=OFF -DHEADLESS=OFF -DUNITTEST=OFF -DUSING_QT_UI=OFF \
    > "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
echo "== PPSSPP (ci vuole un po')"
cmake --build "$B" --target PPSSPPSDL -j"$(nproc)" >> "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
strip -o "$OUT/PPSSPPSDL" "$B/PPSSPPSDL"
rm -rf "$OUT/assets"
cp -a "$B/assets" "$OUT/assets" 2>/dev/null || cp -a "$SRC/assets" "$OUT/assets"
printf 'PPSSPPSDL: %s, glibc fino a %s, GLIBCXX fino a %s\n' "$(du -h "$OUT/PPSSPPSDL" | cut -f1)" \
    "$(objdump -T "$OUT/PPSSPPSDL" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)" \
    "$(objdump -T "$OUT/PPSSPPSDL" | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sort -Vu | tail -1)"
echo "librerie: $(objdump -p "$OUT/PPSSPPSDL" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
