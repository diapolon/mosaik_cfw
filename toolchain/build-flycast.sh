#!/bin/sh
# Flycast (Dreamcast) per la console: il programma a sé con SDL2, non il core libretro, avviato da
# Cores/flycast/launch.sh come PPSSPP. Deadcast disegna con Vulkan direttamente in fb0 e sulla
# Smart Pro, con lo schermo ruotato, resta nero; Flycast passa invece dalla SDL2 della console,
# che lo schermo ruotato lo gestisce già (come il nostro launcher e PPSSPP).
# Come per PPSSPP si compila in Debian 11 arm64 (nativo sul Mac) con GCC 10 e glibc 2.31: il
# firmware ha glibc 2.33 e la libstdc++ di GCC 10 (GLIBCXX 3.4.28).
#   docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       debian:bullseye sh /trimui/toolchain/build-flycast.sh
# - SDL2: Flycast vuole almeno la 2.0.18 e Debian 11 ha la 2.0.14. Si compila la SDL inclusa nei
#   sorgenti (sottomodulo core/deps/SDL), condivisa (SDL_SHARED: altrimenti finisce dentro il
#   programma, senza il driver video della console), solo per collegare il programma a
#   libSDL2-2.0.so.0: sulla console si usa quella del firmware (2.30.8). Alla fine si controlla
#   che ogni simbolo chiesto esista nelle librerie del firmware (/build/fw/tg5040).
# - GLES e Vulkan: Flycast carica le funzioni da SDL (glad, SDL_Vulkan_*), non si collega a
#   libGLESv2, libEGL né libvulkan.
# - libcurl: il CMake di Flycast la vuole sempre su Linux; quella del firmware (7.54.1) non ha
#   curl_mime_* (7.56). Si usa una libcurl statica solo HTTP, senza TLS (serve solo a
#   RetroAchievements e alle copertine, che chiedono HTTPS: con questa build non vanno).
# - Niente X11, Wayland, OpenGL desktop, libretro, Lua, OpenMP, ALSA, PulseAudio, libao,
#   breakpad, Discord; libusb (DreamPicoPort) e miniupnpc restano, statiche e senza udev.
set -e
# Debian 11 è negli archivi: pacchetti da archive.debian.org (firme vecchie accettate per data).
# Flycast chiede CMake 3.22: quello di bullseye-backports (3.25).
cat > /etc/apt/sources.list <<'L'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian-security bullseye-security main
deb http://archive.debian.org/debian bullseye-backports main
L
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archivio
apt-get -qq update > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    build-essential git ca-certificates pkg-config python3 zlib1g-dev curl binutils > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends -t bullseye-backports \
    cmake > /dev/null

SRC=/build/src/flycast
B=/build/src/flycast-build
OUT=/build/out/flycast
FW=/build/fw/tg5040/root
CURLSRC=/build/src/curl-8.10.1-flycast
CURLDIR=/build/src/flycast-curl
JOBS=$(nproc)
F="-O2 -mcpu=cortex-a53"   # senza il -O3 che CMake mette in Release
mkdir -p "$OUT"
: > "$OUT/build.log"

# sorgenti copiati (con i sottomoduli): la copia in deps/ non si tocca
git config --global --add safe.directory '*'
want=$(git -C /trimui/deps/flycast rev-parse HEAD)
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$want" ]; then
    rm -rf "$SRC" "$B"
    cp -a /trimui/deps/flycast "$SRC"
fi
[ -f "$SRC/core/deps/SDL/CMakeLists.txt" ] || { echo "manca il sottomodulo core/deps/SDL"; exit 1; }

# libcurl statica, solo HTTP, dagli stessi sorgenti di build-deps.sh (scaricati se mancano)
if [ ! -f "$CURLDIR/lib/libcurl.a" ]; then
    echo "== libcurl statica"
    rm -rf "$CURLSRC"
    if [ -d /build/src/curl-8.10.1 ]; then
        cp -a /build/src/curl-8.10.1 "$CURLSRC"
        (cd "$CURLSRC" && make distclean > /dev/null 2>&1 || true)
    else
        mkdir -p "$CURLSRC"
        curl -fsSL https://curl.se/download/curl-8.10.1.tar.gz | tar xz -C "$CURLSRC" --strip-components=1
    fi
    cd "$CURLSRC"
    CFLAGS="$F -fPIC" ./configure --prefix="$CURLDIR" --disable-shared --enable-static \
        --without-ssl --with-zlib --disable-ftp --disable-file --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
        --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-ipfs --disable-manual \
        --disable-docs --without-libpsl --without-zstd --without-brotli --without-nghttp2 \
        --without-libidn2 --without-libssh2 >> "$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; exit 1; }
    make -j"$JOBS" >> "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
    make install >> "$OUT/build.log" 2>&1
    cd /
