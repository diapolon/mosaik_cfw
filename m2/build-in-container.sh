#!/bin/sh
# Costruzione del sistema Arcade su SD, dentro un container Alpine arm64 (lanciato da
# m2/build-m2.sh). Produce:
#   /build/m2/arcade-system.squashfs   un solo sistema per tutti i modelli
#   /build/m2/m2-<modello>.img         un'immagine SD per modello (cambia solo l'area di avvio)
#
# Il sistema ha una parte comune (BusyBox, i nostri programmi, glibc) e uno strato per modello
# in /fw/<id>: driver e librerie della GPU, SDL2, ALSA, udev, trimui_inputd e la preparazione
# dell'hardware, presi dal firmware TrimUI di quel modello. L'initramfs del modello collega
# /run/fw allo strato giusto prima di passare al sistema.
set -e
apk add -q busybox-static cpio python3 binutils squashfs-tools sgdisk dosfstools mtools >/dev/null
W=/build/m2
R=$W/root
T=/trimui
# un modello per riga: id dello strato | cartella del firmware estratto | nome nel menu TrimUI |
# nome del file immagine
MODELS="tg4040|tg4040b|Trimui Brick Pro|brickpro
tg5040|tg5040|Trimui Smart Pro|smartpro"
models() { echo "$MODELS"; }

models | while IFS="|" read -r id fw name file; do
    [ -f /build/fw/$fw/boot.fex ] || { echo "manca il firmware estratto $fw (toolchain/analyze-firmware.sh)"; exit 1; }
done

# ---------------------------------------------------------------- parte comune
rm -rf $R
mkdir -p $R/bin $R/sbin $R/usr/bin $R/usr/sbin $R/usr/lib $R/usr/share $R/usr/arcade/bin $R/lib/firmware $R/etc \
         $R/proc $R/sys $R/dev $R/tmp $R/run $R/var $R/root $R/mnt/SDCARD $R/mnt/UDISK $R/fw
