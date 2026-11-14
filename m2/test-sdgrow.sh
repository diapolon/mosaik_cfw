#!/bin/sh
# Prova di sdgrow su schede simulate (file sparsi), in un Linux arm64:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui alpine:3.20 sh /trimui/m2/test-sdgrow.sh
# Costruisce un'immagine come quella vera (GPT con -j 160, FAT preparato da fatprep.py) e la
# "scrive" su schede da 2,3 / 4 / 16 / 64 / 300 GB; poi controlla GPT, FAT e file.
set -e
apk add -q build-base linux-headers coreutils dosfstools mtools sgdisk python3 >/dev/null
W=/tmp/sdgrow-test; rm -rf $W; mkdir -p $W; cd $W
export MTOOLS_SKIP_CHECK=1
gcc -O2 -Wall -Wextra -o sdgrow /trimui/m2/sdgrow.c
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; exit 1; }

# FAT come nella costruzione vera
truncate -s 256G fat.img
mkfs.vfat -F 32 -s 64 -n ARCADE fat.img >/dev/null
python3 -I /trimui/m2/fatprep.py fat.img 2200
head -c 3000000 /dev/urandom > dati.bin
mmd -i fat.img ::Arcade
mcopy -i fat.img dati.bin ::Arcade/dati.bin
fsck.fat -n fat.img >/dev/null && ok "FAT preparato: fsck pulito" || ko "fsck sul FAT preparato"

mkimg() {  # $1 file immagine, $2 FAT
    rm -f $1; truncate -s 2300M $1
    S=2048
    sgdisk -o -j 160 -n 1:$((36*S)):+32M -c 1:boot -n 2:0:+1M -c 2:env -n 3:0:+1M -c 3:env-redund \
        -n 4:0:0 -t 4:0700 -c 4:boot-resource $1 >/dev/null
    START=$(sgdisk -i 4 $1 | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
    dd if=$2 of=$1 bs=512 seek=$START conv=notrunc,sparse status=none
}
mkimg base.img fat.img

check() {  # $1 immagine, $2 GB attesi (circa) del FAT
    sgdisk -v $1 | grep -qi "no problems" && ok "GPT valida" || { sgdisk -v $1; ko "GPT"; }
    END=$(sgdisk -i 4 $1 | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
    TOT=$(( $(stat -c %s $1) / 512 ))
    [ $END -eq $((TOT - 34)) ] && ok "partizione fino in fondo" || ko "fine partizione $END su $TOT"
    START=$(sgdisk -i 4 $1 | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
    mcopy -n -i "$1@@$((START * 512))" ::Arcade/dati.bin letto.bin && cmp -s letto.bin dati.bin && ok "file intatto" || ko "file"
    MB=$(python3 -c "import struct,sys;f=open(sys.argv[1],'rb');f.seek($START*512);b=f.read(512);print(struct.unpack_from('<I',b,32)[0]//2048)" $1)
    echo "  FAT: $MB MB"
    [ -n "$2" ] && { [ $MB -ge $(( $2 * 1000 )) ] && ok "FAT allargato" || ko "FAT di $MB MB, attesi ~$2 GB"; }
    # scrittura dopo la crescita, poi fsck (solo sulle schede piccole: estrarre 64 GB è lento)
    head -c 1000000 /dev/urandom > nuovo.bin
    mcopy -i "$1@@$((START * 512))" nuovo.bin ::nuovo.bin && ok "scrittura dopo la crescita"
    if [ $TOT -le $((20 * 1024 * 1024 * 2)) ]; then
        dd if=$1 of=part.img bs=1M skip=$((START / 2048)) conv=sparse status=none
        fsck.fat -n part.img > fsck.txt 2>&1 && ok "fsck pulito" || { cat fsck.txt; ko "fsck"; }
        grep -q "free" fsck.txt || true
        rm -f part.img
    fi
}

for size in 2300M 4G 16G 64G 300G; do
    echo "--- scheda da $size"
    cp --sparse=always base.img card.img
    truncate -s $size card.img
    set +e; ./sdgrow card.img -; rc=$?; set -e
    [ $rc -eq 0 ] && ok "allargata (uscita 0)" || ko "uscita $rc"
    case $size in 2300M) exp=2;; 4G) exp=3;; 16G) exp=15;; 64G) exp=63;; 300G) exp=255;; esac
    check card.img $exp
    set +e; ./sdgrow card.img - >/dev/null; rc=$?; set -e
    [ $rc -eq 1 ] && ok "seconda volta: niente da fare" || ko "seconda volta uscita $rc"
