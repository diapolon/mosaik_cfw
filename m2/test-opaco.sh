#!/bin/sh
# Display opaco per i giochi Linux e le app che lo chiedono (23 novembre): con il PowerVR la finestra
# di alcuni programmi esce trasparente e si vede nero con l'audio (PPSSPP, il video di TubeHero).
# Il launcher scrive ARCADE_OPAQUE=1 in next.env ("opaque": true in game.json/app.json); il blocco
# dei giochi Linux di m2/system-init esegue «opaco on» prima e, dopo ogni gioco Linux, «opaco off».
# Si esegue il blocco vero con gamewatch e opaco finti che segnano le chiamate.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-opaco.sh
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }

# il blocco: da «if [ "$kind" = "linux" ]; then» al suo «fi»
awk '/^    if \[ "\$kind" = "linux" \]; then/ {on=1} on {print} on && /^    fi$/ {exit}' /trimui/m2/system-init > /tmp/blocco.sh
[ -s /tmp/blocco.sh ] || { echo "blocco dei giochi Linux non trovato in system-init"; exit 1; }

BIN=/tmp/bin ROOT=/tmp/sd/Arcade LINUXLOG=/tmp/linux.log
mkdir -p $BIN $ROOT/System /tmp/sd/Arcade/Linux/Gioco
printf '#!/bin/sh\necho "opaco $1" >> /tmp/chiamate.txt\n' > $BIN/opaco
printf '#!/bin/sh\necho "gioco ARCADE_OPAQUE=${ARCADE_OPAQUE:-}" >> /tmp/chiamate.txt\nexit 0\n' > $BIN/gamewatch
chmod +x $BIN/opaco $BIN/gamewatch
printf '#!/bin/sh\n' > $ROOT/Linux/Gioco/launch.sh
sd_check() { :; }; zombie_note() { :; }; finish() { :; }
gira() {   # $1 = contenuto di next.env (vuoto: nessun file)
    rm -f /tmp/chiamate.txt $ROOT/System/next.env
    [ -n "$1" ] && printf '%s\n' "$1" > $ROOT/System/next.env
    kind=linux rom=$ROOT/Linux/Gioco/launch.sh title=Gioco
    # il blocco finisce con «continue» (è dentro il ciclo dei programmi)
    for giro in 1; do . /tmp/blocco.sh; done
    tr '\n' '|' < /tmp/chiamate.txt
}

echo "--- gioco che chiede il display opaco"
c=$(gira "export ARCADE_OPAQUE=1")
if [ "$c" = "opaco on|gioco ARCADE_OPAQUE=1|opaco off|" ]; then ok "opaco on, gioco, opaco off ($c)"; else ko "ordine sbagliato: $c"; fi

echo "--- gioco normale"
c=$(gira "")
if [ "$c" = "gioco ARCADE_OPAQUE=|opaco off|" ]; then ok "niente opaco on; dopo il gioco opaco off per sicurezza ($c)"; else ko "chiamate: $c"; fi

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
