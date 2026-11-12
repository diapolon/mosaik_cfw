#!/bin/sh
# Prova del registro dei consumi (m2/consumi) con un /sys finto e finti launcher e runner, e
# dell'analisi (toolchain/consumi.py):
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-consumi.sh
set -e
apk add -q python3 >/dev/null
W=/tmp/consumi; rm -rf $W; mkdir -p $W/sys/class/power_supply/axp2202-battery $W/sys/class/power_supply/axp2202-usb \
    $W/sys/class/net/wlan0/statistics $W/sys/devices/system/cpu/cpu0/cpufreq $W/sys/class/thermal/thermal_zone0 $W/bin
cd $W
B=sys/class/power_supply/axp2202-battery
echo 77 > $B/capacity; echo 3912000 > $B/voltage_now; echo -350000 > $B/current_now; echo Discharging > $B/status
echo 0 > sys/class/power_supply/axp2202-usb/online
echo up > sys/class/net/wlan0/operstate; echo 2048000 > sys/class/net/wlan0/statistics/rx_bytes; echo 10240 > sys/class/net/wlan0/statistics/tx_bytes
echo 1008000 > sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq; echo 41000 > sys/class/thermal/thermal_zone0/temp
printf '#!/bin/sh\nsleep 60\n' > bin/launcher; printf '#!/bin/sh\nsleep 60\n' > bin/runner; chmod +x bin/*
ok=0; ko=0
check() { if [ "$2" = "$3" ]; then ok=$((ok+1)); echo "  ok: $1"; else ko=$((ko+1)); echo "  ERRORE: $1: atteso [$2], ottenuto [$3]"; fi; }
export CONSUMI_OUT=$W/consumi.tsv CONSUMI_BUF=$W/buf CONSUMI_PID=$W/pid CONSUMI_SYS=$W/sys CONSUMI_EVERY=1 CONSUMI_FLUSH=2
bin/launcher & L=$!
sh /trimui/m2/consumi & C=$!
sleep 3.5
check "intestazione" "ora" "$(head -1 consumi.tsv | cut -f1)"
check "21 colonne" "21" "$(sed -n 2p consumi.tsv | awk -F'\t' '{print NF}')"
check "stato: menu" "menu" "$(sed -n 2p consumi.tsv | cut -f3)"
check "launcher misurato" "si" "$([ -n "$(sed -n 2p consumi.tsv | cut -f7)" ] && echo si || echo no)"
check "batteria, tensione, corrente" "77 3912 -350 Discharging" "$(sed -n 2p consumi.tsv | cut -f14-17 | tr '\t' ' ')"
check "processore e temperatura" "1008 41" "$(sed -n 2p consumi.tsv | cut -f12-13 | tr '\t' ' ')"
check "wi-fi e traffico" "up 2000 10" "$(sed -n 2p consumi.tsv | cut -f19-21 | tr '\t' ' ')"
kill $L
bin/runner --root /x --core /x/Cores/fbneo_libretro.so --rom /x/a.zip & R=$!
sleep 0.3
kill -USR1 $C; sleep 0.5
check "misura su richiesta, in un gioco" "gioco fbneo" "$(tail -1 consumi.tsv | cut -f3-4 | tr '\t' ' ')"
check "runner misurato" "si" "$([ -n "$(tail -1 consumi.tsv | cut -f8)" ] && echo si || echo no)"
kill $R
echo 76 > $B/capacity
kill -TERM $C; wait $C 2>/dev/null || true
check "chiusura: righe scritte" "76" "$(tail -1 consumi.tsv | cut -f14)"
check "chiusura: pid tolto" "no" "$([ -e pid ] && echo si || echo no)"
check "niente in memoria" "0" "$(wc -c < buf | tr -d ' ')"
# più di 2 MB: il vecchio diventa .1
head -c 2200000 /dev/zero | tr '\0' 'x' > consumi.tsv
sh /trimui/m2/consumi & C=$!
sleep 2.5; kill -TERM $C; wait $C 2>/dev/null || true
check "file grande messo da parte" "si" "$([ -f consumi.tsv.1 ] && echo si || echo no)"
check "file nuovo con intestazione" "ora" "$(head -1 consumi.tsv | cut -f1)"

# analisi: una sessione finta con menu, gioco e una notte di standby
python3 - <<'P'
rows = ["ora\tsecondi\tstato\tgioco\tmem_disp_kB\tmem_libera_kB\tlauncher_kB\trunner_kB\tprocesso_piu_grande\tcpu_%\tcarico\tmhz\ttemp_C\tbatteria_%\tmV\tmA\tcarica\tusb\twifi\trx_kB\ttx_kB"]
t = 0
def add(state, game, cap, ma, launcher, runner, mem, sec):
    rows.append("\t".join(map(str, ["2026-11-12T%02d:%02d:%02d" % (sec // 3600 % 24, sec // 60 % 60, sec % 60), sec, state, game, mem, mem - 1000, launcher, runner, "deadcast:900", 10, 0.5, 1008, 40, cap, 3900, ma, "Discharging", 0, "up", 0, 0])))
for i in range(20): add("menu", "", 90 - i // 10, -300, 30000 + i * 100, "", 300000, i * 30)
for i in range(20): add("gioco", "fbneo", 88 - i // 5, -650, "", 50000, 250000, 600 + i * 30)
add("menu", "", 84, -300, 32000, "", 300000, 1200)
add("menu", "", 80, -300, 32000, "", 300000, 1200 + 8 * 3600)   # notte in standby
open("/tmp/consumi/sessione.tsv", "w").write("\n".join(rows) + "\n")
P
python3 /trimui/toolchain/consumi.py sessione.tsv > analisi.txt
cat analisi.txt
check "analisi: menu" "si" "$(grep -q '^menu' analisi.txt && echo si || echo no)"
check "analisi: gioco fbneo" "si" "$(grep -q 'gioco fbneo' analisi.txt && echo si || echo no)"
check "analisi: standby 0.5 %/h" "si" "$(grep -q 'standby.*0.50 %/h' analisi.txt && echo si || echo no)"
check "analisi: launcher che cresce" "si" "$(grep -q 'launcher.*+' analisi.txt && echo si || echo no)"
echo
if [ $ko -eq 0 ]; then echo "TUTTO OK ($ok prove)"; else echo "$ko PROVE FALLITE"; exit 1; fi