done

echo "--- dal sistema avviato: prova (-n) e sola GPT (-g), poi il FAT all'avvio dopo"
cp --sparse=always base.img card.img; truncate -s 16G card.img
cp --sparse=always card.img prima.img
set +e; out=$(./sdgrow -n card.img); rc=$?; set -e
[ $rc -eq 0 ] && ok "prova: si può allargare (0)" || ko "prova uscita $rc"
echo "$out" | grep -q "partizione ARCADE" && echo "$out" | grep -q "FAT:" && ok "prova: dice partizione e FAT" || ko "prova: $out"
cmp -s card.img prima.img && ok "prova: scheda non toccata" || ko "la prova ha scritto"
set +e; out=$(./sdgrow -g card.img); rc=$?; set -e
[ $rc -eq 3 ] && ok "sola GPT: riavvio necessario (3)" || ko "sola GPT uscita $rc: $out"
END=$(sgdisk -i 4 card.img | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
[ $END -eq $(( $(stat -c %s card.img) / 512 - 34 )) ] && ok "sola GPT: partizione fino in fondo" || ko "fine partizione $END"
START=$(sgdisk -i 4 card.img | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
cmp -s -n 1048576 card.img prima.img $((START * 512)) $((START * 512)) && ok "sola GPT: FAT non toccato" || ko "la sola GPT ha scritto nel FAT"
set +e; ./sdgrow -g card.img >/dev/null; rc=$?; set -e
[ $rc -eq 3 ] && ok "sola GPT di nuovo: FAT ancora da allargare (3)" || ko "seconda sola GPT uscita $rc"
set +e; ./sdgrow card.img - >/dev/null; rc=$?; set -e
[ $rc -eq 0 ] && ok "avvio dopo: FAT allargato (0)" || ko "avvio dopo uscita $rc"
check card.img 15
set +e; ./sdgrow -n card.img >/dev/null; rc=$?; set -e
[ $rc -eq 1 ] && ok "prova su scheda già allargata: niente da fare (1)" || ko "prova dopo uscita $rc"

echo "--- limite della tabella (FAT preparato per 8 GB) su scheda da 16G, con fsck"
truncate -s 8G fat8.img; mkfs.vfat -F 32 -s 64 -n ARCADE fat8.img >/dev/null
python3 -I /trimui/m2/fatprep.py fat8.img 2200 >/dev/null
mmd -i fat8.img ::Arcade; mcopy -i fat8.img dati.bin ::Arcade/dati.bin
mkimg card.img fat8.img; truncate -s 16G card.img
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 0 ] && ok "allargata fino al limite della tabella" || ko "uscita $rc"
check card.img 7

echo "--- FAT normale (non preparato) su scheda da 16G"
truncate -s 1900M plain.img; mkfs.vfat -F 32 -n ARCADE plain.img >/dev/null
mmd -i plain.img ::Arcade; mcopy -i plain.img dati.bin ::Arcade/dati.bin
mkimg card.img plain.img; truncate -s 16G card.img
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 0 ] && ok "solo GPT allargata, FAT lasciato com'era" || ko "uscita $rc"
check card.img ""

# copia settori da un'immagine all'altra: $1 da, $2 a, $3 primo settore, $4 quanti
copysec() { dd if=$1 of=$2 bs=512 skip=$3 seek=$3 count=$4 conv=notrunc status=none; }
ENT=$(python3 -c "import struct;f=open('base.img','rb');f.seek(512);print(struct.unpack_from('<Q',f.read(92),72)[0])")

echo "--- corrente mancata dopo la copia di riserva, voci principali a metà"
cp --sparse=always base.img ref.img; truncate -s 16G ref.img; ./sdgrow ref.img - >/dev/null
cp --sparse=always ref.img card.img
copysec base.img card.img $ENT 32
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 0 ] && ok "ricostruita dalla copia di riserva" || ko "uscita $rc"
dd if=card.img of=a.bin bs=512 count=$((ENT + 32)) status=none; dd if=ref.img of=b.bin bs=512 count=$((ENT + 32)) status=none
cmp -s a.bin b.bin && ok "GPT principale uguale a quella completa" || ko "GPT principale diversa"
check card.img 15

