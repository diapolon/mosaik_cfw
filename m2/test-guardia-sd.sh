#!/bin/sh
# Guardia della SD (m2/guardia-sd.c): scheda tolta e rimessa a console accesa. Come sul kernel della
# console, la scheda rimessa è un disco nuovo: ogni scheda è un FAT su un suo loop, e PARTIZIONE è
# un collegamento che dopo il reinserimento punta al loop nuovo. Togliere la scheda è svuotare il
# file del loop vecchio (il vecchio aggancio non legge più niente) e mettere a zero la dimensione
# nel /sys/block finto (GUARDIA_SYS); rimetterla è un loop nuovo con il contenuto, e nel /sys finto
# il disco ha il CID della scheda e la partizione un numero di dispositivo nuovo.
# GUARDIA_TEST scrive «riavvio» o «spegnimento» invece di farlo. Serve --privileged (loop, mount):
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-guardia-sd.sh
apk add -q build-base dosfstools util-linux > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
gcc -std=gnu11 -O2 -Wall -Wextra -o /tmp/guardia /trimui/m2/guardia-sd.c || { echo "non compila"; exit 1; }
mkdir -p /tmp/led /tmp/run /tmp/mnt /tmp/vedi
for z in lr m rear; do : > /tmp/led/effect_rgb_hex_$z; : > /tmp/led/effect_$z; done
echo 0 > /tmp/usb
export GUARDIA_TEST=/tmp/azione GUARDIA_USB=/tmp/usb GUARDIA_LED=/tmp/led GUARDIA_RUN=/tmp/run GUARDIA_MNT=/tmp/mnt GUARDIA_SYS=/tmp/sys
LOOPS=; N=0

