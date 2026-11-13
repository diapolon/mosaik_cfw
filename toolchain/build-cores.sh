#!/bin/sh
# Core libretro per la console (aarch64, Cortex-A53), dai sorgenti in ../deps.
# Niente LDFLAGS da riga di comando: annullerebbero le opzioni dei Makefile dei core (-shared).
# Si compila in una copia pulita dentro il volume Docker: il Mac ha i suoi file oggetto
# nelle cartelle originali e il suo filesystem non distingue maiuscole e minuscole.
#   docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       trimui-tg5040 sh /trimui/toolchain/build-cores.sh [fbneo] [mame2003p] [mame2010] [fceumm] [genesis_plus_gx] [gambatte] [mgba] [snes9x] [pce_fast] [ngp] [pcsx]
#       [stella2014] [prosystem] [handy] [wswan] [vb] [gearcoleco] [pokemini] [freeintv] [scummvm] [fuse] [bluemsx] [vice]
set -e
SRC=/build/src
OUT=/build/out/cores
mkdir -p "$SRC" "$OUT"
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar
JOBS=$(nproc)
WHICH=${*:-fbneo mame2003p mame2010 fceumm genesis_plus_gx gambatte mgba snes9x pce_fast ngp pcsx stella2014 prosystem handy wswan vb gearcoleco pokemini freeintv scummvm fuse bluemsx vice}

clone() {   # cartella in deps, destinazione
    if [ ! -d "$SRC/$2/.git" ]; then
        git clone -q --no-hardlinks "/trimui/deps/$1" "$SRC/$2"
    else
        git -C "$SRC/$2" fetch -q "/trimui/deps/$1" && git -C "$SRC/$2" reset -q --hard FETCH_HEAD
    fi
}

