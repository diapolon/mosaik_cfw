#!/bin/sh
# Scarica in dist/release/sources i sorgenti elencati in toolchain/sorgenti-firmware.txt e ne
# controlla l'impronta; un file già presente e giusto non si riscarica.
#   sh toolchain/sorgenti-firmware.sh            scarica quelli che mancano
#   sh toolchain/sorgenti-firmware.sh --check    solo controllo: esce con 1 se ne manca uno o è diverso
# SORGENTI: un'altra cartella (per le prove).
TOP=$(cd "$(dirname "$0")/.." && pwd)
DIR=${SORGENTI:-$TOP/dist/release/sources}
LIST=$TOP/toolchain/sorgenti-firmware.txt
sum() { if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
mkdir -p "$DIR"
bad=0
grep -v '^#' "$LIST" | while read -r name url sha; do
    [ -n "$name" ] || continue
    if [ -f "$DIR/$name" ] && [ "$(sum "$DIR/$name")" = "$sha" ]; then continue; fi
    if [ "$1" = --check ]; then
        echo "manca o è diverso: $name"; echo x >> "$DIR/.sorgenti-mancanti"; continue
    fi
    echo "scarico $name"
    if curl -sSfL -o "$DIR/.$name.part" "$url" && [ "$(sum "$DIR/.$name.part")" = "$sha" ]; then
        mv -f "$DIR/.$name.part" "$DIR/$name"
    else
        rm -f "$DIR/.$name.part"; echo "ERRORE: $name non scaricato o con un'impronta diversa"; echo x >> "$DIR/.sorgenti-mancanti"
    fi
done
[ -f "$DIR/.sorgenti-mancanti" ] && bad=1
rm -f "$DIR/.sorgenti-mancanti"
exit $bad
