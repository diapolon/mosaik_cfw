#!/bin/sh
# Immagine SD della prova M1 per la Brick Pro (TG4040), dalla cartella trimui:
#   sh m1/build-m1.sh            -> dist/m1-brickpro.img (256 MB)
# Pezzi di avvio originali dal firmware v1.1.1 (estratti da toolchain/analyze-firmware.sh in
# /build/fw/tg4040b): boot0 e boot_package in modalità normale (non quella della scheda di
# ripristino, che riscrive la memoria interna), kernel 4.9.191 e ambiente di U-Boot.
# Disposizione come Knulli e come la scheda di ripristino TrimUI:
#   boot0 a 8 KiB (copia a 128 KiB), GPT con le voci a 80 KiB, boot_package al settore 32800,
#   partizioni da 36 MiB: boot (kernel + nostro initramfs), env, env-redund, boot-resource (FAT).
set -e
cd "$(dirname "$0")/.."
TOP="$PWD"
mkdir -p dist

# 1. programma di prova, statico, con il compilatore del kit TrimUI
docker run --rm --platform linux/amd64 -v "$TOP/m1":/m1 -v trimui-build:/build trimui-tg5040 sh -c \
    'mkdir -p /build/m1 && aarch64-linux-gnu-gcc -static -O2 -Wall -o /build/m1/m1test /m1/m1test.c && aarch64-linux-gnu-strip /build/m1/m1test'

# 2. initramfs, immagine di avvio e scheda, in un Linux arm64
docker run --rm --platform linux/arm64 -v "$TOP":/trimui -v trimui-build:/build alpine:3.20 sh -c '
set -e
apk add -q busybox-static cpio python3 sgdisk dosfstools mtools >/dev/null
FW=/build/fw/tg4040b
[ -f $FW/boot.fex ] || { echo "manca il firmware estratto: lanciare prima toolchain/analyze-firmware.sh"; exit 1; }
W=/build/m1; rm -rf $W/rd; mkdir -p $W/rd/bin $W/rd/dev $W/rd/proc $W/rd/sys $W/rd/tmp $W/rd/mnt
cp /bin/busybox.static $W/rd/bin/busybox
cp $W/m1test $W/rd/bin/m1test
cp /trimui/m1/rdinit $W/rd/rdinit
# trimui_inputd del firmware e le librerie di base che usa (glibc 2.33 del firmware)
R=$FW/root; mkdir -p $W/rd/lib $W/rd/usr/trimui/bin
cp $R/usr/trimui/bin/trimui_inputd $W/rd/usr/trimui/bin/
for l in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libpthread.so.0 libgcc_s.so.1 libdl.so.2 librt.so.1; do
    f=$(find $R/lib $R/usr/lib -name "$l" | head -1); [ -n "$f" ] && cp -L "$f" $W/rd/lib/
done
ln -sf lib $W/rd/lib64
chmod 755 $W/rd/rdinit $W/rd/bin/*
ln -sf rdinit $W/rd/init
(cd $W/rd && find . | cpio -o -H newc --owner 0:0 2>/dev/null | gzip -9) > $W/initramfs.cpio.gz
python3 -I /trimui/toolchain/bootimg.py pack $FW/boot.fex $W/boot.img $W/initramfs.cpio.gz

# partizione FAT con il file segnaposto (solo lì lo script scrive)
rm -f $W/fat.img; truncate -s 180M $W/fat.img
mkfs.vfat -F 32 -n ARCADE $W/fat.img >/dev/null
mcopy -i $W/fat.img /trimui/m1/ARCADE-M1.txt ::ARCADE-M1.txt
mmd -i $W/fat.img ::LOG

IMG=$W/m1.img; rm -f $IMG; truncate -s 256M $IMG
S=2048   # settori per MiB
sgdisk -o -j 160 \
  -n 1:$((36*S)):+32M -c 1:boot \
  -n 2:0:+1M -c 2:env \
  -n 3:0:+1M -c 3:env-redund \
  -n 4:0:0 -t 4:0700 -c 4:boot-resource \
  $IMG >/dev/null
put() { dd if="$1" of=$IMG bs=512 seek=$2 conv=notrunc status=none; }
start() { sgdisk -i $1 $IMG | sed -n "s/^First sector: \([0-9]*\).*/\1/p"; }
put $FW/boot0_sdcard.fex 16
put $FW/boot0_sdcard.fex 256
put $FW/boot_package.fex 32800
put $W/boot.img $(start 1)
put $FW/env.fex $(start 2)
put $FW/env.fex $(start 3)
put $W/fat.img $(start 4)
sgdisk -v $IMG | tail -1
sgdisk -p $IMG | tail -6
cp $IMG /trimui/dist/m1-brickpro.img
'
echo "pronto: dist/m1-brickpro.img"
