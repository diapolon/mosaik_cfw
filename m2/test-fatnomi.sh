#!/bin/sh
# Prova di fatnomi su un FAT32 vero, montato nel container (serve --privileged per il loop):
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-fatnomi.sh
# I nomi corti doppi si creano come li lascia macOS: stesso nome corto, nomi lunghi giusti
# (con la loro somma di controllo aggiornata).
set -e
apk add -q build-base linux-headers dosfstools python3 >/dev/null
W=/tmp/fatnomi; rm -rf $W; mkdir -p $W/m; cd $W
gcc -O2 -Wall -Wextra -o fatnomi /trimui/m2/fatnomi.c
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; umount $W/m 2>/dev/null || true; exit 1; }

# rende uguali due nomi corti nell'immagine: $1 immagine, $2 nome da cambiare, $3 nome nuovo
# (11 caratteri ciascuno, come sul disco)
dup() {
    python3 -I - "$@" <<'P'
import sys
img, old, new = sys.argv[1], sys.argv[2].encode(), sys.argv[3].encode()
data = bytearray(open(img, 'rb').read())
def csum(n):
    s = 0
    for c in n: s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s
found = 0
for off in range(0, len(data), 32):
    e = data[off:off + 32]
    if e[:11] == old and e[11] != 0x0F:
        data[off:off + 11] = new
        p = off - 32
        while p >= 0 and data[p + 11] == 0x0F and data[p] != 0xE5:
            data[p + 13] = csum(new)
            p -= 32
        found += 1
assert found == 1, f"{old} trovato {found} volte"
open(img, 'wb').write(data)
P
}

truncate -s 64M sd.img
mkfs.vfat -F 32 -n MOSAIK sd.img >/dev/null
mount -o loop -t vfat sd.img m
mkdir -p m/Linux/Gioco/stages/aether_plaza m/Linux/Gioco/stages/aether_rooftop m/Linux/Gioco/stages/ironworks_foundry
echo tetto > m/Linux/Gioco/stages/aether_rooftop/mappa.bin
echo piazza > m/Linux/Gioco/stages/aether_plaza/mappa.bin
mkdir -p m/Saves/fbneo
echo uno > "m/Saves/fbneo/Art of Fighting.state"
echo due > "m/Saves/fbneo/Art of Fighting 2.state"
echo tre > m/Saves/fbneo/a.txt
umount m
dup sd.img "AETHER~1   " "AETHER~2   "
dup sd.img "ARTOFF~1STA" "ARTOFF~2STA"
fsck.fat -n sd.img > fsck-prima.txt 2>&1 || true
[ "$(grep -c 'Duplicate directory entry' fsck-prima.txt)" = 2 ] && ok "fsck vede i due doppioni" || { cat fsck-prima.txt; ko "doppioni non creati"; }

echo "--- riparazione"
mount -o loop -t vfat sd.img m
rc=0; ./fatnomi $W/m > uscita.txt || rc=$?
cat uscita.txt | sed 's/^/    /'
[ $rc = 0 ] && ok "esito 0" || ko "esito $rc"
[ "$(grep -c '^riparato:' uscita.txt)" = 2 ] && ok "due file riparati" || ko "riparazioni: $(cat uscita.txt)"
[ "$(cat m/Linux/Gioco/stages/aether_rooftop/mappa.bin)" = tetto ] && ok "cartella rinominata col suo nome e contenuto" || ko "cartella cambiata"
[ "$(cat m/Linux/Gioco/stages/aether_plaza/mappa.bin)" = piazza ] && ok "l'altra cartella intatta" || ko "altra cartella cambiata"
[ "$(cat "m/Saves/fbneo/Art of Fighting.state")$(cat "m/Saves/fbneo/Art of Fighting 2.state")" = unodue ] && ok "salvataggi intatti" || ko "salvataggi cambiati"
[ "$(ls m/Linux/Gioco/stages | wc -l)" = 3 ] && ok "nessun file provvisorio rimasto" || ko "file in più: $(ls m/Linux/Gioco/stages)"
./fatnomi $W/m > uscita2.txt && [ ! -s uscita2.txt ] && ok "seconda passata: niente da fare" || ko "seconda passata: $(cat uscita2.txt)"
umount m
fsck.fat -n sd.img > fsck-dopo.txt 2>&1 && ok "fsck pulito dopo la riparazione" || { cat fsck-dopo.txt; ko "fsck dopo la riparazione"; }

echo "--- nome lungo uguale al nome corto dell'altro"
# b.txt prende il nome corto A.TXT di a.txt: "a.txt" indicherebbe tutti e due, si rinomina b.txt
mount -o loop -t vfat sd.img m; echo quattro > m/Saves/fbneo/b.txt; umount m
dup sd.img "B       TXT" "A       TXT"
mount -o loop -t vfat sd.img m
rc=0; ./fatnomi $W/m > uscita3.txt || rc=$?
sed 's/^/    /' uscita3.txt
[ $rc = 0 ] && grep -q '^riparato: /Saves/fbneo/b.txt' uscita3.txt && ok "riparato b.txt, esito 0" || ko "esito $rc: $(cat uscita3.txt)"
[ "$(cat m/Saves/fbneo/a.txt)$(cat m/Saves/fbneo/b.txt)" = trequattro ] && ok "a.txt e b.txt intatti" || ko "a.txt o b.txt cambiati"
[ "$(ls m/Saves/fbneo | grep -c provvisorio)" = 0 ] && ok "nessun file provvisorio" || ko "file provvisorio rimasto"
umount m
fsck.fat -n sd.img > fsck-ab.txt 2>&1 && ok "fsck pulito" || { cat fsck-ab.txt; ko "fsck dopo a.txt e b.txt"; }

