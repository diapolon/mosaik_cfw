#!/bin/sh
# vtest sul Mac, con FFmpeg, SDL2 e SDL2_ttf di Homebrew (stessa versione di FFmpeg della console).
#   sh toolchain/vtest/build-mac.sh OUT
set -e
OUT="${1:-build-vtest}"
mkdir -p "$OUT"
cc -O2 -Wall -Wextra -o "$OUT/vtest" "$(dirname "$0")/vtest.c" \
    $(pkg-config --cflags --libs libavformat libavcodec libswscale libswresample libavutil sdl2 SDL2_ttf)
echo "$OUT/vtest"
