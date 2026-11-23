#!/bin/sh
# Rete locale 127.0.0.1 all'avvio (23 novembre): Mosaik non accendeva l'interfaccia lo e TubeHero,
# che passa il video al suo lettore attraverso 127.0.0.1, diceva «Could not open local media
# transport». Si spegne lo, si esegue il blocco di m2/system-init e si controlla che un server su
# 127.0.0.1 risponda. Serve NET_ADMIN per spegnere lo:
#   docker run --rm --cap-add NET_ADMIN --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-rete-locale.sh
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }

# il blocco, dal commento «rete locale 127.0.0.1» alla riga con step "rete locale
awk '/^# rete locale 127\.0\.0\.1/ {on=1} on {print} on && /step "rete locale/ {exit}' /trimui/m2/system-init > /tmp/blocco.sh
step() { echo "$1" > /tmp/step.txt; }

echo "--- lo spenta all'avvio"
ip link set lo down || { echo "serve --cap-add NET_ADMIN"; exit 1; }
ip addr flush dev lo
. /tmp/blocco.sh
( echo pronto | nc -l -p 9123 -s 127.0.0.1 > /dev/null ) & srv=$!
sleep 1
got=$(nc -w 2 127.0.0.1 9123 < /dev/null 2> /dev/null)
kill $srv 2> /dev/null
if [ "$got" = pronto ]; then ok "un server su 127.0.0.1 risponde"; else ko "un server su 127.0.0.1 non risponde"; fi
if grep -q "spenta, ora 127.0.0.1 attivo" /tmp/step.txt; then ok "registro: $(cat /tmp/step.txt)"; else ko "registro: $(cat /tmp/step.txt 2> /dev/null)"; fi

echo "--- lo già accesa"
. /tmp/blocco.sh
if grep -q "già accesa, ora 127.0.0.1 attivo" /tmp/step.txt; then ok "registro: $(cat /tmp/step.txt)"; else ko "registro: $(cat /tmp/step.txt 2> /dev/null)"; fi

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
