#!/bin/sh
# Deadcast (emulatore Dreamcast) in deps/deadcast: il pacchetto pubblicato dall'autore, a una
# versione fissa e controllato con lo SHA-256. Lo usa toolchain/package.sh.
#   sh toolchain/fetch-deadcast.sh
set -e
VER=1.2.5
SHA=37780776628972717e2edc20836c8ad15b20fdc40d664d951c85600e489793f3
D="$(cd "$(dirname "$0")/.." && pwd)/deps/deadcast"
Z="$D/DC-DEADCAST-$VER.pak.zip"
mkdir -p "$D"
if [ ! -f "$Z" ]; then
    curl -fsSL -o "$Z.part" "https://github.com/tyrannotorus/emulator-dreamcast-deadcast/releases/download/$VER/DC-DEADCAST.pak.zip"
    mv "$Z.part" "$Z"
fi
echo "$SHA  $Z" | shasum -a 256 -c - >/dev/null || { echo "deadcast: SHA-256 diverso"; rm -f "$Z"; exit 1; }
echo "$Z"
