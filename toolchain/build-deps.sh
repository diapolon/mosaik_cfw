#!/bin/sh
# Librerie da compilare per la console, in versione statica dentro i nostri programmi.
# Il firmware ha SDL2 2.26 (con il driver GPU, quello lo usiamo) ma SDL2_ttf, SDL2_image e
# SDL2_mixer vecchie (2.0.x) e niente libcurl. Si gira dentro l'immagine trimui-tg5040:
#   docker run --rm -v trimui-build:/build -v "$PWD":/trimui trimui-tg5040 sh /trimui/toolchain/build-deps.sh
set -e
PREFIX=/build/prefix
SRC=/build/src
mkdir -p "$PREFIX" "$SRC"
export CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar RANLIB=aarch64-linux-gnu-ranlib STRIP=aarch64-linux-gnu-strip
export CFLAGS="-O2 -mcpu=cortex-a53 -fPIC"
# la glibc della console (2.23, nel kit) e quella del compilatore (2.25) non vanno mescolate:
# il linker deve risolvere libc/libpthread con quelle del compilatore
TCLIBC=/opt/aarch64-linux-gnu-7.5.0-linaro/aarch64-linux-gnu/libc
export LDFLAGS="-Wl,-rpath-link,$TCLIBC/lib -Wl,-rpath-link,$TCLIBC/usr/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
HOST=aarch64-linux-gnu
JOBS=$(nproc)

fetch() {   # url, cartella che l'archivio crea
    [ -d "$SRC/$2" ] && return
    echo "== scarico $1"
    curl -fsSL "$1" -o "$SRC/archive"
    tar xf "$SRC/archive" -C "$SRC"
    rm "$SRC/archive"
}

fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-2.22.0.tar.gz SDL2_ttf-2.22.0
fetch https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.2/SDL2_image-2.8.2.tar.gz SDL2_image-2.8.2
fetch https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.0/SDL2_mixer-2.8.0.tar.gz SDL2_mixer-2.8.0
fetch https://curl.se/download/curl-8.10.1.tar.gz curl-8.10.1

build() {   # cartella, opzioni di configure
    dir=$1; shift
    [ -f "$SRC/$dir/.fatto" ] && return
    echo "== compilo $dir"
    cd "$SRC/$dir"
    ./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static "$@" > configure.log 2>&1 \
        || { tail -30 configure.log; exit 1; }
    make -j"$JOBS" > make.log 2>&1 || { tail -40 make.log; exit 1; }
    make install > install.log 2>&1
    touch .fatto
}

# FreeType incluso (supporta i font variabili); niente HarfBuzz, non serve per testi latini
build SDL2_ttf-2.22.0 --enable-freetype-builtin --disable-harfbuzz
# PNG e JPG con stb_image, senza librerie esterne
build SDL2_image-2.8.2 --enable-stb-image --disable-avif --disable-jxl --disable-tif --disable-webp \
    --disable-qoi --disable-lbm --disable-pcx --disable-pnm --disable-svg --disable-xcf --disable-xpm --disable-xv
# MP3 (minimp3), OGG (stb_vorbis), FLAC (dr_flac), WAV: decoder inclusi
build SDL2_mixer-2.8.0 --enable-music-mp3 --enable-music-mp3-minimp3 --disable-music-mp3-mpg123 \
    --enable-music-ogg --enable-music-ogg-stb --disable-music-ogg-vorbis --disable-music-ogg-tremor \
    --enable-music-flac --enable-music-flac-drflac --disable-music-flac-libflac \
    --disable-music-mod --disable-music-midi --disable-music-opus --disable-music-wavpack
# libcurl con OpenSSL 1.1 del firmware (dinamica) per lo scraper
build curl-8.10.1 --with-openssl="$SDK" --with-zlib="$SDK" --disable-ldap --disable-ldaps --disable-rtsp \
    --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
    --disable-mqtt --disable-manual --without-libpsl --without-zstd --without-brotli --without-nghttp2 \
    --without-libidn2 --without-libssh2 --disable-docs

echo "== librerie pronte in $PREFIX"
ls "$PREFIX/lib"
