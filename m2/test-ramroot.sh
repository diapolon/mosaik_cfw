#!/bin/sh
# Sistema in memoria (m2/ramroot.c, 23 novembre): il sistema è uno squashfs su un loop che legge un
# file sulla SD; ramroot cambia il file sotto il loop (LOOP_CHANGE_FD) con una copia in memoria, così
# togliendo la SD il sistema resta leggibile. Qui una «SD» finta (immagine FAT su un loop) contiene
# lo squashfs, montato come quello vero; si svuota la cache, si stacca la SD e si legge il sistema.
# Serve --privileged (loop, mount, drop_caches):
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-ramroot.sh
apk add -q build-base linux-headers squashfs-tools dosfstools > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
gcc -std=gnu11 -O2 -Wall -Wextra -o /tmp/ramroot /trimui/m2/ramroot.c 2> /tmp/cc.log || { cat /tmp/cc.log; ko "ramroot non compila"; echo; echo "$errors prove fallite"; exit 1; }

prepara() {   # SD finta con il sistema finto, sistema montato come in m2/initramfs-init
    mkdir -p /tmp/rootsrc/bin /tmp/sd /tmp/root
    dd if=/dev/urandom of=/tmp/rootsrc/bin/programma bs=1M count=4 2> /dev/null
    md5sum /tmp/rootsrc/bin/programma | cut -d' ' -f1 > /tmp/md5
    rm -f /tmp/sys.sqfs; mksquashfs /tmp/rootsrc /tmp/sys.sqfs -quiet -noappend > /dev/null
    dd if=/dev/zero of=/tmp/sd.img bs=1M count=24 2> /dev/null; mkfs.vfat /tmp/sd.img > /dev/null
    SDLOOP=$(losetup -f) && losetup "$SDLOOP" /tmp/sd.img
    mount -t vfat "$SDLOOP" /tmp/sd && cp /tmp/sys.sqfs /tmp/sd/arcade-system.squashfs
    mount -t squashfs -o loop,ro /tmp/sd/arcade-system.squashfs /tmp/root
    ROOTLOOP=$(awk '$2 == "/tmp/root" { print $1 }' /proc/mounts)
}
togli_sd() {   # come togliere la scheda: dalla «SD» arrivano solo zeri, e la cache è vuota
    sync; truncate -s 0 /tmp/sd.img; echo 3 > /proc/sys/vm/drop_caches
}
leggi() { md5sum /tmp/root/bin/programma 2> /dev/null | cut -d' ' -f1; }
pulisci() { umount /tmp/root 2> /dev/null; losetup -d "$ROOTLOOP" 2> /dev/null; umount /tmp/sd 2> /dev/null; losetup -d "$SDLOOP" 2> /dev/null; rm -f /run/sistema.sqfs; }

echo "--- senza ramroot: SD tolta, il sistema non si legge (la prova vede davvero la scheda tolta)"
prepara
togli_sd
[ "$(leggi)" = "$(cat /tmp/md5)" ] && ko "il sistema si legge anche senza SD: la prova non simula la scheda tolta" || ok "senza SD il sistema non si legge più"
pulisci

echo "--- sistema passato in memoria, poi SD tolta"
prepara
if /tmp/ramroot "$ROOTLOOP" /run/sistema.sqfs > /tmp/rr.log 2>&1; then ok "ramroot: $(cat /tmp/rr.log)"; else ko "ramroot non riuscito: $(cat /tmp/rr.log)"; fi
cmp -s /run/sistema.sqfs /tmp/sys.sqfs && ok "la copia in memoria è uguale al sistema della SD" || ko "copia diversa dal sistema"
grep -q "/run/sistema.sqfs" /sys/block/${ROOTLOOP#/dev/}/loop/backing_file && ok "il loop legge dalla copia in memoria" || ko "il loop legge ancora: $(cat /sys/block/${ROOTLOOP#/dev/}/loop/backing_file)"
togli_sd
[ "$(leggi)" = "$(cat /tmp/md5)" ] && ok "SD tolta: il sistema si legge ancora per intero" || ko "SD tolta: il sistema non si legge più"
pulisci

echo "--- memoria scarsa: niente copia, il sistema resta sulla SD"
prepara
RAMROOT_MARGINE_MB=10000000 /tmp/ramroot "$ROOTLOOP" /run/sistema.sqfs > /tmp/rr.log 2>&1 && ko "copiato anche senza memoria" || ok "rifiutato ($(cat /tmp/rr.log))"
[ -e /run/sistema.sqfs ] && ko "è rimasta una copia in memoria" || ok "nessuna copia rimasta"
grep -q "arcade-system.squashfs" /sys/block/${ROOTLOOP#/dev/}/loop/backing_file && ok "il loop legge ancora la SD" || ko "loop cambiato"
[ "$(leggi)" = "$(cat /tmp/md5)" ] && ok "il sistema si legge" || ko "sistema rovinato"
pulisci

echo "--- loop in scrittura: rifiutato, niente copia"
prepara
cp /tmp/sys.sqfs /tmp/rw.img; RWLOOP=$(losetup -f) && losetup "$RWLOOP" /tmp/rw.img
/tmp/ramroot "$RWLOOP" /run/sistema.sqfs > /tmp/rr.log 2>&1 && ko "accettato un loop in scrittura" || ok "rifiutato ($(cat /tmp/rr.log))"
[ -e /run/sistema.sqfs ] && ko "è rimasta una copia" || ok "nessuna copia rimasta"
losetup -d "$RWLOOP"
pulisci

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
