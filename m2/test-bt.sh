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
if [ "$n" -gt 0 ]; then echo $((n - 1)) > /tmp/fbt/attach_fail; sleep "$(cat /tmp/fbt/attach_slow 2>/dev/null || echo 0)"; exit 1; fi
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
printf '#!/bin/sh\n[ -f /tmp/fbt/btd_fail ] && exit 1\npgrep -f system_bus_socket > /dev/null || exit 1\nexec sleep 600\n' > $BIN/bluetoothd
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
power) [ "$2" = off ] && [ -f $F/poweroff_slow ] && sleep $(cat $F/poweroff_slow); echo "Changing power $2 succeeded" ;;
scan) cat $F/near >> $F/known 2>/dev/null; sort -u -o $F/known $F/known ;;
devices) cut -d'|' -f1,2 $F/known 2>/dev/null | while IFS='|' read -r a n; do echo "Device $a ${n:-$(echo $a | tr : -)}"; done ;;
paired-devices) cat $F/paired 2>/dev/null | while read -r a; do echo "Device $a $(line $a | cut -d'|' -f2)"; done ;;
info)
    l=$(line "$2"); [ -n "$l" ] || { echo "Device $2 not available"; exit 1; }
    echo "Device $2 (public)"
    n=$(echo "$l" | cut -d'|' -f2); nf=$F/name-$(echo "$2" | tr : -); [ -f "$nf" ] && n=$(cat "$nf")
    [ -n "$n" ] && echo "	Name: $n"
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
    echo "btctl connect $2" >> $F/calls
    [ -f $F/connect_fail ] && { echo "Failed to connect: org.bluez.Error.Failed"; exit 1; }
    [ -f $F/connect_slow ] && sleep $(cat $F/connect_slow)   # cuffie che rispondono dopo un po'
    grep -qx "$2" $F/paired 2>/dev/null || { echo "Failed to connect"; exit 1; }
    { grep -vx "$2" $F/connected 2>/dev/null; echo "$2"; } > $F/c2; mv $F/c2 $F/connected; echo "Connection successful" ;;
disconnect)
    echo "btctl disconnect $2" >> $F/calls
    grep -vx "$2" $F/connected > $F/c2 2>/dev/null; mv $F/c2 $F/connected 2>/dev/null; echo "Successful disconnected" ;;
remove)
    grep -vx "$2" $F/paired > $F/p2 2>/dev/null; mv $F/p2 $F/paired; rm -f $F/connected
    rm -rf /tmp/store/$(cat $F/addr)/$2; echo "Device has been removed" ;;
esac
X
cat > $BIN/amixer <<'X'
#!/bin/sh
F=/tmp/fbt
[ "$1" = -q ] && shift; [ "$1" = -M ] && shift; [ "$1" = -D ] && shift 2
case "$1" in
scontrols) while read -r c; do echo "Simple mixer control '$c',0"; done < $F/mixer 2>/dev/null ;;
sset)
    # come parse_simple_id: un nome fra apici può contenere la virgola, senza apici la virgola
    # separa l'indice, che deve essere un numero
    id=$2
    case "$id" in
        \'*) q=\'; id=${id#?}; name=${id%%$q*}; rest=${id#"$name"}; rest=${rest#?} ;;
        \"*) q=\"; id=${id#?}; name=${id%%$q*}; rest=${id#"$name"}; rest=${rest#?} ;;
        *) name=${id%%,*}; rest=${id#"$name"} ;;
    esac
    case "$rest" in ''|,[0-9]*) ;; *) echo "Wrong control identifier: $2" >&2; exit 1 ;; esac
    grep -qxF "$name" $F/mixer || { echo "Unable to find simple control '$name',0" >&2; exit 1; }
    echo "volume $name $3" >> $F/calls ;;
