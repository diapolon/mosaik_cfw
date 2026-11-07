#!/bin/sh
# Sistema Arcade su SD (M2), dalla cartella trimui:
#   sh m2/build-m2.sh
# Produce dist/arcade-system.squashfs (unico per tutti i modelli) e un'immagine SD per modello:
#   dist/m2-brickpro.img   TrimUI Brick Pro (TG4040)
#   dist/m2-smartpro.img   TrimUI Smart Pro (TG5040)
# Le immagini hanno lo stesso contenuto: cambiano solo l'area di avvio (boot0, U-Boot con la
# configurazione dell'hardware, kernel), che è propria di ogni modello.
# Servono il firmware estratto (toolchain/analyze-firmware.sh -> /build/fw/<modello>) e i
# programmi e i core compilati (Makefile.tg5040, toolchain/build-cores.sh).
set -e
cd "$(dirname "$0")/.."
TOP="$PWD"

# dati e core (stessa cartella Arcade del pacchetto per il firmware originale)
sh toolchain/package.sh >/dev/null

# programmi statici dell'initramfs: messaggi a schermo (lo stesso della prova M1) e
# allargamento della partizione ARCADE a tutta la scheda
docker run --rm --platform linux/amd64 -v "$TOP/m1":/m1:ro -v "$TOP/m2":/m2:ro -v trimui-build:/build trimui-tg5040 sh -c \
    'mkdir -p /build/m2 && aarch64-linux-gnu-gcc -static -O2 -o /build/m2/fbmsg /m1/m1test.c && aarch64-linux-gnu-strip /build/m2/fbmsg &&
     aarch64-linux-gnu-gcc -static -O2 -Wall -o /build/m2/sdgrow /m2/sdgrow.c && aarch64-linux-gnu-strip /build/m2/sdgrow'

# tutto il resto in un Linux arm64 (script separato: niente apici annidati)
docker run --rm --platform linux/arm64 -v "$TOP":/trimui -v trimui-build:/build alpine:3.20 sh /trimui/m2/build-in-container.sh
