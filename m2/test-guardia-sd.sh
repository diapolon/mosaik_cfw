#!/bin/sh
# Guardia della SD (m2/guardia-sd.c, 23 novembre): scheda tolta a console accesa. Un loop con
# un'immagine fa da SD; staccarlo è togliere la scheda. GUARDIA_TEST scrive «riavvio» o
# «spegnimento» invece di farlo. Serve --privileged (loop):
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-guardia-sd.sh
apk add -q build-base > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
gcc -std=gnu11 -O2 -Wall -Wextra -o /tmp/guardia /trimui/m2/guardia-sd.c || { echo "non compila"; exit 1; }
dd if=/dev/zero of=/tmp/sd.img bs=1M count=8 2> /dev/null
LOOP=$(losetup -f) && losetup "$LOOP" /tmp/sd.img || { echo "serve --privileged"; exit 1; }
D=${LOOP#/dev/}
mkdir -p /tmp/led; for z in lr m rear; do : > /tmp/led/effect_rgb_hex_$z; : > /tmp/led/effect_$z; done
echo 0 > /tmp/usb
export GUARDIA_TEST=/tmp/azione GUARDIA_USB=/tmp/usb GUARDIA_LED=/tmp/led
parti() { rm -f /tmp/azione; /tmp/guardia "$D" "$1" & pid=$!; sleep 1; }
fine() { kill $pid 2> /dev/null; wait $pid 2> /dev/null; losetup "$LOOP" /tmp/sd.img 2> /dev/null; true; }

echo "--- scheda tolta e rimessa"
parti 30; losetup -d "$LOOP"; sleep 2
grep -q FF0000 /tmp/led/effect_rgb_hex_m && grep -qx 6 /tmp/led/effect_m && ok "LED rossi lampeggianti" || ko "LED non rossi"
losetup "$LOOP" /tmp/sd.img; sleep 2
[ "$(cat /tmp/azione 2> /dev/null)" = riavvio ] && ok "rimessa: riavvio" || ko "rimessa: $(cat /tmp/azione 2> /dev/null || echo niente)"
fine

echo "--- rimessa con il caricatore collegato"
echo 1 > /tmp/usb
parti 30; losetup -d "$LOOP"; sleep 2; losetup "$LOOP" /tmp/sd.img; sleep 2
[ "$(cat /tmp/azione 2> /dev/null)" = spegnimento ] && ok "col caricatore: spegnimento invece del riavvio" || ko "col caricatore: $(cat /tmp/azione 2> /dev/null || echo niente)"
fine; echo 0 > /tmp/usb

echo "--- non rimessa"
parti 5; losetup -d "$LOOP"; sleep 4
[ -f /tmp/azione ] && ko "spenta prima del tempo" || ok "nei primi secondi aspetta"
sleep 4
[ "$(cat /tmp/azione 2> /dev/null)" = spegnimento ] && ok "dopo 5 s: spegnimento" || ko "non rimessa: $(cat /tmp/azione 2> /dev/null || echo niente)"
fine

echo "--- interruzione di un attimo"
parti 30; losetup -d "$LOOP"; sleep 0.2; losetup "$LOOP" /tmp/sd.img; sleep 3
[ -f /tmp/azione ] && ko "un attimo senza disco fa $(cat /tmp/azione)" || ok "meno di un secondo: niente"
fine
losetup -d "$LOOP" 2> /dev/null

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
