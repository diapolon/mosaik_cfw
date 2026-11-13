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

echo "--- schema sconosciuto: niente modifiche"
mkimg card.img fat.img; sgdisk -c 4:altro card.img >/dev/null; truncate -s 16G card.img
before=$(md5sum card.img | cut -d" " -f1)
set +e; ./sdgrow card.img -; rc=$?; set -e
[ $rc -eq 2 ] && [ "$(md5sum card.img | cut -d" " -f1)" = "$before" ] && ok "rifiutato, immagine intatta" || ko "uscita $rc"
echo "TUTTO OK"