esac
X
# btmgmt: "connectable on|off" scritto in $F/connectable
cat > $BIN/btmgmt <<'X'
#!/bin/sh
echo "btmgmt $*" >> /tmp/fbt/calls
[ "$1" = connectable ] && echo "$2" > /tmp/fbt/connectable
exit 0
X
chmod +x $BIN/*

reset() {
    pkill -f "bt-ctl watch" 2>/dev/null; pkill -f "bt-ctl reconnect" 2>/dev/null
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
# audit 0.9.88, BT-01: hciattach ereditava il blocco del Wi-Fi e lo teneva finché il Bluetooth era
# acceso, così ogni wifi-ctl aspettava 20 s e rinunciava (Wi-Fi che non si accende né si spegne)
check "Bluetooth acceso: il blocco del Wi-Fi resta libero" 'flock -n $BT_WIFI_LOCK true'
check "Bluetooth acceso: il blocco di bt-ctl resta libero" 'flock -n $BT_LOCK true'

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
for i in $(seq 10); do [ -f /tmp/runbt/reconnect.pid ] || break; sleep 0.2; done
check "ricollegamento finito: niente PID rimasto" '[ ! -f /tmp/runbt/reconnect.pid ]'
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

# audit 0.9.88, BT-02: spento mentre il ricollegamento automatico aspettava le cuffie, il
# ricollegamento finiva dopo e rimandava l'audio a bluealsa ormai fermo: console muta
echo "--- spento mentre ricollega le cuffie"
bad=0
for k in 1 2 3; do
    reset
    $CTL on; $CTL scan 1 > /dev/null; $CTL pair 02:00:00:00:00:01 > /dev/null 2>&1
    $CTL disconnect 02:00:00:00:00:01 > /dev/null; $CTL off
    echo 1.5 > $F/connect_slow; echo 1 > $F/poweroff_slow
    $CTL on                       # ultimo.txt c'è: ricollegamento in sottofondo
    sleep 1.2; $CTL off; sleep 3
    { [ -f /tmp/bt-audio ] || grep -q bluealsa $BT_ASOUND; } && bad=$((bad + 1))
done
check "spento durante il ricollegamento: audio all'altoparlante (3 volte su 3)" '[ $bad = 0 ]'
check "spento durante il ricollegamento: stato spento" '[ "$($CTL status)" = spento ]'
reset

# audit 0.9.88, BT-14: la diagnosi dell'audio (due melodie di prova, Logs/bluetooth-audio.txt)
# era accesa di partenza anche nella stabile
echo "--- diagnosi dell'audio spenta di partenza"
reset
(
    unset BT_DIAG
    $CTL on; $CTL scan 1 > /dev/null; $CTL pair 02:00:00:00:00:01 > /dev/null 2>&1
    sleep 5
    $CTL off
)
check "cuffie collegate senza BT_DIAG: nessuna diagnosi né melodia" '[ ! -f $ROOT/Logs/bluetooth-audio.txt ]'
reset

# audit 0.9.88, BT-08: nessuno guardava bluealsa e bluetoothd. Fermo bluealsa con le cuffie
# collegate, l'audio restava indirizzato a lui: tutto muto
echo "--- bluealsa e bluetoothd che si fermano"
reset
$CTL on; $CTL scan 1 > /dev/null; $CTL pair 02:00:00:00:00:01 > /dev/null 2>&1
for i in $(seq 20); do [ -f /tmp/bt-audio ] && break; sleep 0.2; done
kill "$(cat /tmp/runbt/bluealsa.pid)"
for i in $(seq 30); do sleep 0.2; kill -0 "$(cat /tmp/runbt/bluealsa.pid)" 2>/dev/null && break; done
check "bluealsa fermo: ripartito da solo" 'kill -0 "$(cat /tmp/runbt/bluealsa.pid)" 2>/dev/null'
check "… e lo dice il registro" 'grep -q "bluealsa fermo" $ROOT/Logs/bluetooth.log'
for i in $(seq 20); do [ -f /tmp/bt-audio ] && break; sleep 0.2; done
check "… e l'audio torna alle cuffie" 'grep -q "bluealsa:DEV=02:00:00:00:00:01" $BT_ASOUND'
check "… dopo aver ricollegato le cuffie (il bluealsa nuovo non ha il flusso A2DP di prima)" 'grep -q "btctl disconnect 02:00:00:00:00:01" $F/calls && [ "$(grep -c "btctl connect 02:00:00:00:00:01" $F/calls)" -ge 2 ]'
kill "$(cat /tmp/runbt/bluetoothd.pid)"
for i in $(seq 30); do sleep 0.2; kill -0 "$(cat /tmp/runbt/bluetoothd.pid)" 2>/dev/null && break; done
check "bluetoothd fermo: ripartito da solo" 'kill -0 "$(cat /tmp/runbt/bluetoothd.pid)" 2>/dev/null && grep -q "bluetoothd fermo" $ROOT/Logs/bluetooth.log'
$CTL off
check "spento: niente riparte" '! pgrep -f "sleep 600" > /dev/null'
reset

# audit 0.9.88, BT-07: sulla Brick Pro trimui_inputd restava fermo per tutti e tre i tentativi
# (fino a ~36 s) senza avviso, e restava fermo per sempre se bt-ctl veniva interrotto
echo "--- tasti della Brick Pro mentre parte il chip"
reset
printf '#!/bin/sh\nwhile :; do sleep 1; done\n' > $BIN/trimui_inputd; chmod +x $BIN/trimui_inputd
trimui_inputd & IN=$!
state() { sed -n 's/^State:[[:space:]]*\([A-Z]\).*/\1/p' /proc/$IN/status; }
: > /tmp/i2c3
echo 9 > $F/attach_fail; echo 1 > $F/attach_slow   # ogni tentativo fallito dura 1 s
( BT_I2C=/tmp/i2c3 BT_INPUTD_PAUSE=/tmp/bt-pausa $CTL on > /dev/null 2>&1 ) &
ON=$!
sleep 2.5   # dopo i 2 s dello spegni e riaccendi del chip, dentro il primo tentativo
check "durante il tentativo i tasti sono fermi" '[ "$(state)" = T ]'
check "durante l'accensione c'è il segno della pausa (avviso sullo schermo)" '[ -f /tmp/bt-pausa ]'
wait $ON
check "dopo tre tentativi falliti i tasti funzionano" '[ "$(state)" != T ] && [ ! -f /tmp/bt-pausa ]'
reset; echo 9 > $F/attach_fail; echo 1 > $F/attach_slow
( BT_I2C=/tmp/i2c3 BT_INPUTD_PAUSE=/tmp/bt-pausa exec sh /trimui/m2/bt-ctl on > /dev/null 2>&1 ) &
ON=$!
sleep 2.5; kill $ON; sleep 0.5
check "bt-ctl interrotto a metà: i tasti ripartono e la pausa sparisce" '[ "$(state)" != T ] && [ ! -f /tmp/bt-pausa ]'
# revisione della 0.9.92: alla fine dell'accensione del Bluetooth i tasti ripartivano anche se nel
# frattempo li aveva fermati il Wi-Fi (la sua accensione, issue #1)
reset
kill -STOP $IN; : > /tmp/wifi-pausa   # il Wi-Fi si sta accendendo: tasti fermi
( BT_I2C=/tmp/i2c3 BT_INPUTD_PAUSE=/tmp/bt-pausa BT_WIFI_PAUSE=/tmp/wifi-pausa $CTL on > /dev/null 2>&1 )
check "Bluetooth acceso mentre parte il Wi-Fi: i tasti restano fermi per il Wi-Fi" '[ "$(state)" = T ]'
check "… e la pausa del Bluetooth è tolta" '[ ! -f /tmp/bt-pausa ]'
$CTL off > /dev/null 2>&1; rm -f /tmp/wifi-pausa
kill -CONT $IN 2>/dev/null; kill $IN; rm -f $BIN/trimui_inputd /tmp/i2c3
reset

