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

# url, cartella che l'archivio crea, SHA-256: un archivio diverso da quello provato non si estrae
# (audit 0.9.88, DEV-05; prova in test-files)
fetch() {
    [ -d "$SRC/$2" ] && return
    echo "== scarico $1"
    curl -fsSL "$1" -o "$SRC/archive"
    if ! echo "$3  $SRC/archive" | sha256sum -c - > /dev/null 2>&1; then
        echo "$1: SHA-256 diverso da quello atteso ($3): mi fermo"
        rm -f "$SRC/archive"
        exit 1
    fi
    tar xf "$SRC/archive" -C "$SRC"
    rm "$SRC/archive"
}

fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-2.22.0.tar.gz SDL2_ttf-2.22.0 d48cbd1ce475b9e178206bf3b72d56b66d84d44f64ac05803328396234d67723
fetch https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.2/SDL2_image-2.8.2.tar.gz SDL2_image-2.8.2 8f486bbfbcf8464dd58c9e5d93394ab0255ce68b51c5a966a918244820a76ddc
fetch https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.0/SDL2_mixer-2.8.0.tar.gz SDL2_mixer-2.8.0 1cfb34c87b26dbdbc7afd68c4f545c0116ab5f90bbfecc5aebe2a9cb4bb31549
fetch https://curl.se/download/curl-8.10.1.tar.gz curl-8.10.1 d15ebab765d793e2e96db090f0e172d127859d78ca6f6391d7eafecfd894bbc0

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

# Le stesse tre SDL anche condivise, per i giochi Linux compilati da altri che le chiedono
# (libSDL2_image-2.0.so.0 e le altre: stesso nome di libreria delle 2.0.x del firmware, che nel
# nostro sistema non ci sono). Stesse opzioni, decoder inclusi: nessun'altra libreria. Vanno in
# /usr/lib del sistema (m2/build-in-container.sh), che è nel LD_LIBRARY_PATH dei giochi.
SHARED=/build/out/syslibs
build_shared() {   # cartella, opzioni di configure
    dir=$1; shift
    [ -f "$SRC/$dir-shared/.fatto" ] && return
    echo "== compilo $dir condivisa"
    rm -rf "$SRC/$dir-shared"
    cp -a "$SRC/$dir" "$SRC/$dir-shared"
    cd "$SRC/$dir-shared"
    make distclean > /dev/null 2>&1 || true
    ./configure --host=$HOST --prefix=/build/prefix-shared --enable-shared --disable-static "$@" > configure.log 2>&1 \
        || { tail -30 configure.log; exit 1; }
    make -j"$JOBS" > make.log 2>&1 || { tail -40 make.log; exit 1; }
    make install > install.log 2>&1
    touch .fatto
}
build_shared SDL2_ttf-2.22.0 --enable-freetype-builtin --disable-harfbuzz
build_shared SDL2_image-2.8.2 --enable-stb-image --disable-avif --disable-jxl --disable-tif --disable-webp \
    --disable-qoi --disable-lbm --disable-pcx --disable-pnm --disable-svg --disable-xcf --disable-xpm --disable-xv
build_shared SDL2_mixer-2.8.0 --enable-music-mp3 --enable-music-mp3-minimp3 --disable-music-mp3-mpg123 \
    --enable-music-ogg --enable-music-ogg-stb --disable-music-ogg-vorbis --disable-music-ogg-tremor \
    --enable-music-flac --enable-music-flac-drflac --disable-music-flac-libflac \
    --disable-music-mod --disable-music-midi --disable-music-opus --disable-music-wavpack
rm -rf "$SHARED" && mkdir -p "$SHARED"
for l in libSDL2_image-2.0.so.0 libSDL2_mixer-2.0.so.0 libSDL2_ttf-2.0.so.0; do
    cp -L /build/prefix-shared/lib/$l "$SHARED/$l"
    $STRIP --strip-unneeded "$SHARED/$l"
done
ls -la "$SHARED"

echo "== librerie pronte in $PREFIX"
ls "$PREFIX/lib"
