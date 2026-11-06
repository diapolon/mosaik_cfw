#!/bin/sh
# Analisi di un firmware TrimUI (aggiornamento .zip/.7z con dentro un .awimg): estrae il
# sistema e stampa ciò che serve al pacchetto Arcade. Gira in un container Linux:
#   docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro \
#       alpine:3.20 sh /trimui/toolchain/analyze-firmware.sh <archivio dentro /trimui> <nome>
set -e
ARCH="/trimui/$1"
NAME="$2"
W="/build/fw/$NAME"
apk add -q 7zip python3 e2fsprogs e2fsprogs-extra dtc binutils >/dev/null
mkdir -p "$W" && cd "$W"
if [ ! -f image.awimg ]; then
    7z e -y "$ARCH" -o"$W/x" >/dev/null
    IMG=$(find "$W/x" -iname "*.awimg" | head -1)
    # alcuni aggiornamenti hanno un archivio dentro l'archivio
    [ -z "$IMG" ] && for a in "$W"/x/*.7z "$W"/x/*.zip; do [ -f "$a" ] && 7z e -y "$a" -o"$W/x" >/dev/null; done
    IMG=$(find "$W/x" -iname "*.awimg" | head -1)
    [ -z "$IMG" ] && { echo "nessun .awimg in $ARCH"; ls "$W/x"; exit 1; }
    mv "$IMG" image.awimg
    rm -rf "$W/x"
fi
python3 -I /trimui/toolchain/awimg.py image.awimg . rootfs.fex sunxi.fex >/dev/null
[ -d root ] || { mkdir root; debugfs -R "rdump / root" rootfs.fex 2>/dev/null; }
dtc -I dtb -O dts -o sunxi.dts sunxi.fex 2>/dev/null || true
R=root
echo "== $NAME"
echo "modello (MainUI): $(grep -aoE 'Trimui [A-Za-z ]{2,20}' $R/usr/trimui/bin/MainUI | sort -u | tr '\n' ' ')"
echo "schermo: $(grep -oE 'lcd_(x|y) = <0x[0-9a-f]+>' sunxi.dts | sort -u | sed 's/lcd_//;s/ = <//;s/>//' | while read k v; do printf '%s=%d ' $k $v; done)"
echo "glibc: $(ls $R/lib | grep -oE 'libc-[0-9.]+so' | head -1)"
echo "SDL2: $(find $R/usr/lib $R/usr/trimui/lib -name 'libSDL2-2.0.so.0.*' | sed 's/.*so\.0\.//' | tr '\n' ' ')"
for l in libssl.so.1.1 libcrypto.so.1.1 libz.so.1 libGLESv2.so.2 libEGL.so.1; do
    printf '%s: %s\n' $l "$(find $R/usr/lib $R/lib $R/usr/trimui/lib -name "$l" 2>/dev/null | head -1 | sed "s|$R||" | grep . || echo MANCA)"
done
echo "batteria: $(grep -rhoaE '/sys/class/power_supply/[a-z0-9_-]+' $R/usr/trimui/bin 2>/dev/null | sort -u | tr '\n' ' ')"
echo "luminosità /dev/disp: $(grep -lac '/dev/disp' $R/usr/trimui/bin/* 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"
echo "demone tasti: $(ls $R/usr/trimui/bin | grep -i inputd | tr '\n' ' ')· $(strings $R/usr/trimui/bin/trimui_inputd* 2>/dev/null | grep -oE 'TRIMUI Player[0-9]?' | sort -u | tr '\n' ' ')"
echo "amixer: $(ls $R/usr/bin $R/bin 2>/dev/null | grep -E '^(amixer|tinymix)$' | tr '\n' ' ')"
