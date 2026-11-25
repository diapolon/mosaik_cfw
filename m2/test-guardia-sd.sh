#!/bin/sh
# Guardia della SD (m2/guardia-sd.c, 23 novembre): scheda tolta e rimessa a console accesa. Un FAT
# su un loop fa da SD, montato come nell'initramfs; togliere la scheda è svuotare il file e far
# rileggere la dimensione al loop (0 settori, come il disco sparito), rimetterla è ridargli il
# contenuto. GUARDIA_TEST scrive «riavvio» o «spegnimento» invece di farlo. Differenza dal vero:
# qui la scheda rimessa è lo stesso dispositivo, sulla console è un disco nuovo con lo stesso nome.
# Serve --privileged (loop, mount):
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-guardia-sd.sh
apk add -q build-base dosfstools util-linux > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
gcc -std=gnu11 -O2 -Wall -Wextra -o /tmp/guardia /trimui/m2/guardia-sd.c || { echo "non compila"; exit 1; }
mkdir -p /tmp/led /tmp/run /tmp/mnt
for z in lr m rear; do : > /tmp/led/effect_rgb_hex_$z; : > /tmp/led/effect_$z; done
echo 0 > /tmp/usb
export GUARDIA_TEST=/tmp/azione GUARDIA_USB=/tmp/usb GUARDIA_LED=/tmp/led GUARDIA_RUN=/tmp/run GUARDIA_MNT=/tmp/mnt

