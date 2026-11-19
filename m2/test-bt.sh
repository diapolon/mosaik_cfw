#!/bin/sh
# Servizio del Bluetooth (m2/bt-ctl) con hciattach, hciconfig, hcitool, bluetoothctl, bluetoothd,
# bluealsa e D-Bus finti: accensione con tentativi, indirizzo fisso, ricerca, abbinamento salvato
# sulla SD e ripreso, ricollegamento, "dimentica", chip che non risponde.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-bt.sh
apk add -q python3 > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

F=/tmp/fbt                      # stato dei programmi finti
export ROOT=/tmp/sd/Arcade BT_SYS=/tmp/sys BT_STORE=/tmp/store BT_RUN=/tmp/runbt BT_BTWRITE=/tmp/btwrite
export BT_LOCK=/tmp/bt.lock BT_WIFI_LOCK=/tmp/wifi.lock BT_I2C=/nessuno BT_RECONNECT_WAIT=0 BT_WATCH_EVERY=1 BT_DIAG=0
export BT_ASOUND=/tmp/asound.conf BT_ASOUND_BASE=/tmp/asound.base BT_AUDIO_FLAG=/tmp/bt-audio
BIN=/tmp/fbin; mkdir -p $BIN; export PATH=$BIN:$PATH
CTL="sh /trimui/m2/bt-ctl"

cat > $BIN/hciattach <<'X'
#!/bin/sh
n=$(cat /tmp/fbt/attach_fail 2>/dev/null || echo 0)
echo "hciattach $*" >> /tmp/fbt/calls
if [ "$n" -gt 0 ]; then echo $((n - 1)) > /tmp/fbt/attach_fail; exit 1; fi
mkdir -p /tmp/sys/bluetooth/hci0; exec sleep 300
X
cat > $BIN/hciconfig <<'X'
#!/bin/sh
case "$2" in up|down) echo "hciconfig $2" >> /tmp/fbt/calls; exit 0 ;; esac
printf 'hci0:\tType: Primary  Bus: UART\n\tBD Address: %s  ACL MTU: 1021:8\n' "$(cat /tmp/fbt/addr 2>/dev/null || echo 22:22:01:02:03:04)"
X
cat > $BIN/hcitool <<'X'
#!/bin/sh
# hcitool -i hci0 cmd 0x3f 0x000a 02 00 06 b0 b1 b2 b3 b4 b5 (indirizzo al contrario)
echo "hcitool $*" >> /tmp/fbt/calls
shift 2
[ "$1 $2 $3" = "cmd 0x3f 0x000a" ] && echo "${12}:${11}:${10}:${9}:${8}:${7}" > /tmp/fbt/addr
exit 0
X
cat > $BIN/dbus-daemon <<'X'
#!/bin/sh
exec python3 -c "import socket,time;s=socket.socket(socket.AF_UNIX);s.bind('/run/dbus/system_bus_socket');time.sleep(600)"
X
printf '#!/bin/sh\nexec sleep 600\n' > $BIN/bluetoothd
printf '#!/bin/sh\nexec sleep 600\n' > $BIN/bluealsa
# bluetoothctl: dispositivi vicini in $F/near (indirizzo|nome|icona), conosciuti in $F/known,
# abbinati e collegati in $F/paired, $F/connected; gli abbinamenti scritti come bluetoothd
cat > $BIN/bluetoothctl <<'X'
#!/bin/sh
F=/tmp/fbt
while [ $# -gt 0 ]; do case "$1" in --timeout|--agent) shift 2 ;; *) break ;; esac; done
pgrep -f "sleep 600" > /dev/null || { echo "Waiting to connect to bluetoothd..."; exit 1; }
line() { grep "^$1|" $F/known 2>/dev/null | head -n 1; }
case "$1" in
show) echo "Controller $(cat $F/addr 2>/dev/null) (public)" ;;
power) echo "Changing power $2 succeeded" ;;
scan) cat $F/near >> $F/known 2>/dev/null; sort -u -o $F/known $F/known ;;
devices) cut -d'|' -f1,2 $F/known 2>/dev/null | while IFS='|' read -r a n; do echo "Device $a ${n:-$(echo $a | tr : -)}"; done ;;
paired-devices) cat $F/paired 2>/dev/null | while read -r a; do echo "Device $a $(line $a | cut -d'|' -f2)"; done ;;
info)
    l=$(line "$2"); [ -n "$l" ] || { echo "Device $2 not available"; exit 1; }
    echo "Device $2 (public)"
    n=$(echo "$l" | cut -d'|' -f2); [ -n "$n" ] && echo "	Name: $n"
    echo "	Alias: ${n:-x}"
    echo "	Icon: $(echo "$l" | cut -d'|' -f3)"
    c=$(echo "$l" | cut -d'|' -f4); [ -n "$c" ] && echo "	Class: $c"
    grep -qx "$2" $F/paired 2>/dev/null && echo "	Paired: yes" || echo "	Paired: no"
    grep -qx "$2" $F/connected 2>/dev/null && echo "	Connected: yes" || echo "	Connected: no" ;;