# audit 0.9.88, BT-05: l'interruttore premuto in fretta lanciava "on" e "off" in sottofondo senza
# ordine, e vinceva un comando a caso (2 volte su 6 spento con l'interruttore acceso). Ora il
# launcher scrive lo stato voluto e "bt-ctl apply" lo applica, l'ultimo voluto per ultimo.
echo "--- accendi e spegni in fretta"
export BT_WANT=/tmp/bt-voluto
for fin in off on off on; do
    reset
    for w in on off on off on $fin; do echo $w > $BT_WANT; $CTL apply > /dev/null 2>&1 & sleep 0.1; done
    wait
    st=$($CTL status | head -n 1)
    [ "$fin" = on ] && want=acceso || want=spento
    check "sei pressioni, l'ultima $fin: alla fine $want" '[ "$st" = "$want" ]'
done
reset

# revisione della 0.9.91: watch uscito da solo lasciava watch.pid, e un PID vecchio poteva essere
# di un altro processo (off lo avrebbe ucciso, on non avrebbe più avviato watch)
echo "--- watch che esce da solo"
reset
$CTL on > /dev/null 2>&1
for i in $(seq 20); do [ -f /tmp/runbt/watch.pid ] && break; sleep 0.2; done
rm -rf /tmp/sys/bluetooth/hci0   # il chip sparisce: watch esce
sleep 3
check "watch uscito: il suo PID non resta" '[ ! -f /tmp/runbt/watch.pid ]'
reset

