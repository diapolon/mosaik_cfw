#!/bin/sh
# Prova della scelta del sistema nell'initramfs (conteggio degli avvii di prova, ritorno al
# precedente) con mount finto. Non prova loop mount, switch_root e hardware: servono le console.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-boot.sh
sed -n '/^# >>> scelta del sistema/,/^# <<< scelta del sistema/p' /trimui/m2/initramfs-init > /tmp/blocco.sh
[ -s /tmp/blocco.sh ] || { echo "blocco non trovato"; exit 1; }
step() { :; }; fail() { echo "FAIL:$1"; exit 9; }
SD=/sd/Arcade/System; SYSF=$SD/arcade-system.squashfs
run() {  # pending, sistema (ok|noinit|-), precedente (ok|-)
    rm -rf /sd /root/sbin /root/fw; mkdir -p $SD /root /etc; printf 'tg5040\nModello\n' > /etc/arcade-model
    [ "$2" != - ] && echo "$2" > $SYSF
    [ "$3" != - ] && echo "$3" > $SYSF.prec
    [ -n "$1" ] && echo "$1" > $SD/update-pending
    (
        mount() {
            [ -f "$5" ] || return 1; MOUNTED=$5
            [ "$(cat "$5")" = ok ] || return 0
            mkdir -p /root/sbin /root/fw/tg5040; : > /root/sbin/init; chmod +x /root/sbin/init
        }
        umount() { rm -rf /root/sbin /root/fw; }
        sync() { :; }
        . /tmp/blocco.sh
        echo "${MOUNTED##*/} pending=$(cat $SD/update-pending 2>/dev/null) guasto=$([ -f $SYSF.guasto ] && echo si || echo no)"
    )
}
N=0
expect() { N=$((N + 1)); [ "$1" = "$2" ] && echo "  ok: $3" || { echo "  ERRORE: $3: atteso [$2], ottenuto [$1]"; exit 1; }; }
S=arcade-system.squashfs
expect "$(run '' ok ok)" "$S pending= guasto=no" "avvio normale"
expect "$(run 0 ok ok)" "$S pending=1 guasto=no" "primo avvio di prova"
expect "$(run 2 noinit ok)" "$S pending= guasto=si" "terza prova fallita: torna al precedente"
expect "$(run 0 noinit ok)" "$S.prec pending=1 guasto=no" "sistema senza init: usa il precedente"
expect "$(run '1 2' ok ok)" "$S pending=1 guasto=no" "contatore rovinato"
expect "$(run 0 - ok)" "$S.prec pending=1 guasto=no" "sistema assente: usa il precedente"
expect "$(run '' - -)" "FAIL:MANCA IL SISTEMA" "nessun sistema"
expect "$(run '' noinit -)" "FAIL:SISTEMA NON AVVIABILE" "sistema rotto senza precedente"

echo "--- controllo del FAT prima di montarlo"
apk add -q dosfstools >/dev/null
sed -n '/^# >>> controllo del FAT/,/^# <<< controllo del FAT/p' /trimui/m2/initramfs-init > /tmp/fat.sh
[ -s /tmp/fat.sh ] || { echo "blocco del FAT non trovato"; exit 1; }
fat() {  # $1 immagine; stampa il risultato del blocco
    (
        P=$1; fbmsg() { :; }
        fail() { echo "FAIL:$1"; exit 0; }
        . /tmp/fat.sh
        echo "$FSCK"
    )
}
dirty() { printf '\001' | dd of=$1 bs=1 seek=65 conv=notrunc status=none; }
rm -f /tmp/f.img; truncate -s 64M /tmp/f.img; mkfs.vfat -F 32 /tmp/f.img >/dev/null
expect "$(fat /tmp/f.img)" "skip" "chiuso bene: nessun controllo"
dirty /tmp/f.img
r=$(fat /tmp/f.img); case $r in 0|1) echo "  ok: spento male ma sano ($r)";; *) echo "  ERRORE: spento male ma sano: $r"; exit 1;; esac
# le due copie della tabella diverse: fsck le riallinea, poi la verifica è pulita
python3 -c "import sys" 2>/dev/null || apk add -q python3 >/dev/null
python3 - <<'PY'
f=open('/tmp/f.img','r+b'); b=f.read(512); rs=int.from_bytes(b[14:16],'little'); fz=int.from_bytes(b[36:40],'little')
f.seek((rs+fz)*512+12); f.write(b'\xff\xff\xff\x0f'); f.close()
PY
dirty /tmp/f.img
expect "$(fat /tmp/f.img)" "1" "errori riparati e verificati"
fsck.fat -n /tmp/f.img >/dev/null && echo "  ok: dopo la riparazione è pulito" || { echo "  ERRORE: non pulito"; exit 1; }
mkdir -p /tmp/finto; printf '#!/bin/sh\nexit 1\n' > /tmp/finto/fsck.fat; chmod +x /tmp/finto/fsck.fat
dirty /tmp/f.img
expect "$(PATH=/tmp/finto:$PATH fat /tmp/f.img)" "FAIL:SD DA RIPARARE" "non riparabile: si ferma prima di allargare e montare"
# la funzione vera: anche se lo spegnimento non riesce, dopo un errore non si va avanti
sed -n '/^fail() {/,/^}/p' /trimui/m2/initramfs-init > /tmp/fail.sh
[ -s /tmp/fail.sh ] || { echo "funzione fail non trovata"; exit 1; }
( step() { :; }; fbmsg() { :; }; poweroff() { return 1; }; sync() { :; }; SD_LOGS=""
  . /tmp/fail.sh; fail "PROVA" ""; echo PROSEGUITO ) > /tmp/fail.out 2>&1 &
FP=$!; sleep 2
if grep -q PROSEGUITO /tmp/fail.out; then echo "  ERRORE: dopo l'errore l'avvio prosegue"; exit 1; fi
kill $FP 2>/dev/null; N=$((N + 1)); echo "  ok: spegnimento non riuscito: l'avvio non prosegue"
echo "TUTTO OK ($N casi)"