disco() {   # /sys/block/sd finto: dimensione $1, CID $2, partizione col dispositivo 179:$N
    rm -rf /tmp/sys/sd; mkdir -p /tmp/sys/sd/device /tmp/sys/sd/part
    echo "$2" > /tmp/sys/sd/device/cid; echo "179:$N" > /tmp/sys/sd/part/dev; echo "$1" > /tmp/sys/sd/size
}
attacca() {   # loop nuovo sull'immagine $1, collegato come /tmp/part
    L=$(losetup -f) && losetup "$L" "$1" || { echo "serve --privileged"; exit 1; }
    LOOPS="$LOOPS $L"; ln -sfn "$L" /tmp/part
}
nuova_sd() {   # scheda A: FAT con numero di serie $1, montata come nell'initramfs
    dd if=/dev/zero of=/tmp/a.img bs=1M count=40 2> /dev/null
    mkfs.vfat -F 32 -i "$1" /tmp/a.img > /dev/null
    attacca /tmp/a.img; disco 81920 cid-a
    mount -t vfat -o rw,noatime,flush /tmp/part /tmp/mnt
    mkdir -p /tmp/mnt/Arcade/Logs; echo prima > /tmp/mnt/Arcade/a.txt; sync
}
parti() {   # guardia con «secondi azione» scritti dal launcher in /run
    rm -f /tmp/azione /tmp/run/*; echo "$1" > /tmp/run/scheda-impostazioni
    /tmp/guardia sd /tmp/part 60 & pid=$!; sleep 1
}
togli() {   # il disco sparisce; quello che resta in memoria del vecchio aggancio non va più da nessuna parte
    sync; n=$(readlink /tmp/part); cp "$(losetup -nO BACK-FILE "$n")" /tmp/sd.bak
    truncate -s 0 "$(losetup -nO BACK-FILE "$n")"; losetup -c "$n"; echo 0 > /tmp/sys/sd/size
    echo 3 > /proc/sys/vm/drop_caches
}
rimetti() {   # disco nuovo: immagine $1 (di partenza la copia della scheda tolta), CID $2 (cid-a)
    N=$((N + 1)); i=/tmp/r$N.img; cp "${1:-/tmp/sd.bak}" "$i"; attacca "$i"; disco 81920 "${2:-cid-a}"
}
leggi() {   # file $1 sulla scheda attuale, letto da un aggancio a parte (rw: stesso superblocco se è montata)
    sync; mount -t vfat /tmp/part /tmp/vedi 2> /dev/null; cat "/tmp/vedi/$1" 2> /dev/null; umount /tmp/vedi 2> /dev/null
}
fine() {
    kill $pid 2> /dev/null; wait $pid 2> /dev/null
    while umount /tmp/mnt 2> /dev/null; do :; done; umount /tmp/vedi 2> /dev/null
    for l in $LOOPS; do losetup -d "$l" 2> /dev/null; done; LOOPS=; rm -f /tmp/r*.img; true
}
azione() { head -1 /tmp/azione 2> /dev/null || echo niente; }
uptime_s() { cut -d. -f1 /proc/uptime; }

echo "--- tolta e rimessa: si riprende senza riavvio, sul disco nuovo"
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
echo dopo > /tmp/mnt/Arcade/b.txt && sync
[ "$(leggi Arcade/b.txt)" = dopo ] && ok "scritto dopo il rimontaggio: è sulla scheda rimessa" || ko "file scritto dopo il rimontaggio non sulla scheda rimessa"
leggi Arcade/Logs/scheda-tolta.txt | grep -q "fsck.fat -a" && leggi Arcade/Logs/scheda-tolta.txt | grep -q rimontata && ok "registro scritto sulla scheda rimessa" || ko "registro: $(leggi Arcade/Logs/scheda-tolta.txt)"
echo "--- di nuovo tolta e rimessa: la guardia continua"
togli; sleep 2; [ -e /tmp/run/scheda-tolta ] && ok "vista di nuovo tolta" || ko "seconda volta non vista"
rimetti; sleep 3
[ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 2 ] && [ "$(azione)" = niente ] && ok "seconda volta: rimontata (contatore 2)" || ko "seconda volta: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
echo ancora > /tmp/mnt/Arcade/c.txt && sync
[ "$(leggi Arcade/c.txt)" = ancora ] && [ "$(leggi Arcade/b.txt)" = dopo ] && ok "seconda volta: scritture sulla scheda, quelle di prima ci sono" || ko "seconda volta: scritture perse"
fine

echo "--- un attimo fuori: il vecchio aggancio è morto, si rimonta lo stesso"
for come in "0,3 s senza disco" "fra due controlli (solo il numero del dispositivo cambia)"; do
    nuova_sd 1234ABCD; parti "30 spegni"
    if [ "$come" = "0,3 s senza disco" ]; then togli; sleep 0.3; rimetti
    else   # il disco non si vede mai assente: loop vecchio svuotato, loop nuovo, dispositivo nuovo
        sync; n=$(readlink /tmp/part); cp "$(losetup -nO BACK-FILE "$n")" /tmp/sd.bak
        truncate -s 0 "$(losetup -nO BACK-FILE "$n")"; losetup -c "$n"; echo 3 > /proc/sys/vm/drop_caches
        N=$((N + 1)); cp /tmp/sd.bak /tmp/r$N.img; attacca /tmp/r$N.img; echo "179:$N" > /tmp/sys/sd/part/dev
    fi
    sleep 4
    [ "$(azione)" = niente ] && [ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 1 ] && ok "$come: rimontata" || ko "$come: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
    echo dopo > /tmp/mnt/Arcade/b.txt 2> /dev/null; sync
    [ "$(leggi Arcade/b.txt)" = dopo ] && ok "$come: i salvataggi dopo arrivano sulla scheda" || ko "$come: scritture dopo perse"
    [ -e /tmp/run/scheda-tolta ] && ko "$come: segno lasciato" || ok "$come: nessun segno rimasto"
    fine
done

echo "--- errore di lettura della dimensione del disco: la scheda è dentro"
nuova_sd 1234ABCD; parti "30 spegni"
rm /tmp/sys/sd/size; mkdir /tmp/sys/sd/size; sleep 1; rmdir /tmp/sys/sd/size; echo 81920 > /tmp/sys/sd/size; sleep 3
[ ! -e /tmp/run/scheda-rimessa ] && [ "$(azione)" = niente ] && ok "nessun controllo né rimontaggio della scheda in uso" || ko "scheda in uso presa per tolta: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
grep -q " /tmp/mnt " /proc/mounts && [ "$(cat /tmp/mnt/Arcade/a.txt 2> /dev/null)" = prima ] && ok "aggancio di prima ancora al suo posto" || ko "aggancio cambiato"
fine

echo "--- rimessa una scheda diversa"
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 2
dd if=/dev/zero of=/tmp/altra.img bs=1M count=40 2> /dev/null; mkfs.vfat -F 32 -i 5678CDEF /tmp/altra.img > /dev/null
rimetti /tmp/altra.img cid-b; sleep 3
[ "$(azione)" = riavvio ] && ok "numero di serie diverso: riavvio" || ko "scheda diversa: $(azione)"
fine
echo 1 > /tmp/usb
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 2; rimetti /tmp/altra.img cid-b; sleep 3
[ "$(azione)" = spegnimento ] && ok "col caricatore: spegnimento invece del riavvio" || ko "col caricatore: $(azione)"
fine; echo 0 > /tmp/usb
echo "--- un'altra scheda scritta con la stessa immagine (stesso numero di serie del FAT)"
nuova_sd 1234ABCD; cp /tmp/a.img /tmp/gemella.img; parti "30 spegni"; togli; sleep 2
rimetti /tmp/gemella.img cid-b; sleep 3
[ "$(azione)" = riavvio ] && ok "CID diverso: riavvio" || ko "scheda gemella presa per la stessa: $(azione)"
[ -z "$(leggi Arcade/Logs/scheda-tolta.txt)" ] && ok "sulla scheda gemella non si scrive" || ko "scritto sulla scheda gemella"
fine

echo "--- stessa scheda, passo non riuscito"
export GUARDIA_OPZIONI=opzione_che_non_esiste
nuova_sd 1234ABCD; parti "30 spegni"; unset GUARDIA_OPZIONI; togli; sleep 2; rimetti; sleep 3
[ "$(azione)" = riavvio ] && ok "rimontaggio non riuscito: riavvio" || ko "rimontaggio non riuscito: $(azione)"
[ -e /tmp/run/scheda-tolta ] && ok "il segno resta fino al riavvio (nessuno riparte)" || ko "segno tolto prima del riavvio"
R=$(leggi Arcade/Logs/scheda-tolta.txt)
echo "$R" | grep -q "rimontaggio non riuscito" && echo "$R" | grep -q "passo non riuscito" && ok "registro sulla scheda anche col passo non riuscito" || ko "registro perso: $R"
echo "$R" | grep -q "messaggi del kernel" && ok "con i messaggi del kernel" || ko "senza i messaggi del kernel"
grep -q guardia-registro /proc/mounts && ko "aggancio di servizio rimasto" || ok "aggancio di servizio tolto"
fine
printf '#!/bin/sh\nexit 4\n' > /tmp/fsck-rotto; chmod +x /tmp/fsck-rotto
export GUARDIA_FSCK=/tmp/fsck-rotto
nuova_sd 1234ABCD; parti "30 spegni"; unset GUARDIA_FSCK; togli; sleep 2; rimetti; sleep 3
[ "$(azione)" = riavvio ] && ok "fsck non riuscito: riavvio" || ko "fsck non riuscito: $(azione)"
[ -z "$(leggi Arcade/Logs/scheda-tolta.txt)" ] && ok "FAT non riparato: niente scritto" || ko "scritto su un FAT non riparato"
fine
echo "/sd/Arcade/System/arcade-system.squashfs" > /tmp/radice; export GUARDIA_RADICE=/tmp/radice
nuova_sd 1234ABCD; parti "30 spegni"; unset GUARDIA_RADICE; togli; sleep 2; rimetti; sleep 3
[ "$(azione)" = riavvio ] && ok "sistema non in memoria: riavvio invece del rimontaggio" || ko "sistema sulla scheda tolta: $(azione)"
[ -z "$(cat /tmp/mnt/Arcade/a.txt 2> /dev/null)" ] && ok "la scheda non è stata rimontata" || ko "rimontata col sistema sulla scheda tolta"
fine

echo "--- controllo della scheda lungo o senza memoria: niente riavvio, lo rifà il prossimo avvio"
printf '#!/bin/sh\n: > /tmp/fsck-chiamato\nsleep 5\nexit 0\n' > /tmp/fsck-lungo; chmod +x /tmp/fsck-lungo
export GUARDIA_FSCK=/tmp/fsck-lungo GUARDIA_FSCK_SECS=1
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 1; rimetti; sleep 4
[ "$(azione)" = niente ] && [ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 1 ] && ok "controllo oltre il tempo: rimontata senza riavvio" || ko "controllo lungo: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
[ "$(cat /tmp/run/fat-da-controllare 2> /dev/null)" = /tmp/part ] && ok "segno per il controllo al prossimo avvio" || ko "nessun segno per il prossimo avvio"
fine
rm -f /tmp/fsck-chiamato; export GUARDIA_MEMORIA_MB=40
nuova_sd 1234ABCD; parti "30 spegni"; togli; sleep 1; rimetti; sleep 3
[ ! -e /tmp/fsck-chiamato ] && [ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 1 ] && [ "$(azione)" = niente ] && ok "poca memoria: niente controllo ora, rimontata" || ko "poca memoria: fsck $([ -e /tmp/fsck-chiamato ] && echo chiamato), contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
[ -e /tmp/run/fat-da-controllare ] && ok "poca memoria: controllo al prossimo avvio" || ko "poca memoria: nessun segno"
fine; unset GUARDIA_FSCK GUARDIA_FSCK_SECS GUARDIA_MEMORIA_MB

echo "--- system-init: il segno «in uso» rimesso allo spegnimento, il controllo lo fa l'initramfs"
sed -n '/^# >>> segno per il controllo/,/^# <<< segno per il controllo/p' /trimui/m2/system-init > /tmp/segno.sh
dd if=/dev/zero of=/tmp/s.img bs=1M count=40 2> /dev/null; mkfs.vfat -F 32 /tmp/s.img > /dev/null
stato() { dd if=/tmp/s.img bs=1 skip=65 count=1 2> /dev/null | od -An -tu1 | tr -d ' '; }   # come initramfs-init
[ $(( $(stato) & 1 )) = 0 ] && ok "FAT chiuso bene: nessun controllo" || ko "FAT nuovo già segnato"
mkdir -p /tmp/srun2; echo /tmp/s.img > /tmp/srun2/fat-da-controllare
( SD_RUN=/tmp/srun2; . /tmp/segno.sh; fat_segno_controllo )
[ $(( $(stato) & 1 )) = 1 ] && ok "dopo lo spegnimento: segno «in uso», l'initramfs controlla" || ko "segno non rimesso ($(stato))"
fsck.fat -n /tmp/s.img > /dev/null 2>&1; [ $? -le 1 ] && ok "FAT ancora valido" || ko "FAT rovinato"

echo "--- scheda infilata male: c'è ma non si legge"
dd if=/dev/zero of=/tmp/guasta.img bs=1M count=40 2> /dev/null   # niente FAT: il contatto che non regge
nuova_sd 1234ABCD; parti "60 spegni"; togli; sleep 1; rimetti /tmp/guasta.img; sleep 17
[ "$(azione)" = niente ] && ok "nessun riavvio (senza scheda la console partirebbe dal firmware interno)" || ko "infilata male: $(azione)"
[ -e /tmp/run/scheda-tolta ] && ok "per il launcher è ancora fuori" || ko "segno della scheda tolta sparito"
rimetti; sleep 5
[ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 1 ] && [ "$(azione)" = niente ] && ok "infilata bene: rimontata" || ko "dopo: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
fine
nuova_sd 1234ABCD; parti "6 spegni"; togli; sleep 1; rimetti /tmp/guasta.img; sleep 9
[ "$(azione)" = spegnimento ] && ok "infilata male e lasciata: alla scadenza si spegne, niente riavvio" || ko "lasciata: $(azione)"
fine

echo "--- tolta di nuovo durante il controllo"
printf '#!/bin/sh\nsleep 3\nexit 0\n' > /tmp/fsck-lento; chmod +x /tmp/fsck-lento
export GUARDIA_FSCK=/tmp/fsck-lento
nuova_sd 1234ABCD; parti "30 spegni"; unset GUARDIA_FSCK; togli; sleep 2; rimetti; sleep 1.5
togli; sleep 3
[ "$(azione)" = niente ] && ok "nessun riavvio" || ko "tolta durante il controllo: $(azione)"
[ "$(cut -d' ' -f2 /tmp/run/scheda-tolta 2> /dev/null)" = spegni ] && ok "di nuovo il conto alla rovescia" || ko "segno: '$(cat /tmp/run/scheda-tolta 2> /dev/null)'"
rimetti; sleep 5
[ "$(cat /tmp/run/scheda-rimessa 2> /dev/null)" = 1 ] && [ "$(azione)" = niente ] && ok "rimessa ancora: rimontata" || ko "dopo: contatore $(cat /tmp/run/scheda-rimessa 2> /dev/null), $(azione)"
fine

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
nuova_sd 1234ABCD; parti "60 spegni"; togli; sleep 2; rimetti; sleep 3
: > /tmp/run/scheda-spegni-ora   # tasto tenuto mentre si controllava la scheda: richiesta rimasta
togli; sleep 3
[ "$(azione)" = niente ] && ok "richiesta rimasta da prima: alla nuova estrazione non spegne" || ko "richiesta vecchia: $(azione)"
fine

echo "--- senza impostazioni del launcher"
nuova_sd 1234ABCD; parti "x"; rm -f /tmp/run/scheda-impostazioni; togli; sleep 2
read sc az < /tmp/run/scheda-tolta 2> /dev/null
left=$(( ${sc:-0} - $(uptime_s) ))
[ "$az" = spegni ] && [ $left -ge 55 ] && ok "valori di partenza: spegni fra $left s" || ko "senza impostazioni: '$sc $az'"
fine

echo "--- system-init a scheda tolta: nessun programma nuovo finché non torna"
sed -n '/^# >>> attesa della scheda/,/^# <<< attesa della scheda/p' /trimui/m2/system-init > /tmp/attesa.sh
mkdir -p /tmp/bin; printf '#!/bin/sh\necho "$@" > /tmp/fbmsg-visto\n' > /tmp/bin/fbmsg; chmod +x /tmp/bin/fbmsg
rm -rf /tmp/srun /tmp/fbmsg-visto /tmp/partito; mkdir -p /tmp/srun /tmp/ssys/sd; echo 81920 > /tmp/ssys/sd/size
cat > /tmp/ciclo.sh <<'X'
SDDISK=sd SD_RUN=/tmp/srun SD_SYS=/tmp/ssys PATH=/tmp/bin:$PATH
. /tmp/attesa.sh
sd_out_wait; echo partito > /tmp/partito
X
sh /tmp/ciclo.sh; [ -f /tmp/partito ] && ok "scheda dentro: il programma parte subito" || ko "scheda dentro: non parte"
rm -f /tmp/partito; : > /tmp/srun/scheda-tolta
sh /tmp/ciclo.sh & c=$!; sleep 2
[ ! -f /tmp/partito ] && ok "scheda tolta: il programma non parte" || ko "scheda tolta: partito lo stesso"
grep -q "SCHEDA TOLTA" /tmp/fbmsg-visto 2> /dev/null && ok "scheda tolta: messaggio sullo schermo" || ko "nessun messaggio"
rm -f /tmp/srun/scheda-tolta; sleep 1.5
[ -f /tmp/partito ] && ok "scheda rimontata: il programma parte" || ko "rimontata: non parte"
kill $c 2> /dev/null
rm -f /tmp/partito; echo 0 > /tmp/ssys/sd/size   # disco sparito, la guardia non ha ancora scritto il segno
sh /tmp/ciclo.sh & c=$!; sleep 2
[ ! -f /tmp/partito ] && ok "disco appena sparito, prima del segno: non parte" || ko "disco sparito: partito lo stesso"
echo 81920 > /tmp/ssys/sd/size; sleep 1.5; kill $c 2> /dev/null

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
