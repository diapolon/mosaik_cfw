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
echo "TUTTO OK ($N casi)"
