#!/bin/sh
# Giornale dell'aggiornamento in system-init (audit R03, issue #3): file di prima rimessi dopo
# un'installazione interrotta o un ritorno al sistema precedente, copie tolte dopo la conferma.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-journal.sh
sed -n '/^# >>> giornale/,/^# <<< giornale/p' /trimui/m2/system-init > /tmp/giornale.sh
[ -s /tmp/giornale.sh ] || { echo "blocco non trovato"; exit 1; }
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
ROOT=/tmp/j/Arcade; SYSF=$ROOT/System/arcade-system.squashfs; PENDING=$ROOT/System/update-pending
J=$ROOT/System/update-journal.txt
# scheda dopo un'installazione: a.so sostituito (originale in .annulla), x.json nuovo,
# y.json non ancora sostituito (c'è solo il .nuovo scaricato)
card() {  # stato, versione nel giornale
    rm -rf /tmp/j; mkdir -p $ROOT/System $ROOT/Cores $ROOT/Lang
    echo nuovo > $ROOT/Cores/a.so; echo vecchio > $ROOT/Cores/a.so.annulla
    echo nuovo > $ROOT/Lang/x.json
    echo vecchio > $ROOT/Lang/y.json; echo nuovo > $ROOT/Lang/y.json.nuovo
    echo fuori > /tmp/j/fuori.txt.annulla; echo resta > /tmp/j/fuori.txt
    echo sistema > $SYSF; echo scaricato > $SYSF.nuovo
    printf 'mosaik-giornale 1\nversion %s\nstato %s\nfile Cores/a.so\nnuovo Lang/x.json\nfile Lang/y.json\nfile ../fuori.txt\n' "$2" "$1" > $J
}
run() {  # RUNVER
    (
        RUNVER=$1; LOG=/tmp/j/log
        step() { echo "$*" >> $LOG; }
        rollback() { echo rollback >> $LOG; }
        sync() { :; }
        . /tmp/giornale.sh
        journal_check
    )
}
restored() {
    [ "$(cat $ROOT/Cores/a.so)" = vecchio ] && [ ! -e $ROOT/Cores/a.so.annulla ] && [ ! -e $ROOT/Lang/x.json ] \
        && [ "$(cat $ROOT/Lang/y.json)" = vecchio ] && [ ! -e $ROOT/Lang/y.json.nuovo ] && [ ! -e $J ]
}
outside_ok() { [ "$(cat /tmp/j/fuori.txt)" = resta ] && [ -f /tmp/j/fuori.txt.annulla ]; }

echo "--- corrente mancata durante l'installazione (stato applico)"
card applico "1.0 B"; run "1.0 A"
restored && ok "tutti i file di prima rimessi, quelli nuovi tolti" || ko "non ripristinato: $(ls -R $ROOT)"
[ ! -e $SYSF.nuovo ] && ok "tolto anche il sistema scaricato e mai installato" || ko "resta il sistema .nuovo"
outside_ok && ok "percorso fuori da Arcade ignorato" || ko "toccato un file fuori da Arcade"
grep -q rollback /tmp/j/log && ko "sistema rimesso senza bisogno" || ok "sistema vecchio ancora in uso: nessun riavvio"
card applico "1.0 B"; echo 0 > $PENDING; run "1.0 B"
restored && grep -q rollback /tmp/j/log && ok "sistema nuovo già al suo posto: file di prima e ritorno al sistema precedente" || ko "applico con segno di prova"
card applico "1.0 B"; mv $SYSF $SYSF.prec; run "1.0 A"
[ -f $SYSF ] && [ ! -e $SYSF.prec ] && restored && ok "sistema principale mancante: rimesso dal precedente" || ko "sistema non rimesso"

echo "--- tornati al sistema precedente dopo tre prove (versione in uso diversa)"
card installato "1.0 B"; run "1.0 A"
restored && ok "file della versione precedente rimessi" || ko "ritorno indietro senza file di prima"

echo "--- avvio di prova in corso"
card installato "1.0 B"; echo 1 > $PENDING; run "1.0 B"
[ "$(cat $ROOT/Cores/a.so)" = nuovo ] && [ -f $ROOT/Cores/a.so.annulla ] && [ -f $J ] && ok "niente cambia finché il launcher non conferma" || ko "toccato durante la prova"

echo "--- confermato, corrente mancata prima di togliere le copie"
card installato "1.0 B"; run "1.0 B"
[ "$(cat $ROOT/Cores/a.so)" = nuovo ] && [ ! -e $ROOT/Cores/a.so.annulla ] && [ -f $ROOT/Lang/x.json ] && [ ! -e $J ] \
    && ok "versione nuova tenuta, copie e giornale tolti" || ko "conferma non finita: $(ls -R $ROOT)"
outside_ok && ok "anche qui il percorso fuori da Arcade è ignorato" || ko "copia fuori da Arcade tolta"

echo "--- nessun giornale"
card installato "1.0 B"; rm $J; run "1.0 A"
[ "$(cat $ROOT/Cores/a.so)" = nuovo ] && [ -f $ROOT/Cores/a.so.annulla ] && ok "niente da fare" || ko "toccato senza giornale"

[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors errori"; exit 1; }