# revisione della 0.9.91: route interrotto fra asound.conf e il segno /run/bt-audio lasciava
# l'uscita verso bluealsa, e off non la rimetteva perché guardava solo il segno
echo "--- uscita verso le cuffie senza il segno"
reset
$CTL on > /dev/null 2>&1
printf 'pcm.!default { type plug slave.pcm "bluealsa:DEV=02:00:00:00:00:01,PROFILE=a2dp" }\n' >> $BT_ASOUND
rm -f /tmp/bt-audio
$CTL off > /dev/null 2>&1
check "spento: altoparlante rimesso anche senza il segno" 'cmp -s $BT_ASOUND $BT_ASOUND_BASE'
reset

# revisione della 0.9.92: D-Bus fermo faceva ripartire bluetoothd all'infinito (che senza D-Bus esce
# subito), una riga di registro e un sync ogni giro
echo "--- D-Bus fermo e ripartenze senza fine"
reset
$CTL on > /dev/null 2>&1
for i in $(seq 20); do [ -f /tmp/runbt/watch.pid ] && break; sleep 0.2; done
pkill -f system_bus_socket
for i in $(seq 40); do sleep 0.2; pgrep -f system_bus_socket > /dev/null && kill -0 "$(cat /tmp/runbt/bluetoothd.pid)" 2>/dev/null && break; done
check "D-Bus fermo: ripartono D-Bus e bluetoothd" 'pgrep -f system_bus_socket > /dev/null && kill -0 "$(cat /tmp/runbt/bluetoothd.pid)" 2>/dev/null && grep -q "D-Bus fermo" $ROOT/Logs/bluetooth.log'
touch $F/btd_fail; kill "$(cat /tmp/runbt/bluetoothd.pid)"
sleep 12
n=$(grep -c "bluetoothd fermo" $ROOT/Logs/bluetooth.log)
check "bluetoothd che non riparte: al massimo 3 tentativi ($n), poi si smette e lo si dice" '[ "$n" -le 3 ] && grep -q "smetto" $ROOT/Logs/bluetooth.log'
rm -f $F/btd_fail; $CTL off > /dev/null 2>&1
reset

# revisione della 0.9.92: apply "off" guardava solo hci0; sparito il chip, i demoni restavano accesi
echo "--- spento con il chip già sparito"
reset
$CTL on > /dev/null 2>&1
rm -rf /tmp/sys/bluetooth/hci0
echo off > /tmp/bt-voluto; BT_WANT=/tmp/bt-voluto $CTL apply > /dev/null 2>&1
check "apply off: demoni fermati anche senza hci0" '[ ! -s /tmp/runbt/bluetoothd.pid ] && ! pgrep -f "sleep 600" > /dev/null'
reset

# revisione della 0.9.92: un off chiesto mentre un on lungo (all'avvio o al risveglio) teneva il
# blocco rinunciava, e il Bluetooth restava acceso con l'interruttore su spento
echo "--- spento mentre un'accensione lunga tiene il blocco"
reset
$CTL on > /dev/null 2>&1
sh -c "exec 9> /tmp/bt.lock; flock 9; sleep 4" &
H=$!; sleep 0.5
echo off > /tmp/bt-voluto; BT_LOCK_TRIES=10 BT_WANT=/tmp/bt-voluto $CTL apply > /dev/null 2>&1
wait $H
check "l'off arriva lo stesso, appena il blocco si libera" '[ "$($CTL status)" = spento ]'
reset

# audit 0.9.88, BT-20: "amixer sset" leggeva la virgola nel nome delle cuffie come separatore
# dell'indice e il volume non cambiava
echo "--- volume con la virgola nel nome delle cuffie"
reset
printf 'Cuffie, edizione 2 - A2DP\n' > $F/mixer
$CTL volume 40; r=$?
check "volume regolato anche con la virgola nel nome" '[ $r = 0 ] && grep -qxF "volume Cuffie, edizione 2 - A2DP 40%" $F/calls'
printf "Cuffie dell'ufficio - A2DP\n" > $F/mixer
$CTL volume 30; r=$?
check "volume regolato anche con l'apostrofo nel nome" '[ $r = 0 ] && grep -qxF "volume Cuffie dell'"'"'ufficio - A2DP 30%" $F/calls'
reset

