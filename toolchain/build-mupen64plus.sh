#!/bin/sh
# Nintendo 64 per la console: mupen64plus, programma a sé con SDL2 e GLES2 come PPSSPP e Deadcast
# (il runner non dà la GPU ai core libretro), avviato da Cores/mupen64plus/launch.sh.
# Si compila in Debian 11 arm64 (GCC 10, glibc 2.31; il firmware ha la 2.33). A runtime usa la SDL2,
# la GLES (PowerVR) e la zlib della console; la libpng si collega dentro, perché il firmware ha
# solo la 1.2. Sorgenti: deps/mupen64plus/mupen64plus-{core,ui-console,audio-sdl,input-sdl,rsp-hle,
# video-rice,video-glide64mk2}, cloni di github.com/mupen64plus.
#   docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       debian:bullseye sh /trimui/toolchain/build-mupen64plus.sh
set -e
cat > /etc/apt/sources.list <<'L'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian-security bullseye-security main
L
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archivio
apt-get -qq update > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    build-essential git ca-certificates pkg-config patch \
    libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev zlib1g-dev libpng-dev libboost-dev > /dev/null
SRC=/build/src/mupen64plus
OUT=/build/out/mupen64plus
rm -rf "$SRC"
mkdir -p "$SRC" "$OUT"
cp -a /trimui/deps/mupen64plus/. "$SRC/"
# modifica di Mosaik: con SIGTERM (uscita dal menu di Mosaik, spegnimento) l'emulazione si ferma
# e il core scrive i salvataggi della cartuccia, invece di morire sul colpo
patch -s -p1 -d "$SRC/mupen64plus-ui-console" < /trimui/toolchain/patches/mupen64plus-ui-console-mosaik.patch
LOG="$OUT/build.log"
: > "$LOG"
PNG_A=$(ls /usr/lib/aarch64-linux-gnu/libpng16.a)
# anche la zlib dentro: la libpng 1.6 chiede inflateValidate (zlib 1.2.9), il firmware ha la 1.2.8
Z_A=$(ls /usr/lib/aarch64-linux-gnu/libz.a)
COMMON="USE_GLES=1 VULKAN=0 OSD=0 NETPLAY=0 OPENCV=0 V=1 PIE=1 OPTFLAGS=-O2\ -mcpu=cortex-a53 APIDIR=$SRC/mupen64plus-core/src/api"
build() {   # modulo, variabili in più
    m=$1; shift
    echo "== $m"
    # shellcheck disable=SC2086
    eval make -C "$SRC/mupen64plus-$m/projects/unix" -j"$(nproc)" all $COMMON "$@" >> "$LOG" 2>&1 || { tail -40 "$LOG"; exit 1; }
}
build core NEW_DYNAREC=1 LIBPNG_CFLAGS=-I/usr/include/libpng16 "LIBPNG_LDLIBS='$PNG_A $Z_A -lm'" ZLIB_CFLAGS= "ZLIB_LDLIBS=$Z_A"
build ui-console
build audio-sdl NO_SRC=1 NO_SPEEX=1 NO_OSS=1
build input-sdl
build rsp-hle
build video-rice LIBPNG_CFLAGS=-I/usr/include/libpng16 "LIBPNG_LDLIBS='$PNG_A $Z_A -lm'" ZLIB_CFLAGS= "ZLIB_LDLIBS=$Z_A"
# Glide64mk2: più fedele ma più pesante; se non si compila resta Rice
( build video-glide64mk2 LIBPNG_CFLAGS=-I/usr/include/libpng16 "LIBPNG_LDLIBS='$PNG_A $Z_A -lm'" ZLIB_CFLAGS= "ZLIB_LDLIBS=$Z_A" ) || echo "glide64mk2 non compilato: resta Rice"

rm -rf "$OUT/lib" "$OUT/share"
mkdir -p "$OUT/lib" "$OUT/share"
strip -o "$OUT/mupen64plus" "$SRC/mupen64plus-ui-console/projects/unix/mupen64plus"
for f in core/projects/unix/libmupen64plus.so.2.0.0 audio-sdl/projects/unix/mupen64plus-audio-sdl.so \
         input-sdl/projects/unix/mupen64plus-input-sdl.so rsp-hle/projects/unix/mupen64plus-rsp-hle.so \
         video-rice/projects/unix/mupen64plus-video-rice.so video-glide64mk2/projects/unix/mupen64plus-video-glide64mk2.so; do
    [ -f "$SRC/mupen64plus-$f" ] || continue
    n=$(basename "$f"); [ "$n" = libmupen64plus.so.2.0.0 ] && n=libmupen64plus.so.2
    strip -o "$OUT/lib/$n" "$SRC/mupen64plus-$f"
done
# dati: catalogo dei giochi, caratteri, configurazione automatica dei controller, impostazioni di Rice
cp "$SRC"/mupen64plus-core/data/* "$OUT/share/"
cp "$SRC"/mupen64plus-input-sdl/data/InputAutoCfg.ini "$OUT/share/"
cp "$SRC"/mupen64plus-video-rice/data/RiceVideoLinux.ini "$OUT/share/"
cp "$SRC"/mupen64plus-video-glide64mk2/data/Glide64mk2.ini "$OUT/share/" 2>/dev/null || true
ls -la "$OUT" "$OUT/lib"
for f in "$OUT/mupen64plus" "$OUT"/lib/*; do
    printf '%s: glibc fino a %s, GLIBCXX fino a %s, librerie: %s\n' "$(basename "$f")" \
        "$(objdump -T "$f" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)" \
        "$(objdump -T "$f" | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sort -Vu | tail -1)" \
        "$(objdump -p "$f" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
done