echo "--- doppione vero, senza nomi lunghi"
# C.TXT e D.TXT maiuscoli non hanno nome lungo: D.TXT diventa un secondo C.TXT
mount -o loop -t vfat sd.img m; mkdir m/vero; echo c > m/vero/C.TXT; echo d > m/vero/D.TXT; umount m
dup sd.img "D       TXT" "C       TXT"
mount -o loop -t vfat sd.img m
rc=0; ./fatnomi $W/m > uscita4.txt || rc=$?
sed 's/^/    /' uscita4.txt
[ $rc = 1 ] && grep -q 'lasciato: /vero/C.TXT' uscita4.txt && ok "segnalato e lasciato, esito 1" || ko "esito $rc: $(cat uscita4.txt)"
[ "$(ls m/vero | grep -c '^C.TXT$')" = 2 ] && ok "entrambe le voci ancora lì" || ko "voci: $(ls m/vero)"
umount m

echo "--- avvio (blocco vero di system-init)"
# scheda nuova con un doppione; fatnomi finto che conta le chiamate e usa quello vero
truncate -s 64M boot.img; mkfs.vfat -F 32 boot.img >/dev/null
mount -o loop -t vfat boot.img m; mkdir -p m/Arcade/System m/Arcade/Logs m/g/aether_plaza m/g/aether_rooftop m/.fseventsd; umount m
dup boot.img "AETHER~1   " "AETHER~2   "
mount -o loop -t vfat boot.img m
mkdir -p bin; cat > bin/fatnomi <<F
#!/bin/sh
echo x >> $W/chiamate
exec $W/fatnomi "\$@"
F
chmod +x bin/fatnomi
sed -n '/^# >>> nomi corti dopo il Mac/,/^# <<< nomi corti dopo il Mac/p' /trimui/m2/system-init > blocco.sh
[ "$(wc -l < blocco.sh)" -gt 8 ] || ko "blocco non trovato in system-init"
boot() { (set +e; PATH=$W/bin:$PATH ROOT=$W/m/Arcade SD=$W/m; step() { echo "$*" >> $W/passi; }; . ./blocco.sh); }
: > chiamate; : > passi
boot; boot
[ "$(wc -l < chiamate)" = 2 ] && ok "controllo a ogni avvio" || ko "chiamate: $(wc -l < chiamate)"
grep -q 'riparato: /g/aether_rooftop' m/Arcade/Logs/nomi-corti.txt && ok "doppione riparato al primo avvio" || ko "nomi-corti.txt: $(cat m/Arcade/Logs/nomi-corti.txt)"
[ "$(grep -c '^===' m/Arcade/Logs/nomi-corti.txt)" = 1 ] && ok "senza doppioni il registro non cresce" || ko "registro scritto anche senza doppioni"
grep -q '^nomi corti controllati in [0-9.]* s: esito 0, riparati 1$' passi && ok "passo con il tempo: $(head -1 passi)" || ko "passi: $(cat passi)"
[ -d m/.fseventsd ] && ok "cartelle di macOS lasciate" || ko "cartelle di macOS toccate"
umount m
fsck.fat -n boot.img > fsck-boot.txt 2>&1 && ok "fsck pulito" || { cat fsck-boot.txt; ko "fsck dopo gli avvii"; }

echo "--- tempo su una scheda con molti file"
truncate -s 512M grande.img; mkfs.vfat -F 32 grande.img >/dev/null
mount -o loop -t vfat grande.img m
mkdir -p m/Arcade/Roms/FBNEO m/Arcade/Roms/MAME2003P m/Arcade/Media
i=0; while [ $i -lt 4000 ]; do : > m/Arcade/Roms/FBNEO/g$i.zip; : > "m/Arcade/Media/Gioco numero $i.png"; i=$((i + 1)); done
i=0; while [ $i -lt 300 ]; do mkdir -p "m/Arcade/Linux/gioco $i/dati"; : > "m/Arcade/Linux/gioco $i/dati/livello.bin"; i=$((i + 1)); done
umount m; mount -o loop -t vfat grande.img m   # cache dei nomi vuota, come all'avvio
t0=$(cut -d' ' -f1 /proc/uptime); ./fatnomi $W/m > /dev/null; t1=$(cut -d' ' -f1 /proc/uptime)
ok "8.900 voci e 600 cartelle in $(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.2f", b-a}') s (immagine in RAM: sulla SD è più lento)"
umount m

echo "--- cartella non FAT"
./fatnomi /tmp > /dev/null 2>&1 && ko "su una cartella non FAT dovrebbe fermarsi" || ok "su una cartella non FAT si ferma"
echo "tutte le prove passate"