nuova_sd() {   # SD con il FAT numero di serie $1, montata come nell'initramfs
    dd if=/dev/zero of=/tmp/sd.img bs=1M count=40 2> /dev/null
    mkfs.vfat -F 32 -i "$1" /tmp/sd.img > /dev/null
    LOOP=$(losetup -f) && losetup "$LOOP" /tmp/sd.img || { echo "serve --privileged"; exit 1; }
    D=${LOOP#/dev/}
    mount -t vfat -o rw,noatime,flush "$LOOP" /tmp/mnt
    mkdir -p /tmp/mnt/Arcade/Logs; echo prima > /tmp/mnt/Arcade/a.txt; sync
}
parti() {   # guardia con «secondi azione» scritti dal launcher in /run
    rm -f /tmp/azione /tmp/run/*; echo "$1" > /tmp/run/scheda-impostazioni
    /tmp/guardia "$D" "$LOOP" 60 & pid=$!; sleep 1
}
togli() { sync; cp /tmp/sd.img /tmp/sd.bak; truncate -s 0 /tmp/sd.img; losetup -c "$LOOP"; echo 3 > /proc/sys/vm/drop_caches; }
rimetti() { cat "${1:-/tmp/sd.bak}" > /tmp/sd.img; losetup -c "$LOOP"; }   # cp di BusyBox sostituirebbe il file sotto il loop
fine() { kill $pid 2> /dev/null; wait $pid 2> /dev/null; while umount /tmp/mnt 2> /dev/null; do :; done; losetup -d "$LOOP" 2> /dev/null; true; }
azione() { head -1 /tmp/azione 2> /dev/null || echo niente; }
uptime_s() { cut -d. -f1 /proc/uptime; }

echo "--- tolta e rimessa: si riprende senza riavvio"
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 2
grep -q FF0000 /tmp/led/effect_rgb_hex_m && grep -qx 6 /tmp/led/effect_m && ok "LED rossi lampeggianti" || ko "LED non rossi"
read sc az < /tmp/run/scheda-tolta 2> /dev/null
left=$(( ${sc:-0} - $(uptime_s) ))
[ "$az" = spegni ] && [ $left -ge 25 ] && [ $left -le 31 ] && ok "segno per il launcher: spegni fra $left s" || ko "segno: '$sc $az' (fra $left s)"
{ echo durante > /tmp/mnt/Arcade/durante.txt; sync; } 2> /dev/null   # come il launcher che scrive a scheda tolta
rimetti; sleep 3
[ "$(azione)" = niente ] && ok "nessun riavvio" || ko "rimessa: $(azione)"
[ -e /tmp/run/scheda-tolta ] && ko "segno della scheda tolta rimasto" || ok "segno della scheda tolta tolto"
[ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 1 ] && ok "contatore delle schede rimesse: 1" || ko "contatore: $(cat /tmp/run/scheda-rimessa 2> /dev/null)"
[ "$(grep -c ' /tmp/mnt ' /proc/mounts)" = 1 ] && ok "un solo aggancio in /tmp/mnt" || ko "agganci: $(grep -c ' /tmp/mnt ' /proc/mounts)"
[ "$(cat /tmp/mnt/Arcade/a.txt 2> /dev/null)" = prima ] && ok "i file di prima si leggono" || ko "file di prima non leggibili"
grep -q "fsck.fat -a" /tmp/mnt/Arcade/Logs/scheda-tolta.txt 2> /dev/null && grep -q rimontata /tmp/mnt/Arcade/Logs/scheda-tolta.txt && ok "registro scritto sulla scheda rimessa" || ko "registro: $(cat /tmp/mnt/Arcade/Logs/scheda-tolta.txt 2> /dev/null)"
echo dopo > /tmp/mnt/Arcade/b.txt && sync
echo "--- di nuovo tolta e rimessa: la guardia continua"
togli; sleep 2; [ -e /tmp/run/scheda-tolta ] && ok "vista di nuovo tolta" || ko "seconda volta non vista"
rimetti; sleep 3
[ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 2 ] && [ "$(azione)" = niente ] && ok "seconda volta: rimontata (contatore 2)" || ko "seconda volta: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
kill $pid 2> /dev/null; wait $pid 2> /dev/null
umount /tmp/mnt; mount -t vfat -o ro "$LOOP" /tmp/mnt
[ "$(cat /tmp/mnt/Arcade/b.txt 2> /dev/null)" = dopo ] && ok "scritto dopo il rimontaggio: è davvero sulla scheda" || ko "file scritto dopo il rimontaggio perso"
fine

echo "--- rimessa una scheda diversa"
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 2
dd if=/dev/zero of=/tmp/altra.img bs=1M count=40 2> /dev/null; mkfs.vfat -F 32 -i 5678CDEF /tmp/altra.img > /dev/null
rimetti /tmp/altra.img; sleep 3
[ "$(azione)" = riavvio ] && ok "scheda diversa: riavvio" || ko "scheda diversa: $(azione)"
fine
echo "--- stessa scheda, passo non riuscito: registro salvato prima del riavvio"
printf '#!/bin/sh\nexit 4\n' > /tmp/fsck-rotto; chmod +x /tmp/fsck-rotto
export GUARDIA_FSCK=/tmp/fsck-rotto; nuova_sd 1234ABCD; parti "30 spegni"; unset GUARDIA_FSCK; togli; sleep 2; rimetti; sleep 3
[ "$(azione)" = riavvio ] && ok "fsck non riuscito: riavvio" || ko "fsck non riuscito: $(azione)"
kill $pid 2> /dev/null; wait $pid 2> /dev/null; umount /tmp/mnt 2> /dev/null; mount -t vfat -o ro "$LOOP" /tmp/mnt
R=/tmp/mnt/Arcade/Logs/scheda-tolta.txt
grep -q "fsck.fat -a: non riuscito" $R 2> /dev/null && grep -q "passo non riuscito" $R && ok "registro sulla scheda anche col passo non riuscito" || ko "registro perso: $(cat $R 2> /dev/null)"
grep -q "messaggi del kernel" $R 2> /dev/null && ok "con i messaggi del kernel" || ko "senza i messaggi del kernel"
grep -q guardia-registro /proc/mounts && ko "aggancio di servizio rimasto" || ok "aggancio di servizio tolto"
fine
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 2; rimetti /tmp/altra.img; sleep 3
umount /tmp/mnt 2> /dev/null; mount -t vfat -o ro "$LOOP" /tmp/mnt
[ -e /tmp/mnt/Arcade ] && ko "scritto sulla scheda diversa" || ok "sulla scheda diversa non si scrive"
fine
echo 1 > /tmp/usb
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 2; rimetti /tmp/altra.img; sleep 3
[ "$(azione)" = spegnimento ] && ok "col caricatore: spegnimento invece del riavvio" || ko "col caricatore: $(azione)"
fine; echo 0 > /tmp/usb

echo "--- non rimessa, spegni dopo 5 s"
nuova_sd 1234ABCD; parti "5 spegni"; togli; sleep 3
[ -f /tmp/azione ] && ko "spenta prima del tempo" || ok "nei primi secondi aspetta"
sleep 4
[ "$(azione)" = spegnimento ] && ok "dopo 5 s: spegnimento" || ko "non rimessa: $(azione)"
fine

echo "--- non rimessa, sospendi dopo 5 s"
GUARDIA_RISERVA=3; export GUARDIA_RISERVA
nuova_sd 1234ABCD; parti "5 sospendi"; togli; sleep 6
[ "$(cut -d' ' -f2 /tmp/run/scheda-tolta 2> /dev/null)" = sospendi ] && [ ! -f /tmp/azione ] && ok "alla scadenza la sospensione tocca al launcher" || ko "sospendi: segno '$(cat /tmp/run/scheda-tolta 2> /dev/null)', $(azione)"
sleep 4
[ "$(azione)" = spegnimento ] && ok "nessuno ha sospeso: spegnimento di riserva" || ko "riserva: $(azione)"
fine; unset GUARDIA_RISERVA

echo "--- pulsante Spegni ora"
nuova_sd 1234ABCD; parti "60 spegni"; togli; sleep 2; : > /tmp/run/scheda-spegni-ora; sleep 1
[ "$(azione)" = spegnimento ] && ok "spegnimento chiesto: subito" || ko "spegni ora: $(azione)"
fine

echo "--- senza impostazioni del launcher"
nuova_sd 1234ABCD; parti "x"; rm -f /tmp/run/scheda-impostazioni; togli; sleep 2
read sc az < /tmp/run/scheda-tolta 2> /dev/null
left=$(( ${sc:-0} - $(uptime_s) ))
[ "$az" = spegni ] && [ $left -ge 55 ] && ok "valori di partenza: spegni fra $left s" || ko "senza impostazioni: '$sc $az'"
fine

echo "--- interruzione di un attimo"
nuova_sd 1234ABCD; parti "30 spegni"
sync; cp /tmp/sd.img /tmp/sd.bak; truncate -s 0 /tmp/sd.img; losetup -c "$LOOP"; sleep 0.2; rimetti; sleep 3
[ -f /tmp/azione ] && ko "un attimo senza disco fa $(azione)" || ok "meno di un secondo: niente"
[ -e /tmp/run/scheda-tolta ] && ko "segno lasciato" || ok "nessun segno per il launcher"
fine

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
