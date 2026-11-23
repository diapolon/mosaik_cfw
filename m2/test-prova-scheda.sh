#!/bin/sh
# Prova della scheda tolta a console accesa (m2/prova-scheda.c, 23 novembre): un loop con
# un'immagine FAT fa da SD; staccarlo è togliere la scheda, riattaccarlo è rimetterla. Il programma
# deve accorgersi delle due cose e, rimessa la scheda, scriverci Arcade/Logs/prova-scheda.txt.
# Serve --privileged (loop e mount):
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-prova-scheda.sh
apk add -q build-base dosfstools > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
gcc -std=gnu11 -O2 -Wall -Wextra -o /tmp/prova-scheda /trimui/m2/prova-scheda.c || { echo "non compila"; exit 1; }

dd if=/dev/zero of=/tmp/sd.img bs=1M count=40 2> /dev/null
mkfs.vfat /tmp/sd.img > /dev/null
mkdir -p /tmp/m && mount -o loop /tmp/sd.img /tmp/m && mkdir -p /tmp/m/Arcade/Logs && umount /tmp/m
LOOP=$(losetup -f) && losetup "$LOOP" /tmp/sd.img || { echo "serve --privileged"; exit 1; }
D=${LOOP#/dev/}
mkdir -p /run/prova-scheda
( cd / && exec /tmp/prova-scheda "$D" "$LOOP" ) > /dev/null 2>&1 &
pid=$!

echo "--- scheda tolta e rimessa"
sleep 1
losetup -d "$LOOP"
sleep 2
losetup "$LOOP" /tmp/sd.img
sleep 4
kill $pid 2> /dev/null
losetup -d "$LOOP" 2> /dev/null
mount -o loop /tmp/sd.img /tmp/m
L=/tmp/m/Arcade/Logs/prova-scheda.txt
if [ -f "$L" ]; then ok "registro scritto sulla scheda rimessa"; else ko "registro non scritto"; fi
grep -q "scheda TOLTA" "$L" 2> /dev/null && ok "si accorge della scheda tolta" || ko "scheda tolta non vista"
grep -q "scheda RIMESSA" "$L" 2> /dev/null && ok "si accorge della scheda rimessa" || ko "scheda rimessa non vista"
grep -q "mount di prova: riuscito" "$L" 2> /dev/null && ok "rimonta la scheda da sé" || ko "mount non riuscito"
grep -q "/run/prova-scheda/mnt" /proc/mounts && ko "la scheda resta montata" || ok "e poi la smonta"
umount /tmp/m

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; cat "$L" 2> /dev/null | head -20; exit 1; }
