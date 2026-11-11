#!/bin/sh
# Core libretro per la console (aarch64, Cortex-A53), dai sorgenti in ../deps.
# Niente LDFLAGS da riga di comando: annullerebbero le opzioni dei Makefile dei core (-shared).
# Si compila in una copia pulita dentro il volume Docker: il Mac ha i suoi file oggetto
# nelle cartelle originali e il suo filesystem non distingue maiuscole e minuscole.
#   docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       trimui-tg5040 sh /trimui/toolchain/build-cores.sh [fbneo] [mame2003p] [fceumm] [genesis_plus_gx] [gambatte] [mgba] [snes9x] [pce_fast] [ngp]
set -e
SRC=/build/src
OUT=/build/out/cores
mkdir -p "$SRC" "$OUT"
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar
JOBS=$(nproc)
WHICH=${*:-fbneo mame2003p fceumm genesis_plus_gx gambatte mgba snes9x pce_fast ngp}

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
    fceumm)
        clone libretro-fceumm fceumm
        echo "== FCEUmm (NES)"
        make -C "$SRC/fceumm" -f Makefile.libretro -j"$JOBS" platform=unix \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/fceumm.log" 2>&1 \
            || { tail -30 "$SRC/fceumm.log"; exit 1; }
        cp "$SRC/fceumm/fceumm_libretro.so" "$OUT/"
        ;;
    genesis_plus_gx)
        clone Genesis-Plus-GX genesis_plus_gx
        echo "== Genesis Plus GX (Mega Drive)"
        make -C "$SRC/genesis_plus_gx" -f Makefile.libretro -j"$JOBS" platform=unix \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/genesis_plus_gx.log" 2>&1 \
            || { tail -30 "$SRC/genesis_plus_gx.log"; exit 1; }
        cp "$SRC/genesis_plus_gx/genesis_plus_gx_libretro.so" "$OUT/"
        ;;
    gambatte)
        clone gambatte-libretro gambatte
        echo "== Gambatte (Game Boy e Game Boy Color)"
        make -C "$SRC/gambatte" -f Makefile.libretro -j"$JOBS" platform=unix \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/gambatte.log" 2>&1 \
            || { tail -30 "$SRC/gambatte.log"; exit 1; }
        cp "$SRC/gambatte/gambatte_libretro.so" "$OUT/"
        ;;
    mgba)
        clone mgba mgba
        echo "== mGBA (Game Boy Advance)"
        # Solo la libreria e il core libretro, senza le dipendenze esterne: il cmake del
        # contenitore troverebbe le librerie del sistema amd64.
        F="-O2 -mcpu=cortex-a53 -fno-common"
        rm -rf "$SRC/mgba/build-a53"
        cmake -S "$SRC/mgba" -B "$SRC/mgba/build-a53" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
            -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
            -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" \
            -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON -DLIBRETRO_STATIC=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=OFF \
            -DUSE_ZLIB=OFF -DUSE_MINIZIP=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_SQLITE3=OFF \
            -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_FREETYPE=OFF -DUSE_LZMA=OFF \
            -DUSE_FFMPEG=OFF -DUSE_EDITLINE=OFF -DUSE_DISCORD_RPC=OFF -DUSE_EPOXY=OFF \
            > "$SRC/mgba.log" 2>&1 \
            && cmake --build "$SRC/mgba/build-a53" --target mgba_libretro -j"$JOBS" >> "$SRC/mgba.log" 2>&1 \
            || { tail -30 "$SRC/mgba.log"; exit 1; }
        cp "$SRC/mgba/build-a53/mgba_libretro.so" "$OUT/"
        ;;
    snes9x)
        clone snes9x snes9x
        echo "== Snes9x (Super Nintendo)"
        make -C "$SRC/snes9x/libretro" -j"$JOBS" platform=unix \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/snes9x.log" 2>&1 \
            || { tail -30 "$SRC/snes9x.log"; exit 1; }
        cp "$SRC/snes9x/libretro/snes9x_libretro.so" "$OUT/"
        ;;
    pce_fast)
        clone beetle-pce-fast-libretro pce_fast
        echo "== Beetle PCE Fast (PC Engine)"
        make -C "$SRC/pce_fast" -j"$JOBS" platform=unix \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/pce_fast.log" 2>&1 \
            || { tail -30 "$SRC/pce_fast.log"; exit 1; }
        cp "$SRC/pce_fast/mednafen_pce_fast_libretro.so" "$OUT/"
        ;;
    ngp)
        clone beetle-ngp-libretro ngp
        # flash come memoria di salvataggio, stati controllati (audit R05, issue #6)
        git -C "$SRC/ngp" apply /trimui/toolchain/patches/beetle-ngp-saveram.patch
        echo "== Beetle NeoPop (Neo Geo Pocket)"
        make -C "$SRC/ngp" -j"$JOBS" platform=unix \
            CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/ngp.log" 2>&1 \
            || { tail -30 "$SRC/ngp.log"; exit 1; }
        cp "$SRC/ngp/mednafen_ngp_libretro.so" "$OUT/"
        ;;
    esac
done
aarch64-linux-gnu-strip "$OUT"/*.so
for f in "$OUT"/*.so; do
    printf '%s: %s, glibc fino a %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)" \
        "$(aarch64-linux-gnu-objdump -T "$f" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)"
done
