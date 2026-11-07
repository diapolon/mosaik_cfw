#!/bin/sh
# Copia verificata sulla SD ARCADE dal Mac:
#   sh toolchain/sd-copy.sh ORIGINE DESTINAZIONE [ORIGINE DESTINAZIONE]...
# es. sh toolchain/sd-copy.sh dist/arcade-system.squashfs Arcade/System/arcade-system.squashfs
# DESTINAZIONE è relativa alla radice della SD (/Volumes/ARCADE).
#
# Alcuni lettori USB (per esempio il Super Top 14cd:1212) ogni tanto mettono un blocco da 32 KB
# nel posto sbagliato: si copia con un nome provvisorio, si smonta e rimonta (niente cache del
# Mac), si confronta e, se serve, si riprova; solo una copia identica prende il nome giusto,
# e dopo un altro rimontaggio si rilegge due volte.
set -u
VOL=/Volumes/ARCADE
[ -d "$VOL" ] || { echo "SD ARCADE non collegata"; exit 1; }
DEV=$(diskutil info "$VOL" | sed -n 's/.*Device Node: *//p')

remount() {
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
        diskutil unmount "$VOL" > /dev/null 2>&1 && break
        [ $i = 12 ] && { echo "impossibile smontare la SD (in uso?)"; exit 1; }
        sleep 5
    done
    diskutil mount "$DEV" > /dev/null || { echo "impossibile rimontare la SD"; exit 1; }
}

fail=0
while [ $# -ge 2 ]; do
    src=$1 dst="$VOL/$2"
    shift 2
    tmp="$dst.nuovo"
    ok=0
    for try in 1 2 3 4 5; do
        rm -f "$tmp"
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$tmp" && sync
        remount
        if cmp -s "$src" "$tmp"; then ok=1; break; fi
        echo "$(basename "$src"): copia $try diversa, riprovo"
    done
    if [ $ok = 0 ]; then echo "$(basename "$src"): NON RIUSCITA dopo 5 tentativi"; rm -f "$tmp"; fail=1; continue; fi
    mv -f "$tmp" "$dst" && sync
    remount
    if cmp -s "$src" "$dst" && cmp -s "$src" "$dst"; then
        echo "$(basename "$src"): ok (tentativo $try)"
    else
        echo "$(basename "$src"): ATTENZIONE, dopo il rinomino la rilettura è diversa"
        fail=1
    fi
done
exit $fail