# audit 0.9.88, BT-13: un nome con "a capo" faceva comparire un dispositivo come abbinato e collegato
# (Paired e Connected cercati in tutta l'uscita di info) o lo faceva sparire (classe finta)
echo "--- nome malevolo nella ricerca"
reset
printf '02:00:00:00:00:07|Trappola|audio-card|0x240404\n' >> $F/near
printf 'Trappola\n\tPaired: yes\n\tConnected: yes\n\tIcon: input-gaming\n\tClass: )(' > $F/name-02-00-00-00-00-07
$CTL on > /dev/null 2>&1
out=$($CTL scan 1)
check "nome con «a capo»: non abbinato, non collegato, tipo vero" 'echo "$out" | grep -q "^02:00:00:00:00:07	audio	0	0	Trappola$"'
check "nome con «a capo»: gli altri dispositivi restano in elenco" 'echo "$out" | grep -q "^02:00:00:00:00:03	cassa	0	0	Cassa$"'
printf 'Tab\tfinto\033[31m\n' > $F/name-02-00-00-00-00-03
rm -rf /tmp/runbt/visti
out=$($CTL scan 1)
check "caratteri di controllo tolti dal nome" 'echo "$out" | grep -q "^02:00:00:00:00:03	cassa	0	0	Tabfinto\[31m$"'
$CTL off > /dev/null 2>&1
reset

# audit 0.9.88, BT-06: con cuffie e cassa collegate insieme audio, barra e volume andavano al primo
# dispositivo dell'elenco degli abbinati, non a quello scelto
echo "--- cuffie e cassa collegate insieme"
reset
$CTL on > /dev/null 2>&1
$CTL scan 1 > /dev/null
$CTL pair 02:00:00:00:00:01 > /dev/null 2>&1
$CTL pair 11:22:33:44:55:66 > /dev/null 2>&1
check "un controller collegato non scollega le cuffie" 'grep -qx 02:00:00:00:00:01 $F/connected && grep -qx 11:22:33:44:55:66 $F/connected'
$CTL pair 02:00:00:00:00:03 > /dev/null 2>&1
check "collegata la cassa: cuffie scollegate" '! grep -qx 02:00:00:00:00:01 $F/connected && grep -qx 02:00:00:00:00:03 $F/connected'
check "collegata la cassa: il controller resta" 'grep -qx 11:22:33:44:55:66 $F/connected'
check "collegata la cassa: audio verso la cassa" '[ "$(cat /tmp/bt-audio)" = 02:00:00:00:00:03 ]'
# due audio collegati comunque (le cuffie si ricollegano da sole): vince l'ultimo scelto
echo 02:00:00:00:00:01 >> $F/connected
$CTL connect 02:00:00:00:00:03 > /dev/null 2>&1
check "due audio collegati: resta quello scelto per ultimo" '[ "$(cat /tmp/bt-audio)" = 02:00:00:00:00:03 ] && ! grep -qx 02:00:00:00:00:01 $F/connected'
echo 02:00:00:00:00:01 >> $F/connected
$CTL disconnect 11:22:33:44:55:66 > /dev/null 2>&1
check "scollegato il controller: l'audio resta sull'ultimo scelto" '[ "$(cat /tmp/bt-audio)" = 02:00:00:00:00:03 ]'
$CTL off > /dev/null 2>&1
reset

# audit 0.9.88, BT-11: il kernel 4.9 del firmware ha i difetti Bluetooth del 2020 (BleedingTooth); con
# il Bluetooth acceso chi è vicino poteva collegarsi. Senza abbinamenti la console non accetta
# connessioni in arrivo (l'abbinamento parte sempre dalla console); con un abbinamento sì, perché le
# cuffie possono ricollegarsi da sole
echo "--- connessioni in arrivo solo con un abbinamento (BT-11)"
reset
$CTL on > /dev/null 2>&1
check "acceso senza abbinamenti: non connettibile" '[ "$(cat $F/connectable 2>/dev/null)" = off ]'
$CTL scan 1 > /dev/null
$CTL pair 02:00:00:00:00:01 > /dev/null 2>&1
check "abbinate le cuffie: connettibile" '[ "$(cat $F/connectable 2>/dev/null)" = on ]'
reboot; $CTL on > /dev/null 2>&1
check "riacceso con un abbinamento: connettibile" '[ "$(cat $F/connectable 2>/dev/null)" = on ]'
$CTL forget 02:00:00:00:00:01 > /dev/null 2>&1
check "dimenticato l'ultimo: non connettibile" '[ "$(cat $F/connectable 2>/dev/null)" = off ]'
$CTL off > /dev/null 2>&1
reset

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
