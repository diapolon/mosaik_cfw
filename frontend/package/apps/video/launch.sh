#!/bin/sh
# Video (app dello Store): parte come un gioco Linux; ROOT è la cartella Arcade. Le librerie di
# FFmpeg (LGPL, condivise) sono in lib/ accanto al programma.
cd "$(dirname "$0")" || exit 1
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./video --root "${ROOT:-/mnt/SDCARD/Arcade}" --app "$PWD"