echo "--- corrente mancata con le voci nuove e l'intestazione vecchia"
cp --sparse=always ref.img card.img
copysec base.img card.img 1 1
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 0 ] && ok "ricostruita dalla copia di riserva" || ko "uscita $rc"
check card.img 15

echo "--- principale e copia di riserva rovinate: niente modifiche"
cp --sparse=always ref.img card.img
copysec base.img card.img $ENT 32
dd if=/dev/zero of=card.img bs=512 seek=$(( $(stat -c %s card.img) / 512 - 1 )) count=1 conv=notrunc status=none
before=$(md5sum card.img | cut -d" " -f1)
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 2 ] && [ "$(md5sum card.img | cut -d" " -f1)" = "$before" ] && ok "rifiutato, immagine intatta" || ko "uscita $rc"

echo "--- intestazione GPT già spostata in fondo, partizione rimasta piccola (issue #7, audit SDGROW)"
# sgdisk -e sposta in fondo la copia di riserva e il limite utile senza allargare la partizione 4,
# come può fare un programma per dischi: fino alla 0.9.58 sdgrow non allargava più la partizione e
# rispondeva "niente da fare" (FAT solo fino alla fine della partizione piccola)
cp --sparse=always base.img card.img; truncate -s 64G card.img
sgdisk -e card.img >/dev/null
# sgdisk lascia il limite a 192 settori dalla fine (array a 160): lo si porta dove lo mette sdgrow
# (fine meno 34), con il CRC dell'intestazione principale ricalcolato
python3 - card.img <<'PY'
import struct, sys, zlib
f = open(sys.argv[1], "r+b"); f.seek(0, 2); tot = f.tell() // 512
f.seek(512); h = bytearray(f.read(512)); hs = struct.unpack_from("<I", h, 12)[0]
struct.pack_into("<Q", h, 48, tot - 34); struct.pack_into("<I", h, 16, 0)
struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:hs])) & 0xffffffff)
f.seek(512); f.write(h); f.close()
PY
LU=$(python3 -c "import struct;f=open('card.img','rb');f.seek(512);print(struct.unpack_from('<Q',f.read(92),48)[0])")
END=$(sgdisk -i 4 card.img | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
TOT=$(( $(stat -c %s card.img) / 512 ))
[ $LU -ge $((TOT - 34)) ] && [ $END -lt $((TOT / 2)) ] && ok "preparata: limite utile in fondo ($LU), partizione piccola (fine $END)" || ko "preparazione: limite $LU, fine $END su $TOT"
cp --sparse=always card.img prima.img
set +e; out=$(./sdgrow -n card.img); rc=$?; set -e
cmp -s card.img prima.img && ok "prova: scheda non toccata" || ko "la prova ha scritto"
echo "  sdgrow -n: uscita $rc: $(echo "$out" | tr '\n' ' ')"
[ $rc -eq 0 ] && echo "$out" | grep -q "partizione ARCADE" && ok "prova: propone di allargare la partizione (0)" || ko "prova uscita $rc: $out"
# percorso dell'app (sistema avviato): -g solo la GPT, poi il FAT all'avvio dopo
cp --sparse=always card.img app.img
set +e; out=$(./sdgrow -g app.img); rc=$?; set -e
END3=$(sgdisk -i 4 app.img 2>/dev/null | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
[ $rc -eq 3 ] && [ $END3 -eq $((TOT - 34)) ] && ok "sola GPT (-g): partizione fino in fondo, riavvio necessario (3)" || ko "-g uscita $rc, fine $END3: $out"
START=$(sgdisk -i 4 app.img 2>/dev/null | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
cmp -s -n 1048576 app.img prima.img $((START * 512)) $((START * 512)) && ok "sola GPT (-g): FAT non toccato" || ko "-g ha scritto nel FAT"
set +e; ./sdgrow app.img - >/dev/null; rc=$?; set -e
[ $rc -eq 0 ] && ok "avvio dopo: FAT allargato (0)" || ko "avvio dopo uscita $rc"
check app.img 63
rm -f app.img
set +e; out=$(./sdgrow card.img -); rc=$?; set -e
END2=$(sgdisk -i 4 card.img 2>/dev/null | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
echo "  sdgrow: uscita $rc: $(echo "$out" | tr '\n' ' ')"
[ $END2 -eq $((TOT - 34)) ] && ok "partizione allargata fino in fondo (era il difetto della issue #7)" || ko "partizione ferma a $((END2 / 2048)) MB su $((TOT / 2048)) MB: \"niente da fare\" sbagliato"
[ $rc -eq 0 ] && ok "uscita 0" || ko "uscita $rc"
check card.img 63

echo "--- tabella allargata ma il kernel vede la partizione vecchia (esito 5, audit SDGROW B2)"
cp --sparse=always base.img card.img; truncate -s 16G card.img
set +e; ./sdgrow -g card.img >/dev/null; set -e
START=$(sgdisk -i 4 base.img | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
OLDEND=$(sgdisk -i 4 base.img | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
truncate -s $(( (OLDEND - START + 1) * 512 )) kpart.img   # la "partizione" come la vede il kernel: misura vecchia
set +e; out=$(./sdgrow card.img kpart.img); rc=$?; set -e
echo "$out" | grep -q "il kernel ne vede" && ok "dice le due misure ($(echo "$out" | head -1))" || ko "misure: $out"
[ $rc -eq 0 ] && ok "FAT allargato solo fino alla misura del kernel (0)" || ko "uscita $rc: $out"
MB=$(python3 -c "import struct,sys;f=open(sys.argv[1],'rb');f.seek($START*512);print(struct.unpack_from('<I',f.read(512),32)[0])" card.img)
[ $MB -le $((OLDEND - START + 1)) ] && ok "FAT mai oltre la partizione del kernel" || ko "FAT di $MB settori oltre $((OLDEND - START + 1))"
set +e; out=$(./sdgrow card.img kpart.img); rc=$?; set -e
[ $rc -eq 5 ] && echo "$out" | grep -q "non vede la partizione allargata" && ok "la volta dopo: esito 5, non \"niente da fare\"" || ko "uscita $rc: $out"
rm -f kpart.img

echo "--- tabella allargata, kernel che non la rilegge: il FAT vecchio si può montare per il registro? (SDG-R01)"
START=$(sgdisk -i 4 base.img | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
OLDEND=$(sgdisk -i 4 base.img | sed -n "s/^Last sector: \([0-9]*\).*/\1/p")
OLDN=$((OLDEND - START + 1))
dd if=base.img of=kpart.img bs=512 skip=$START count=$OLDN conv=sparse status=none   # partizione vecchia vista dal kernel
for caso in giusta spostata senza-inizio altrove piccola; do
    cp --sparse=always base.img card.img; truncate -s 16G card.img
    KS=$START
    case $caso in
        giusta|senza-inizio) cp --sparse=always kpart.img k.img ;;
        # stesso contenuto del primo settore (la copia del settore di avvio del FAT, 6 settori più
        # avanti) ma inizio diverso: va rifiutata, l'inizio si confronta come numero
        spostata) KS=$((START + 6)); dd if=base.img of=k.img bs=512 skip=$KS count=$((OLDN - 6)) conv=sparse status=none ;;
        altrove) truncate -s $((OLDN * 512)) k.img; dd if=/dev/zero of=k.img bs=512 count=1 conv=notrunc status=none ;;
        piccola) dd if=kpart.img of=k.img bs=512 count=$((OLDN / 2)) status=none ;;
    esac
    if [ $caso = spostata ]; then
        cmp -s -n 512 k.img kpart.img && ok "spostata: il primo settore è uguale a quello del FAT" || ko "spostata: primo settore diverso, la prova non serve"
    fi
    if [ $caso = senza-inizio ]; then
        set +e; out=$(./sdgrow card.img k.img 2>&1); rc=$?; set -e
    else
        set +e; out=$(SDGROW_PART_START=$KS ./sdgrow card.img k.img 2>&1); rc=$?; set -e
    fi
    case $caso in
        giusta) [ $rc -eq 3 ] && ok "partizione del kernel giusta: esito 3, registro possibile" || ko "giusta: uscita $rc: $out" ;;
        *) [ $rc -eq 6 ] && echo "$out" | grep -q "niente registro prima del riavvio" && ok "partizione del kernel $caso: esito 6, niente mount ($(echo "$out" | tail -1))" || ko "$caso: uscita $rc: $out" ;;
    esac
    rm -f k.img
done
rm -f kpart.img

echo "--- registro dell'initramfs (grow_log): testo e rotazione a 20 KB"
sed -n '/^grow_log() {/,/^}/p' /trimui/m2/initramfs-init > grow_log.sh
[ -s grow_log.sh ] || ko "grow_log non trovato in initramfs-init"
. ./grow_log.sh
rm -rf logs; GROW=3; echo "partizione ARCADE: 2229 -> 65465 MB" > /tmp/sdgrow.txt
grow_log logs
grep -q "esito 3 (" logs/sdgrow.txt && grep -q "2229 -> 65465" logs/sdgrow.txt && ok "esito 3 scritto con la risposta di sdgrow" || ko "registro: $(cat logs/sdgrow.txt)"
head -c 21000 /dev/zero | tr '\0' 'x' >> logs/sdgrow.txt
GROW=5; grow_log logs
[ -f logs/sdgrow.txt.old ] && [ $(wc -c < logs/sdgrow.txt) -lt 2000 ] && grep -q "esito 5" logs/sdgrow.txt && ok "oltre 20 KB: ruotato in sdgrow.txt.old, il nuovo esito registrato" || ko "rotazione"

echo "--- GPT più grande del disco (scheda che dichiara meno spazio): niente modifiche"
cp --sparse=always base.img card.img; truncate -s 16G card.img; ./sdgrow card.img - >/dev/null
truncate -s 8G card.img
before=$(md5sum card.img | cut -d" " -f1)
set +e; out=$(./sdgrow card.img - 2>&1); rc=$?; set -e
[ $rc -eq 2 ] && [ "$(md5sum card.img | cut -d" " -f1)" = "$before" ] && ok "rifiutato, immagine intatta ($out)" || ko "uscita $rc: $out"
set +e; ./sdgrow -n card.img >/dev/null 2>&1; rc=$?; set -e
[ $rc -eq 2 ] && ok "anche la prova lo dice (2)" || ko "prova uscita $rc"

echo "--- FAT più grande della sua partizione: niente modifiche, nemmeno alla GPT"
cp --sparse=always base.img card.img; truncate -s 16G card.img
START=$(sgdisk -i 4 card.img | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
python3 - card.img $START <<'PY'
import struct, sys
f = open(sys.argv[1], "r+b"); off = int(sys.argv[2]) * 512
f.seek(off); b = bytearray(f.read(512)); struct.pack_into("<I", b, 32, 6000000); f.seek(off); f.write(b)
PY
before=$(md5sum card.img | cut -d" " -f1)
set +e; out=$(./sdgrow card.img - 2>&1); rc=$?; set -e
[ $rc -eq 2 ] && [ "$(md5sum card.img | cut -d" " -f1)" = "$before" ] && ok "rifiutato prima di toccare la GPT ($out)" || ko "uscita $rc: $out"

echo "--- GPT principale da rifare dalla riserva ma FAT sbagliato: niente modifiche, nemmeno alla GPT"
cp --sparse=always ref.img card.img
copysec base.img card.img $ENT 32
START=$(sgdisk -i 4 ref.img | sed -n "s/^First sector: \([0-9]*\).*/\1/p")
python3 - card.img $START <<'PY'
import struct, sys
f = open(sys.argv[1], "r+b"); off = int(sys.argv[2]) * 512
f.seek(off); b = bytearray(f.read(512)); b[13] = 3; f.seek(off); f.write(b)
PY
before=$(md5sum card.img | cut -d" " -f1)
set +e; out=$(./sdgrow card.img - 2>&1); rc=$?; set -e
[ $rc -eq 2 ] && [ "$(md5sum card.img | cut -d" " -f1)" = "$before" ] && ok "rifiutato prima di rifare la GPT ($out)" || ko "uscita $rc: $out"

echo "--- schema sconosciuto: niente modifiche"
mkimg card.img fat.img; sgdisk -c 4:altro card.img >/dev/null; truncate -s 16G card.img
before=$(md5sum card.img | cut -d" " -f1)
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 2 ] && [ "$(md5sum card.img | cut -d" " -f1)" = "$before" ] && ok "rifiutato, immagine intatta" || ko "uscita $rc"
echo "TUTTO OK"