pair)
    [ -n "$(line "$2")" ] || { echo "Device $2 not available"; exit 1; }
    echo "$2" >> $F/paired
    d=/tmp/store/$(cat $F/addr)/$2; mkdir -p $d /tmp/store/$(cat $F/addr)/cache; echo "[LinkKey]" > $d/info
    echo "x" > /tmp/store/$(cat $F/addr)/cache/$2
    echo "Pairing successful" ;;
trust) echo "Changing $2 trust succeeded" ;;
connect)
    [ -f $F/connect_fail ] && { echo "Failed to connect: org.bluez.Error.Failed"; exit 1; }
    grep -qx "$2" $F/paired 2>/dev/null || { echo "Failed to connect"; exit 1; }
    echo "$2" > $F/connected; echo "Connection successful" ;;
disconnect) rm -f $F/connected; echo "Successful disconnected" ;;
remove)
    grep -vx "$2" $F/paired > $F/p2 2>/dev/null; mv $F/p2 $F/paired; rm -f $F/connected
    rm -rf /tmp/store/$(cat $F/addr)/$2; echo "Device has been removed" ;;
esac
X
chmod +x $BIN/*

reset() {
    pkill -f "bt-ctl watch" 2>/dev/null
    pkill -f "sleep 600" 2>/dev/null; pkill -f "sleep 300" 2>/dev/null; pkill -f system_bus_socket 2>/dev/null
    sleep 0.3
    rm -rf /tmp/sd /tmp/sys /tmp/store /tmp/runbt $F /run/dbus /tmp/bt.lock /tmp/bt-audio
    printf 'pcm.!default { type hw card audiocodec }\n' > $BT_ASOUND_BASE; cp $BT_ASOUND_BASE $BT_ASOUND
    mkdir -p $ROOT/System $ROOT/Logs /tmp/sys/rfkill/rfkill0 /tmp/sys/sunxi_info $F
    echo bluetooth > /tmp/sys/rfkill/rfkill0/type; echo 0 > /tmp/sys/rfkill/rfkill0/state
    : > $BT_BTWRITE
    printf 'sunxi_platform    : sun50iw10p1\nsunxi_serial      : 0123456789abcdef0011\n' > /tmp/sys/sunxi_info/sys_info
    printf '02:00:00:00:00:01|Wireless Earbuds|audio-card|0x240404\n11:22:33:44:55:66|Pad|input-gaming\n02:00:00:00:00:05||\n02:00:00:00:00:03|Cassa|audio-card|0x240414\n' > $F/near
}
reboot() {   # come uno spegnimento: processi e /run spariscono, la SD resta
    pkill -f "bt-ctl watch" 2>/dev/null
    pkill -f "sleep 600" 2>/dev/null; pkill -f "sleep 300" 2>/dev/null; pkill -f system_bus_socket 2>/dev/null
    sleep 0.3
    rm -rf /tmp/sys/bluetooth /tmp/store /tmp/runbt /run/dbus $F/connected $F/calls /tmp/bt-audio
    cp $BT_ASOUND_BASE $BT_ASOUND
    echo 0 > /tmp/sys/rfkill/rfkill0/state
}
addr_of_serial() { h=$(printf 'mosaik-bt %s' 0123456789abcdef0011 | md5sum | cut -c1-10); echo "2C$h" | tr a-f A-F | sed 's/\(..\)/\1:/g; s/:$//'; }

echo "--- accensione"
reset
$CTL on; r=$?
A=$(addr_of_serial)
check "acceso (uscita 0)" '[ $r = 0 ]'
check "indirizzo ricavato dal numero di serie e salvato" '[ "$(cat $ROOT/System/bluetooth/indirizzo.txt)" = "$A" ]'
check "indirizzo dato al chip con 0xfc0a" '[ "$(cat $F/addr)" = "$A" ]'
check "chip acceso (rfkill 1)" '[ "$(cat /tmp/sys/rfkill/rfkill0/state)" = 1 ]'
check "bluetoothd e bluealsa avviati" '[ -f /tmp/runbt/bluetoothd.pid ] && [ -f /tmp/runbt/bluealsa.pid ]'
check "stato acceso" '[ "$($CTL status)" = acceso ]'
check "registro" 'grep -q "Bluetooth acceso" $ROOT/Logs/bluetooth.log'
n1=$(grep -c "^hciattach" $F/calls)
$CTL on
check "seconda accensione: niente di nuovo" '[ "$(grep -c "^hciattach" $F/calls)" = "$n1" ] && grep -q "già acceso" $ROOT/Logs/bluetooth.log'

echo "--- ricerca e abbinamento"
$CTL off > /dev/null
$CTL on > /dev/null 2>&1 &
out=$($CTL scan 1)
check "ricerca chiesta col Bluetooth ancora spento: aspetta che sia pronto" 'echo "$out" | grep -q "^02:00:00:00:00:01	audio"'
wait
out=$($CTL scan 1)
check "ricerca: cuffie come audio" 'echo "$out" | grep -q "^02:00:00:00:00:01	audio	0	0	Wireless Earbuds$"'
check "ricerca: controller come input" 'echo "$out" | grep -q "^11:22:33:44:55:66	input	0	0	Pad$"'
check "ricerca: senza nome non elencato" '! echo "$out" | grep -q 5A:08'
check "ricerca: cassa riconosciuta dalla classe" 'echo "$out" | grep -q "^02:00:00:00:00:03	cassa	0	0	Cassa$"'
$CTL pair 02:00:00:00:00:01; r=$?
check "abbinamento riuscito" '[ $r = 0 ]'
check "abbinamenti sulla SD, senza la cache" 'tar -tf $ROOT/System/bluetooth/abbinamenti.tar | grep -q "02:00:00:00:00:01/info" && ! tar -tf $ROOT/System/bluetooth/abbinamenti.tar | grep -q cache/'
check "ultimo dispositivo ricordato" '[ "$(cat $ROOT/System/bluetooth/ultimo.txt)" = 02:00:00:00:00:01 ]'
check "stato collegato" '$CTL status | grep -q "^collegato 02:00:00:00:00:01 audio Wireless Earbuds$"'
check "audio verso le cuffie" 'grep -q "bluealsa:DEV=02:00:00:00:00:01,PROFILE=a2dp" $BT_ASOUND && grep -q "type hw card audiocodec" $BT_ASOUND && [ "$(cat /tmp/bt-audio)" = 02:00:00:00:00:01 ]'
check "uscita delle cuffie senza softvol" '! grep -q softvol $BT_ASOUND'
check "devices: abbinato e collegato" '$CTL devices | grep -q "^02:00:00:00:00:01	audio	1	1	"'
$CTL disconnect 02:00:00:00:00:01
check "scollegate: audio all'altoparlante" 'cmp -s $BT_ASOUND $BT_ASOUND_BASE && [ ! -f /tmp/bt-audio ]'
$CTL connect 02:00:00:00:00:01
check "ricollegate a mano: audio alle cuffie" 'grep -q "bluealsa:DEV=02:00:00:00:00:01" $BT_ASOUND'
check "indirizzo non valido rifiutato" '! $CTL connect pippo 2>/dev/null'

echo "--- spegnimento e riaccensione"
$CTL off
check "spento: rfkill 0 e demoni fermi" '[ "$(cat /tmp/sys/rfkill/rfkill0/state)" = 0 ] && [ ! -f /tmp/runbt/bluetoothd.pid ] && ! pgrep -f "sleep 600" > /dev/null'
check "stato spento" '[ "$($CTL status)" = spento ]'
check "spento: audio di nuovo all'altoparlante" 'cmp -s $BT_ASOUND $BT_ASOUND_BASE && [ ! -f /tmp/bt-audio ]'
reboot
$CTL on
check "abbinamenti ripresi dalla SD" '[ -f /tmp/store/$A/02:00:00:00:00:01/info ]'
check "stesso indirizzo dopo la riaccensione" '[ "$(cat $F/addr)" = "$A" ]'
for i in $(seq 30); do [ -f /tmp/runbt/collegato ] && break; sleep 0.2; done
check "ricollegato da solo all'ultimo dispositivo" '[ "$(cat /tmp/runbt/collegato 2>/dev/null)" = 02:00:00:00:00:01 ]'
for i in $(seq 20); do [ -f /tmp/bt-audio ] && break; sleep 0.2; done
check "dopo il ricollegamento l'audio torna alle cuffie" 'grep -q "bluealsa:DEV=02:00:00:00:00:01" $BT_ASOUND'
rm -f $F/connected   # le cuffie si spengono da sole
for i in $(seq 30); do [ -f /tmp/bt-audio ] || break; sleep 0.2; done
check "cuffie spente: audio all'altoparlante da solo" 'cmp -s $BT_ASOUND $BT_ASOUND_BASE && [ ! -f /tmp/bt-audio ]'
echo 02:00:00:00:00:01 > $F/connected   # e si riaccendono
for i in $(seq 30); do [ -f /tmp/bt-audio ] && break; sleep 0.2; done
check "cuffie riaccese: audio di nuovo alle cuffie" 'grep -q "bluealsa:DEV=02:00:00:00:00:01" $BT_ASOUND'

echo "--- dimentica"
$CTL forget 02:00:00:00:00:01
check "tolto dalla SD" '! tar -tf $ROOT/System/bluetooth/abbinamenti.tar | grep -q E0:9D'
check "non più l'ultimo" '[ ! -f $ROOT/System/bluetooth/ultimo.txt ]'
check "dimenticate: audio all'altoparlante" 'cmp -s $BT_ASOUND $BT_ASOUND_BASE'
check "stato senza collegamento" '[ "$($CTL status)" = acceso ]'
$CTL off

echo "--- ricollegamento che non riesce"
reset
$CTL on > /dev/null; $CTL scan 1 > /dev/null; $CTL pair 02:00:00:00:00:01 > /dev/null; $CTL off
reboot
touch $F/connect_fail
$CTL on
for i in $(seq 40); do grep -q "ricollegamento a 02:00:00:00:00:01 non riuscito" $ROOT/Logs/bluetooth.log && break; sleep 0.2; done
check "dopo 6 tentativi rinuncia e lo scrive" 'grep -q "ricollegamento a 02:00:00:00:00:01 non riuscito" $ROOT/Logs/bluetooth.log'
check "il Bluetooth resta acceso" '[ "$($CTL status)" = acceso ]'
rm -f $F/connect_fail
$CTL off

echo "--- chip che risponde al terzo tentativo"
reset
echo 2 > $F/attach_fail
$CTL on; r=$?
check "acceso al terzo tentativo" '[ $r = 0 ] && [ "$(grep -c "^hciattach" $F/calls)" = 3 ]'
check "i tentativi falliti nel registro" '[ "$(grep -c "hci0 non comparso" $ROOT/Logs/bluetooth.log)" = 2 ]'
$CTL off

echo "--- chip che non risponde"
reset
echo 9 > $F/attach_fail
$CTL on; r=$?
check "uscita 1" '[ $r = 1 ]'
check "chip spento e niente demoni" '[ "$(cat /tmp/sys/rfkill/rfkill0/state)" = 0 ] && [ ! -f /tmp/runbt/bluetoothd.pid ]'
check "lo dice il registro" 'grep -q "NON acceso: il chip non risponde" $ROOT/Logs/bluetooth.log'
check "stato spento" '[ "$($CTL status)" = spento ]'

echo "--- indirizzo senza numero di serie"
reset
rm -f /tmp/sys/sunxi_info/sys_info
$CTL on
B=$(cat $ROOT/System/bluetooth/indirizzo.txt)
check "indirizzo a caso valido e salvato" 'echo "$B" | grep -q -E "^2C(:[0-9A-F]{2}){5}$" && [ "$(cat $F/addr)" = "$B" ]'
$CTL off; reboot; $CTL on
check "lo stesso alla riaccensione" '[ "$(cat $ROOT/System/bluetooth/indirizzo.txt)" = "$B" ] && [ "$(cat $F/addr)" = "$B" ]'
$CTL off
echo "x" > $ROOT/System/bluetooth/indirizzo.txt
reboot; $CTL on
check "indirizzo rovinato: ne fa uno nuovo valido" 'grep -q -E "^2C(:[0-9A-F]{2}){5}$" $ROOT/System/bluetooth/indirizzo.txt'
$CTL off
reset

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
