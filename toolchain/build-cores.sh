#!/bin/sh
# Core libretro per la console (aarch64, Cortex-A53), dai sorgenti in ../deps.
# Niente LDFLAGS da riga di comando: annullerebbero le opzioni dei Makefile dei core (-shared).
# Si compila in una copia pulita dentro il volume Docker: il Mac ha i suoi file oggetto
# nelle cartelle originali e il suo filesystem non distingue maiuscole e minuscole.
#   docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       trimui-tg5040 sh /trimui/toolchain/build-cores.sh [fbneo] [mame2003p]
set -e
SRC=/build/src
OUT=/build/out/cores
mkdir -p "$SRC" "$OUT"
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar
JOBS=$(nproc)
WHICH=${*:-fbneo mame2003p}

clone() {   # cartella in deps, destinazione
    if [ ! -d "$SRC/$2/.git" ]; then
        git clone -q --no-hardlinks "/trimui/deps/$1" "$SRC/$2"
    else
        git -C "$SRC/$2" fetch -q "/trimui/deps/$1" && git -C "$SRC/$2" reset -q --hard FETCH_HEAD
    fi
}

for core in $WHICH; do
    case $core in
    fbneo)
        clone fbneo fbneo
        echo "== FinalBurn Neo (ci vuole un po')"
        # rpi3_64 = profilo generico armv8-a+crc+simd ottimizzato per Cortex-A53, come l'A133P
        make -C "$SRC/fbneo/src/burner/libretro" -j"$JOBS" platform=rpi3_64 \
            CC="$CC" CXX="$CXX" > "$SRC/fbneo.log" 2>&1 \
            || { tail -30 "$SRC/fbneo.log"; exit 1; }
        cp "$SRC/fbneo/src/burner/libretro/fbneo_libretro.so" "$OUT/"
        ;;
    mame2003p)
        clone mame2003-plus mame2003-plus
        echo "== mame2003-plus"
        make -C "$SRC/mame2003-plus" -j"$JOBS" platform=unix ARM=1 CPU_ARCH=arm64 \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" LD="$CC -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/mame2003p.log" 2>&1 \
            || { tail -30 "$SRC/mame2003p.log"; exit 1; }
        cp "$SRC/mame2003-plus/mame2003_plus_libretro.so" "$OUT/"
        ;;
    esac
done
aarch64-linux-gnu-strip "$OUT"/*.so
for f in "$OUT"/*.so; do
    printf '%s: %s, glibc fino a %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)" \
        "$(aarch64-linux-gnu-objdump -T "$f" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)"
done
