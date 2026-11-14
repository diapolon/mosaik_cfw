#!/bin/sh
# FFmpeg per l'app Video dello Store (frontend/src/apps/video.c) e per la prova di velocità
# toolchain/vtest. Solo parti LGPL (niente --enable-gpl), librerie condivise da mettere accanto al
# programma in lib/, e solo quello che serve: contenitori mp4/mov, mkv/webm, avi; video H.264,
# HEVC, MPEG-4 (anche DivX 3), MPEG-1/2, VP8, VP9; audio AAC, MP3, MP2, AC-3, E-AC-3, DTS, FLAC,
# Opus, Vorbis, PCM; swscale e swresample. Debian 11 arm64 (glibc 2.31, il firmware ha la 2.33),
# come DevilutionX. Poi compila la prova toolchain/vtest/vtest.c contro queste librerie e la SDL2
# di Debian (stesso soname di quella del firmware, usata sulla console).
# Il sorgente (deps/ffmpeg/ffmpeg-VER.tar.xz) va in dist/release/sources a ogni release che porta
# l'app (LGPL).
#   docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       debian:bullseye sh /trimui/toolchain/build-ffmpeg.sh
# Risultato: /build/out/video/lib/*.so.* (le librerie dell'app, che Makefile.tg5040 collega e
# toolchain/package.sh mette in Apps/video/lib) e /build/out/vtest (prova con le stesse librerie).
# Si ricompila FFmpeg solo se cambiano versione o opzioni (/build/out/ffmpeg/opzioni.txt).
set -e
cat > /etc/apt/sources.list <<'L'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian-security bullseye-security main
L
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archivio
apt-get -qq update > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    build-essential xz-utils pkg-config libsdl2-dev libsdl2-ttf-dev > /dev/null
VER=9.0.2
TAR=/trimui/deps/ffmpeg/ffmpeg-$VER.tar.xz
SRC=/build/src/ffmpeg-$VER
PREFIX=/build/out/ffmpeg
OUT=/build/out/vtest
LIBOUT=/build/out/video/lib
OPTS="--enable-shared --disable-static --enable-pic \
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-network \
    --disable-autodetect --enable-pthreads --disable-everything \
    --enable-protocol=file \
    --enable-demuxer=mov,matroska,avi \
    --enable-decoder=h264,hevc,mpeg4,msmpeg4v3,mpeg1video,mpeg2video,vp8,vp9 \
    --enable-decoder=aac,mp3,mp3float,mp2,ac3,eac3,dca,flac,opus,vorbis,pcm_s16le,pcm_s24le,pcm_u8 \
    --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,aac,mpegaudio,ac3,dca,flac,opus,vorbis \
    --enable-swscale --enable-swresample"
WANT="$VER $OPTS"
if [ ! -f "$PREFIX/lib/libavcodec.so" ] || [ "$(cat "$PREFIX/opzioni.txt" 2>/dev/null)" != "$WANT" ]; then
    rm -rf "$PREFIX"
    rm -rf "$SRC"
    mkdir -p /build/src
    tar -xJf "$TAR" -C /build/src
    cd "$SRC"
    echo "== FFmpeg $VER (qualche minuto)"
    # shellcheck disable=SC2086
    ./configure --prefix="$PREFIX" $OPTS --extra-cflags="-O2 -mcpu=cortex-a53" > /build/src/ffmpeg-configure.log 2>&1 \
        || { tail -30 /build/src/ffmpeg-configure.log; exit 1; }
    grep -q '^License: LGPL' /build/src/ffmpeg-configure.log 2>/dev/null || grep -i '^license' /build/src/ffmpeg-configure.log
    make -j"$(nproc)" > /build/src/ffmpeg-build.log 2>&1 || { tail -30 /build/src/ffmpeg-build.log; exit 1; }
    make install > /dev/null
    printf '%s' "$WANT" > "$PREFIX/opzioni.txt"
fi
rm -rf "$OUT" "$LIBOUT"
mkdir -p "$OUT/lib" "$LIBOUT"
for l in avformat avcodec avutil swscale swresample; do
    f=$(ls "$PREFIX"/lib/lib$l.so.* | head -1)
    so=$(objdump -p "$f" | awk '/SONAME/ {print $2}')
    cp -L "$f" "$LIBOUT/$so"
    strip "$LIBOUT/$so"
    cp "$LIBOUT/$so" "$OUT/lib/$so"
done
cp "$SRC/COPYING.LGPLv2.1" "$LIBOUT/../LICENSE-ffmpeg.txt" 2>/dev/null || cp "$PREFIX/share/ffmpeg/COPYING.LGPLv2.1" "$LIBOUT/../LICENSE-ffmpeg.txt" 2>/dev/null || true
echo "== vtest"
gcc -O2 -mcpu=cortex-a53 -Wall -Wextra -o "$OUT/vtest" /trimui/toolchain/vtest/vtest.c \
    -I"$PREFIX/include" -L"$PREFIX/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil \
    $(pkg-config --cflags --libs sdl2) -lSDL2_ttf -lm -Wl,-rpath,'$ORIGIN/lib'
strip "$OUT/vtest"
cp "$LIBOUT/../LICENSE-ffmpeg.txt" "$OUT/" 2>/dev/null || true
for f in "$OUT/vtest" "$LIBOUT"/*; do
    printf '%-32s %6s  glibc fino a %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)" \
        "$(objdump -T "$f" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)"
done
echo "librerie di vtest: $(objdump -p "$OUT/vtest" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