# core semplici: make con platform=unix nella cartella data, il .so che ne esce
simple() {   # NOME_LOG CARTELLA_IN_DEPS SOTTOCARTELLA MAKEFILE FILE_SO
    clone "$2" "$1"
    make -C "$SRC/$1/$3" -f "$4" -j"$JOBS" platform=unix \
        CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR" \
        > "$SRC/$1.log" 2>&1 \
        || { tail -30 "$SRC/$1.log"; exit 1; }
    cp "$SRC/$1/$3/$5" "$OUT/"
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
    mame2010)
        clone mame2010-libretro mame2010
        echo "== MAME 2010 (ci vuole molto)"
        # Compilazione incrociata da amd64: i ricompilatori dinamici x86 (SH-2 e DRC) non vanno
        # compilati, al loro posto il backend in C; PTR64 per i puntatori a 64 bit dell'A53; la zlib
        # interna, perché il kit non ha quella per aarch64.
        make -C "$SRC/mame2010" -j"$JOBS" platform=unix PTR64=1 BUILD_ZLIB=1 X86_SH2DRC=0 FORCE_DRC_C_BACKEND=1 \
            CC="$CXX -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" LD="$CXX -mcpu=cortex-a53" \
            CC_AS="$CC -mcpu=cortex-a53" AR="$AR" \
            > "$SRC/mame2010.log" 2>&1 \
            || { tail -30 "$SRC/mame2010.log"; exit 1; }
        cp "$SRC/mame2010/mame2010_libretro.so" "$OUT/"
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
    pcsx)
        # PCSX ReARMed (PlayStation): ha un sottomodulo (libpicofe), che il clone locale non
        # porta. Si copia la cartella intera, sottomodulo compreso, e si toglie ogni file di build.
        rm -rf "$SRC/pcsx"
        cp -a /trimui/deps/pcsx_rearmed "$SRC/pcsx"
        git -C "$SRC/pcsx" clean -qfdx
        git -C "$SRC/pcsx" apply /trimui/toolchain/patches/pcsx-rearmed-lr.patch
        echo "== PCSX ReARMed (PlayStation)"
        make -C "$SRC/pcsx" -f Makefile.libretro -j"$JOBS" platform=rpi3_64 \
            CC="$CC" CXX="$CXX" AR="$AR" \
            > "$SRC/pcsx.log" 2>&1 \
            || { tail -30 "$SRC/pcsx.log"; exit 1; }
        cp "$SRC/pcsx/pcsx_rearmed_libretro.so" "$OUT/"
        ;;
    # console leggere (pacchetti dello Store, uno per core)
    stella2014) echo "== Stella 2014 (Atari 2600)"; simple stella2014 stella2014-libretro . Makefile stella2014_libretro.so ;;
    prosystem) echo "== ProSystem (Atari 7800)"; simple prosystem prosystem-libretro . Makefile prosystem_libretro.so ;;
    handy) echo "== Handy (Atari Lynx)"; simple handy libretro-handy . Makefile handy_libretro.so ;;
    wswan) echo "== Beetle WonderSwan"; simple wswan beetle-wswan-libretro . Makefile mednafen_wswan_libretro.so ;;
    vb) echo "== Beetle VB (Virtual Boy)"; simple vb beetle-vb-libretro . Makefile mednafen_vb_libretro.so ;;
    gearcoleco) echo "== Gearcoleco (ColecoVision)"; simple gearcoleco Gearcoleco platforms/libretro Makefile gearcoleco_libretro.so ;;
    pokemini) echo "== PokeMini (Pokemon Mini)"; simple pokemini PokeMini . Makefile.libretro pokemini_libretro.so ;;
    freeintv) echo "== FreeIntv (Intellivision)"; simple freeintv FreeIntv . Makefile freeintv_libretro.so ;;
    # computer (cartella Computer dello Store): leggeri, senza GPU
    fuse) echo "== Fuse (ZX Spectrum)"; simple fuse fuse-libretro . Makefile fuse_libretro.so ;;
    bluemsx) echo "== blueMSX (MSX, MSX2)"; simple bluemsx blueMSX-libretro . Makefile bluemsx_libretro.so ;;
    vice) echo "== VICE x64 (Commodore 64) (ci vuole qualche minuto)"; simple vice vice-libretro . Makefile vice_x64_libretro.so ;;
    scummvm)
        # ScummVM: i due sottomoduli (libretro-deps, libretro-common) sono già in deps/scummvm, alle
        # versioni che chiede dependencies.mk, così non si scarica niente qui. Motori "lite" (l'elenco
        # che ScummVM stesso usa per le console poco potenti) e niente OpenGL: il runner disegna
        # senza GPU. Poi scummvm.zip con i file dei dati e i temi della sua interfaccia.
        # copia nuova solo se la versione è cambiata: i file già compilati restano (ci vuole molto)
        want=$(git -C /trimui/deps/scummvm rev-parse HEAD)
        if [ "$(git -C "$SRC/scummvm" rev-parse HEAD 2>/dev/null)" != "$want" ]; then
            rm -rf "$SRC/scummvm"
            cp -a /trimui/deps/scummvm "$SRC/scummvm"
        fi
        echo "== ScummVM (ci vuole molto)"
        make -C "$SRC/scummvm/backends/platform/libretro" -j"$JOBS" platform=unix LITE=1 HAVE_OPENGL=0 \
            DEBUG_ALLOW_DIRTY_SUBMODULES=1 CC="$CC -mcpu=cortex-a53" CXX="$CXX -mcpu=cortex-a53" AR="$AR cr" \
            RANLIB=aarch64-linux-gnu-ranlib > "$SRC/scummvm.log" 2>&1 \
            || { tail -30 "$SRC/scummvm.log"; exit 1; }
        make -C "$SRC/scummvm/backends/platform/libretro" platform=unix LITE=1 HAVE_OPENGL=0 \
            DEBUG_ALLOW_DIRTY_SUBMODULES=1 datafiles >> "$SRC/scummvm.log" 2>&1 \
            || { tail -30 "$SRC/scummvm.log"; exit 1; }
        cp "$SRC/scummvm/backends/platform/libretro/scummvm_libretro.so" "$OUT/"
        mkdir -p /build/out/scummvm
        cp "$SRC/scummvm/backends/platform/libretro/scummvm.zip" /build/out/scummvm/
        ;;
    esac
done
aarch64-linux-gnu-strip "$OUT"/*.so
for f in "$OUT"/*.so; do
    printf '%s: %s, glibc fino a %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)" \
        "$(aarch64-linux-gnu-objdump -T "$f" | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)"
done