fi

mkdir -p "$B"
cmake -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_SKIP_RPATH=ON \
    -DCMAKE_C_FLAGS_RELEASE=-DNDEBUG -DCMAKE_CXX_FLAGS_RELEASE=-DNDEBUG \
    -DUSE_GLES=ON -DUSE_GLES2=OFF -DUSE_OPENGL=ON -DUSE_VULKAN=ON \
    -DLIBRETRO=OFF -DUSE_HOST_SDL=OFF -DUSE_HOST_LIBZIP=OFF -DUSE_HOST_LIBCHDR=OFF -DUSE_HOST_GLSLANG=OFF \
    -DUSE_OPENMP=OFF -DUSE_LUA=OFF -DUSE_BREAKPAD=OFF -DUSE_DISCORD=OFF -DUSE_LIBCDIO=OFF \
    -DUSE_ALSA=OFF -DUSE_PULSEAUDIO=OFF -DUSE_LIBAO=OFF -DUSE_OSS=OFF \
    -DENABLE_CTEST=OFF -DENABLE_GDB_SERVER=OFF \
    -DLIBUSB_ENABLE_UDEV=OFF -DLIBUSB_BUILD_SHARED_LIBS=OFF \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_VULKAN=ON \
    -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_TEST=OFF \
    -DCURL_INCLUDE_DIR="$CURLDIR/include" -DCURL_LIBRARY="$CURLDIR/lib/libcurl.a" \
    >> "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
echo "== Flycast (ci vuole un po')"
cmake --build "$B" --target flycast -j"$JOBS" >> "$OUT/build.log" 2>&1 || { tail -60 "$OUT/build.log"; exit 1; }
strip -o "$OUT/flycast" "$B/flycast"

# controlli: glibc, GLIBCXX, librerie chieste, simboli che le librerie del firmware non hanno
printf 'flycast: %s, glibc fino a %s, GLIBCXX fino a %s\n' "$(du -h "$OUT/flycast" | cut -f1)" \
    "$(objdump -T "$OUT/flycast" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)" \
    "$(objdump -T "$OUT/flycast" | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sort -Vu | tail -1)" | tee "$OUT/controllo.txt"
need=$(objdump -p "$OUT/flycast" | awk '/NEEDED/ {print $2}')
echo "librerie: $(echo $need | tr '\n' ' ')" | tee -a "$OUT/controllo.txt"
echo "$need" | grep -qx 'libSDL2-2.0.so.0' || { echo "ERRORE: SDL2 dentro il programma, non quella della console"; exit 1; }
if [ -d "$FW" ]; then
    : > /tmp/fw-simboli
    for l in $need; do
        p=$(ls "$FW/lib/$l" "$FW/usr/lib/$l" "$FW/usr/trimui/lib/$l" 2>/dev/null | head -1)
        if [ -z "$p" ]; then echo "  manca nel firmware: $l" | tee -a "$OUT/controllo.txt"; continue; fi
        nm -D --defined-only "$p" | awk '{print $NF}' | sed 's/@.*//' >> /tmp/fw-simboli
    done
    sort -u /tmp/fw-simboli -o /tmp/fw-simboli
    nm -D --undefined-only "$OUT/flycast" | awk '$1 == "U" {print $2}' | sed 's/@.*//' | sort -u > /tmp/fc-simboli
    mancano=$(comm -23 /tmp/fc-simboli /tmp/fw-simboli)
    if [ -n "$mancano" ]; then
        echo "simboli che il firmware non ha:" | tee -a "$OUT/controllo.txt"
        echo "$mancano" | sed 's/^/  /' | tee -a "$OUT/controllo.txt"
    else
        echo "simboli: tutti presenti nelle librerie del firmware" | tee -a "$OUT/controllo.txt"
    fi
fi