cp /bin/busybox.static $R/bin/busybox
for a in $($R/bin/busybox --list-full); do [ -e $R/$a ] || ln -s /bin/busybox $R/$a; done
rm -f $R/sbin/init
cp $T/m2/system-init $R/sbin/init
cp /build/out/bin/launcher /build/out/bin/runner /build/out/bin/gamewatch /build/out/bin/gamemenu $W/fbmsg $W/envtool $W/fatnomi $W/axpctl $R/usr/arcade/bin/
# fsck.fat (Alpine, musl) anche nel sistema: controllo della SD su richiesta (System/controlla-sd)
cp /sbin/fsck.fat $R/usr/sbin/fsck.fat
cp /lib/ld-musl-aarch64.so.1 $R/lib/
ln -sf ld-musl-aarch64.so.1 $R/lib/libc.musl-aarch64.so.1
# Wi-Fi: servizio (wifi-ctl), eventi di wpa_cli e script di udhcpc
cp $T/m2/wifi-ctl $T/m2/wifi-event $T/m2/udhcpc-script $T/m2/arcade-suspend $T/m2/prova-wifi $R/usr/arcade/bin/
# la SDL2 di TrimUI chiama "setterm" per nascondere il cursore: basta che esista
rm -f $R/usr/bin/setterm
printf "#!/bin/sh\nexit 0\n" > $R/usr/bin/setterm
F0=/build/fw/tg4040b/root
cp $F0/etc/passwd $F0/etc/group $R/etc/
echo arcade > $R/etc/hostname
# versione: numero (file VERSION) e data di compilazione; l'aggiornamento via Wi-Fi le confronta
echo "$(cat $T/VERSION) $(date -u +%Y%m%d.%H%M)" > $R/etc/arcade-version
cp $R/etc/arcade-version $T/dist/arcade-version.txt
cp $F0/lib/firmware/rgx.* $R/lib/firmware/
# firmware del chip Wi-Fi/Bluetooth XR829 (uguale nei due modelli)
cp $F0/lib/firmware/*xr829* $R/lib/firmware/
# nomi internet: resolv.conf lo scrive udhcpc in /run (lo squashfs è di sola lettura)
ln -s /run/resolv.conf $R/etc/resolv.conf
printf "127.0.0.1 localhost arcade\n" > $R/etc/hosts
printf "hosts: files dns\nnetworks: files\n" > $R/etc/nsswitch.conf
# percorsi fissi che puntano allo strato del modello (/run/fw, creato dall'initramfs)
mkdir -p $R/lib
ln -s /run/fw/udev $R/lib/udev
ln -s /run/fw/etc-udev $R/etc/udev
ln -s /run/fw/asound.conf $R/etc/asound.conf
ln -s /run/fw/alsa $R/usr/share/alsa

# ---------------------------------------------------------------- strati per modello
models | while IFS="|" read -r id fw name file; do
    F=/build/fw/$fw/root; L=$R/fw/$id
    mkdir -p $L/bin $L/modules
    python3 -I $T/toolchain/libclosure.py $F /build/out/bin/launcher /build/out/bin/runner /build/out/cores/*.so \
        $F/usr/trimui/bin/trimui_inputd $F/usr/bin/amixer $F/usr/bin/tinymix $F/usr/bin/aplay $F/usr/bin/tinyplay \
        $F/usr/bin/tinypcminfo $F/sbin/udevd $F/sbin/udevadm $F/usr/sbin/wpa_supplicant $F/usr/sbin/wpa_cli $F/usr/sbin/iw \
        +libEGL.so.1 +libGLESv2.so.2 +libasound.so.2 +libIMGegl.so +libsrv_um.so +libusc.so +libglslcompiler.so +libudev.so.1 \
        > $W/libs-$id.txt 2> $W/libs-mancanti-$id.txt || { echo "strato $id:"; cat $W/libs-mancanti-$id.txt; exit 1; }
    for l in $(cat $W/libs-$id.txt); do mkdir -p $L/$(dirname $l); cp -L $F/$l $L/$l; done
    for pat in "libEGL*" "libGLES*" "libpvr*" "libPVR*" "libIMG*" "libsrv_um*" "libusc*" "libglslcompiler*" "libasound*"; do
        for f in $F/usr/lib/$pat; do [ -e "$f" ] && cp -a "$f" $L/usr/lib/; done
    done
    # il caricatore dinamico e la glibc vanno anche nel percorso fisso /lib (sono uguali nei
    # firmware v1.1.1; le altre librerie si prendono dallo strato con LD_LIBRARY_PATH)
    # libnss_*: senza, la glibc non risolve i nomi internet (scraper, ora da internet)
    for g in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libresolv.so.2 libcrypt.so.1 \
             libnss_dns.so.2 libnss_files.so.2; do
        [ -e $R/lib/$g ] || { f=$(find $F/lib -name $g | head -1); [ -n "$f" ] && cp -L "$f" $R/lib/$g; }
    done
    cp $F/usr/trimui/bin/trimui_inputd $F/usr/bin/amixer $F/usr/bin/tinymix $F/usr/bin/aplay $F/usr/bin/tinyplay \
       $F/usr/bin/tinypcminfo $F/sbin/udevd $F/sbin/udevadm $F/usr/sbin/wpa_supplicant $F/usr/sbin/wpa_cli $F/usr/sbin/iw $L/bin/
    # moduli del kernel: GPU e Wi-Fi (compilati per il kernel di ogni modello)
    cp $F/lib/modules/4.9.191/pvrsrvkm.ko $F/lib/modules/4.9.191/dc_sunxi.ko $F/lib/modules/4.9.191/xradio_*.ko $L/modules/
    # Brick Pro: lo stesso driver Wi-Fi compilato per i kernel di Brick e Smart Pro, per la prova
    # del blocco all'accensione del Wi-Fi (m2/prova-wifi; kernel della Brick con la stessa
    # configurazione, della Smart Pro con 3 voci diverse e nessuna del Wi-Fi)
    if [ $id = tg4040 ]; then
        for o in brick:tg3040 smartpro:tg5040; do
            mkdir -p $L/modules-${o%%:*}
            cp /build/fw/${o#*:}/root/lib/modules/4.9.191/xradio_*.ko $L/modules-${o%%:*}/
        done
    fi
    cp -a $F/usr/share/alsa $L/alsa
    cp $F/etc/asound.conf $L/asound.conf
    mkdir -p $L/udev $L/etc-udev
    cp -a $F/lib/udev/. $L/udev/
    cp -a $F/etc/udev/. $L/etc-udev/
    cp $T/m2/hw-init-$id.sh $L/hw-init.sh
    bits=24; [ $id = tg5040 ] && bits=32
    cp $T/m2/bootlogo-$bits.bmp $L/bootlogo.bmp   # lo stesso bootlogo.bmp dell'immagine
    chmod 755 $L/bin/* $L/hw-init.sh
    echo "strato $id: $(du -sh $L | cut -f1), librerie $(wc -l < $W/libs-$id.txt)"
    if [ -s $W/libs-mancanti-$id.txt ]; then echo "librerie non trovate ($id):"; cat $W/libs-mancanti-$id.txt; fi
done

chmod 755 $R/sbin/init $R/usr/arcade/bin/* $R/usr/bin/setterm
[ -x $R/sbin/init ] || { echo "sbin/init non eseguibile"; exit 1; }
rm -f $W/arcade-system.squashfs
mksquashfs $R $W/arcade-system.squashfs -comp zstd -all-root -noappend -quiet >/dev/null
echo "sistema: $(du -h $W/arcade-system.squashfs | cut -f1)"

# ---------------------------------------------------------------- partizione FAT (comune)
# Cluster da 32 KB e tabella dei cluster già grande per una scheda da 256 GB: all'avvio sdgrow
# allarga il FAT fino alla fine della scheda cambiando solo il numero di settori (fatprep.py).
# 2200 MB è il minimo per un FAT32 con cluster da 32 KB (65525 cluster).
FAT=$W/fat.img; rm -f $FAT; truncate -s 256G $FAT
mkfs.vfat -F 32 -s 64 -n MOSAIK $FAT >/dev/null
python3 -I $T/m2/fatprep.py $FAT 2200
export MTOOLS_SKIP_CHECK=1
mcopy -s -i $FAT $T/dist/SD/Arcade ::Arcade
mcopy -o -i $FAT $W/arcade-system.squashfs ::Arcade/System/arcade-system.squashfs
mcopy -i $FAT $T/m2/LEGGIMI.txt ::LEGGIMI.txt
# macOS: niente indice Spotlight né registro degli eventi su questa scheda
: > $W/vuoto; mcopy -i $FAT $W/vuoto ::.metadata_never_index
mmd -i $FAT ::.fseventsd; mcopy -i $FAT $W/vuoto ::.fseventsd/no_log

# ---------------------------------------------------------------- initramfs e immagini per modello
apk add -q dosfstools >/dev/null
models | while IFS="|" read -r id fw name file; do
    FW=/build/fw/$fw
    # sicurezza: il byte "work mode" del boot_package (offset 2272) deve essere 0, avvio
    # normale. Con 0x11 (schede di ripristino TrimUI) U-Boot riscriverebbe la memoria interna
    wm=$(dd if=$FW/boot_package.fex bs=1 skip=2272 count=1 2>/dev/null | od -An -tu1 | tr -d " ")
    [ "$wm" = 0 ] || { echo "ATTENZIONE: boot_package di $fw con work mode $wm (non 0): fermo la costruzione"; exit 1; }
    I=$W/initramfs-$id; rm -rf $I; mkdir -p $I/bin $I/sbin $I/lib $I/etc $I/dev $I/proc $I/sys $I/tmp
    cp /bin/busybox.static $I/bin/busybox
    cp $W/fbmsg $W/sdgrow $W/axpctl $I/bin/
    # fsck.fat di Alpine (dosfstools, musl) con il suo caricatore, per riparare il FAT a ogni avvio
    cp /sbin/fsck.fat $I/sbin/fsck.fat
    cp /lib/ld-musl-aarch64.so.1 $I/lib/
    ln -sf ld-musl-aarch64.so.1 $I/lib/libc.musl-aarch64.so.1
    printf "%s\n%s\n" "$id" "$name" > $I/etc/arcade-model
    sed -n 's/^GENERAZIONE=//p' $T/m2/area-avvio.conf > $I/etc/area-avvio
    cp $T/m2/initramfs-init $I/rdinit
    chmod 755 $I/rdinit $I/bin/*
    ln -sf rdinit $I/init
    (cd $I && find . | cpio -o -H newc --owner 0:0 2>/dev/null | gzip -9) > $W/initramfs-$id.cpio.gz
    python3 -I $T/toolchain/bootimg.py pack $FW/boot.fex $W/boot-$id.img $W/initramfs-$id.cpio.gz

    IMG=$W/m2-$file.img; rm -f $IMG; truncate -s 2300M $IMG
    S=2048
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
    put $W/boot-$id.img $(start 1)
    # ambiente di U-Boot con loglevel=4: con 8 il kernel scrive ogni messaggio sulla porta
    # seriale (115200 baud) e l'avvio rallenta; i messaggi restano comunque in dmesg
    python3 -I - "$FW/env.fex" "$W/env-$id.fex" <<'PYEOF'
import sys, struct, zlib
d = bytearray(open(sys.argv[1], "rb").read())
assert struct.unpack("<I", d[:4])[0] == zlib.crc32(bytes(d[5:])) & 0xffffffff, "env.fex: CRC"
i = d.find(b"\0loglevel=8\0")
assert i > 0, "env.fex: loglevel=8 non trovato"
d[i + 10] = ord("4")
struct.pack_into("<I", d, 0, zlib.crc32(bytes(d[5:])) & 0xffffffff)
open(sys.argv[2], "wb").write(d)
PYEOF
    put $W/env-$id.fex $(start 2)
    put $W/env-$id.fex $(start 3)
    P4=$(( $(sgdisk -i 4 $IMG | sed -n "s/^Partition size: \([0-9]*\).*/\1/p") * 512 ))
    [ $(stat -c %s $FAT) -le $P4 ] || { echo "FAT più grande della partizione"; exit 1; }
    put $FAT $(start 4)
    # logo di avvio di U-Boot (bootlogo.bmp nella partizione boot-resource, cioè ARCADE): come
    # l'originale, 32 bit per la Smart Pro e 24 per la Brick Pro
    bits=24; [ $id = tg5040 ] && bits=32
    mcopy -o -i "$IMG@@$(( $(start 4) * 512 ))" $T/m2/bootlogo-$bits.bmp ::bootlogo.bmp
    sgdisk -v $IMG | grep -qi "no problems" || { sgdisk -v $IMG; exit 1; }
    wm=$(dd if=$IMG bs=1 skip=$((32800 * 512 + 2272)) count=1 2>/dev/null | od -An -tu1 | tr -d " ")
    [ "$wm" = 0 ] || { echo "ATTENZIONE: immagine $file con work mode $wm: non usarla"; rm -f $IMG; exit 1; }
    cp $IMG $T/dist/m2-$file.img
    echo "immagine $file: dist/m2-$file.img ($name)"
done
cp $W/arcade-system.squashfs $T/dist/arcade-system.squashfs
