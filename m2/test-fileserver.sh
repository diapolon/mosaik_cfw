#!/bin/sh
# Prova del trasferimento file via Wi-Fi (frontend/src/launcher/fileserver.c), con il modulo vero
# e un client HTTP vero:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-fileserver.sh
set -e
apk add -q build-base zlib-dev curl unzip zip python3 zbar imagemagick >/dev/null
W=/tmp/fs; rm -rf $W; mkdir -p $W/sd/System $W/sd/Roms/FBNEO $W/sd/Saves/fbneo $W/sd/Cores $W/sd/Linux $W/sd/Media; cd $W
printf 'network={ssid="x" psk="segreto"}\n' > sd/System/wifi.conf
echo '{}' > sd/System/settings.json
echo salvataggio > sd/Saves/fbneo/gioco.srm
echo core > sd/Cores/core.so
: > "sd/Roms/FBNEO/.vecchio.zip.part"   # avanzo di un caricamento interrotto

cat > main.c <<'C'
#include "fileserver.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
const char *T(const char *k) { return k; }
const char *plat_model(void) { return "prova"; }
const char *update_current(void) { return "0.0.0"; }
static volatile sig_atomic_t fine;
static void stop(int s) { (void)s; fine = 1; }
int main(int argc, char **argv) {
    (void)argc;
    signal(SIGTERM, stop);
    if (!fs_start(argv[1])) return 1;
    printf("porta %d\n", fs_port());
    fflush(stdout);
    while (!fine) {   /* le risposte della console al ripristino arrivano come file "si" e "no" */
        usleep(100000);
        if (access("si", F_OK) == 0) { unlink("si"); fs_restore_answer(FS_PARTS_ALL); }
        if (access("no", F_OK) == 0) { unlink("no"); fs_restore_answer(0); }
        if (access("salvataggi", F_OK) == 0) { unlink("salvataggi"); fs_restore_answer(1 << FS_PART_SAVES); }
        fs_status_t r;
        fs_get_status(&r);
        FILE *f = fopen("stato.tmp", "w");
        if (f) { fprintf(f, "%d %d %d %d\n", r.restore, r.restore_bk.files, r.restore_bk.count[FS_PART_SAVES], r.restore_bk.count[FS_PART_SETTINGS]); fclose(f); rename("stato.tmp", "stato"); }
    }
    fs_status_t s;
    fs_get_status(&s);
    fs_stop();
    printf("caricati %d, cambiato %d\n", s.uploads, fs_changed());
    return 0;
}
C
S=/trimui/frontend/src
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -D_GNU_SOURCE -Wno-format-truncation -fsanitize=undefined -fsanitize-undefined-trap-on-error -I$S/launcher -I$S/common \
    -o srv main.c $S/launcher/fileserver.c $S/launcher/zipread.c -lz -lpthread
ARCADE_FS_PORT=8099 ARCADE_FS_PIN=123456 ./srv $W/sd > srv.log 2>&1 &
PID=$!
sleep 1
U=http://127.0.0.1:8099; J=$W/cj
ok=0; ko=0
check() {   # check NOME ATTESO OTTENUTO
    if [ "$2" = "$3" ]; then ok=$((ok+1)); echo "  ok: $1"; else ko=$((ko+1)); echo "  ERRORE: $1: atteso [$2], ottenuto [$3]"; fi
}
q() { curl -s -b $J "$@"; }

check "avanzo .part tolto all'apertura" "no" "$(ls -a sd/Roms/FBNEO | grep -q part && echo si || echo no)"
check "pagina senza PIN" "200" "$(curl -s -o /dev/null -w '%{http_code}' $U/)"
check "elenco senza PIN" '{"error":"auth"}' "$(curl -s "$U/api/list?path=")"
check "PIN sbagliato" '{"error":"badpin"}' "$(curl -s -X POST -d pin=000000 $U/api/login)"
check "PIN giusto" '{"ok":true}' "$(curl -s -c $J -X POST -d pin=123456 $U/api/login)"
check "wifi.conf non elencato" "no" "$(q "$U/api/list?path=System" | grep -q wifi && echo si || echo no)"
check "wifi.conf non scaricabile" '{"error":"path"}' "$(q "$U/api/get?path=System/wifi.conf")"
check "wifi.conf non nello zip" "no" "$(q -o s.zip "$U/api/zip?path=System"; unzip -l s.zip | grep -q wifi && echo si || echo no)"
check "fuori dalla cartella" '{"error":"path"}' "$(q "$U/api/list?path=..")"
check "fuori, nel mezzo" '{"error":"path"}' "$(q "$U/api/get?path=Saves/../System/wifi.conf")"
check "cartella principale non scrivibile" "false" "$(q "$U/api/list?path=" | sed 's/.*"writable":\([a-z]*\).*/\1/')"
head -c 3000000 /dev/urandom > big.zip
check "caricamento" '{"ok":true}' "$(q -T big.zip "$U/api/put?path=Roms/FBNEO&name=gioco%20uno.zip")"
check "file identico" "0" "$(cmp -s big.zip "sd/Roms/FBNEO/gioco uno.zip"; echo $?)"
check "già presente" '{"error":"exists"}' "$(q -T big.zip "$U/api/put?path=Roms/FBNEO&name=gioco%20uno.zip")"
check "sostituzione" '{"ok":true}' "$(q -T big.zip "$U/api/put?path=Roms/FBNEO&name=gioco%20uno.zip&overwrite=1")"
echo x > x.txt
check "sola lettura (Cores)" '{"error":"readonly"}' "$(q -T x.txt "$U/api/put?path=Cores&name=x.txt")"
check "sola lettura (System)" '{"error":"readonly"}' "$(q -T x.txt "$U/api/put?path=System&name=settings.json&overwrite=1")"
check "nome con punto iniziale" '{"error":"path"}' "$(q -T x.txt "$U/api/put?path=Roms&name=.nascosto")"
check "nome con caratteri vietati" '{"error":"path"}' "$(q -T x.txt "$U/api/put?path=Roms&name=a%3Ab.zip")"
check "cartella con sottocartelle" '{"ok":true}' "$(q -T x.txt "$U/api/put?path=Linux&name=Gioco/dati/x.txt")"
check "sottocartelle create" "yes" "$([ -f sd/Linux/Gioco/dati/x.txt ] && echo yes)"
check "nome accentato" '{"ok":true}' "$(q -T x.txt "$U/api/put?path=Media&name=caf%C3%A8.png")"
check "nuova cartella" '{"ok":true}' "$(q -X POST "$U/api/mkdir?path=Roms&name=NUOVA")"
check "rinomina" '{"ok":true}' "$(q -X POST "$U/api/rename?path=Roms/NUOVA&to=Nuova2")"
check "solo maiuscole" '{"ok":true}' "$(q -X POST "$U/api/rename?path=Roms/Nuova2&to=NUOVA2")"
check "rinomina su un nome esistente" '{"error":"exists"}' "$(q -X POST "$U/api/rename?path=Roms/NUOVA2&to=FBNEO")"
check "elimina cartella piena" '{"ok":true}' "$(q -X POST "$U/api/delete?path=Linux/Gioco")"
check "cartella eliminata" "no" "$([ -e sd/Linux/Gioco ] && echo si || echo no)"
check "eliminare una cartella principale" '{"error":"path"}' "$(q -X POST "$U/api/delete?path=Roms")"
check "rinominare una cartella principale" '{"error":"path"}' "$(q -X POST "$U/api/rename?path=Saves&to=X")"
check "scarica un salvataggio" "salvataggio" "$(q "$U/api/get?path=Saves/fbneo/gioco.srm")"
q -o saves.zip "$U/api/zip?path=Saves"
check "zip dei salvataggi valido" "0" "$(unzip -tq saves.zip >/dev/null 2>&1; echo $?)"
check "zip con il salvataggio" "salvataggio" "$(unzip -p saves.zip Saves/fbneo/gioco.srm)"
check "nessun .part rimasto" "0" "$(find sd -name '*.part' | wc -l | tr -d ' ')"
for i in 1 2 3 4 5; do curl -s -X POST -d pin=999999 $U/api/login >/dev/null; done
check "PIN bloccato dopo 5 errori" '{"error":"locked"}' "$(curl -s -X POST -d pin=123456 $U/api/login)"
check "sessione ancora valida" "200" "$(q -o /dev/null -w '%{http_code}' "$U/api/info")"
sleep 31   # fine del blocco
check "ricorda il browser" '{"ok":true}' "$(curl -s -c $W/cr -X POST -d 'pin=123456&remember=1' $U/api/login)"
R=$(grep mosaikr $W/cr | awk '{print $7}')
check "codice ricordato lungo 32" "32" "${#R}"
check "entra col solo codice ricordato" "200" "$(curl -s -o /dev/null -w '%{http_code}' -H "Cookie: mosaikr=$R" $U/api/info)"
check "codice falso" '{"error":"auth"}' "$(curl -s -H 'Cookie: mosaikr=00000000000000000000000000000000' $U/api/info)"
check "file dei browser salvato" "1" "$(wc -l < sd/System/fileserver-browsers.txt | tr -d ' ')"
check "file dei browser invisibile" '{"error":"path"}' "$(q "$U/api/get?path=System/fileserver-browsers.txt")"
RST() { cut -d' ' -f1 stato; }   # FS_RS_*: 0 niente, 1 in attesa, 3 fatto
wait_rst() { for i in $(seq 50); do [ "$(RST)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
# backup: salvataggi, impostazioni, BIOS; il Wi-Fi solo se chiesto; i core mai
mkdir -p sd/Bios sd/System/collections; echo bios > sd/Bios/neogeo.zip; echo 'fbneo	gioco' > sd/System/collections/Belli.txt; echo stats > sd/System/stats.tsv
q -o b.zip "$U/api/backup"
check "backup valido" "0" "$(unzip -tq b.zip >/dev/null 2>&1; echo $?)"
check "backup con il riepilogo" "Mosaik backup" "$(unzip -p b.zip mosaik-backup.txt | head -1)"
check "backup con salvataggi, BIOS, collezioni" "3" "$(unzip -l b.zip | grep -c 'Saves/fbneo/gioco.srm\|Bios/neogeo.zip\|System/collections/Belli.txt')"
check "backup senza Wi-Fi" "0" "$(unzip -l b.zip | grep -c wifi.conf)"
check "backup senza core" "0" "$(unzip -l b.zip | grep -c Cores)"
check "backup con il Wi-Fi se chiesto" "1" "$(q -o bw.zip "$U/api/backup?wifi=1"; unzip -l bw.zip | grep -c System/wifi.conf)"
check "backup senza giochi" "0" "$(unzip -l b.zip | grep -c ' Roms/')"
# il peso è quello dello zip: i file più le intestazioni
check "peso dei giochi nella pagina" "$(find sd/Roms sd/Linux -type f ! -name '.*' -exec stat -c %s {} + | awk '{t+=$1} END {print int(t/10000)}')" "$(q "$U/api/info" | sed 's/.*"games":\([0-9]*\).*/\1/' | awk '{print int($1/10000)}')"
q -o bg.zip "$U/api/backup?games=1"
check "backup con i giochi se chiesto" "0" "$(unzip -p bg.zip 'Roms/FBNEO/gioco uno.zip' | cmp -s - big.zip; echo $?)"
# ripristino: il salvataggio cambiato torna com'era, solo dopo la conferma
echo cambiato > sd/Saves/fbneo/gioco.srm
check "ripristino ricevuto" "true" "$(q -T b.zip "$U/api/restore" | sed 's/.*"ok":\([a-z]*\).*/\1/')"
wait_rst 1
check "in attesa della console" "1" "$(RST)"
check "nulla scritto prima della conferma" "cambiato" "$(cat sd/Saves/fbneo/gioco.srm)"
touch si; wait_rst 3
check "ripristino fatto" "3" "$(RST)"
check "salvataggio ripristinato" "salvataggio" "$(cat sd/Saves/fbneo/gioco.srm)"
check "zip del ripristino tolto" "no" "$([ -e sd/System/.ripristino.zip ] && echo si || echo no)"
check "nessun .part dal ripristino" "0" "$(find sd -name '*.part' | wc -l | tr -d ' ')"
# annullato dalla console: nulla cambia
echo cambiato > sd/Saves/fbneo/gioco.srm
q -T b.zip "$U/api/restore" >/dev/null; wait_rst 1; touch no; wait_rst 0
check "ripristino annullato" "cambiato" "$(cat sd/Saves/fbneo/gioco.srm)"
# uno zip qualsiasi non è un backup
check "zip qualsiasi rifiutato" '{"error":"notbackup"}' "$(q -T saves.zip "$U/api/restore")"
# backup ricompresso (deflate) da un altro programma, con voci che non devono passare
mkdir -p rz && cd rz && unzip -q ../b.zip && mkdir -p Cores && echo malevolo > Cores/core.so && echo vecchio > Saves/fbneo/gioco.srm && zip -qr -9 ../r.zip . && cd ..
echo cambiato > sd/Saves/fbneo/gioco.srm
q -T r.zip "$U/api/restore" >/dev/null; wait_rst 1; touch si; wait_rst 3
check "backup compresso ripristinato" "vecchio" "$(cat sd/Saves/fbneo/gioco.srm)"
check "core non toccato" "core" "$(cat sd/Cores/core.so)"
# backup con i giochi: la ROM tolta torna
rm "sd/Roms/FBNEO/gioco uno.zip"
q -T bg.zip "$U/api/restore" >/dev/null; wait_rst 1; touch si; wait_rst 3
check "ROM ripristinata dal backup" "0" "$(cmp -s "sd/Roms/FBNEO/gioco uno.zip" big.zip; echo $?)"
# solo alcune parti: con i soli salvataggi le impostazioni non cambiano
echo cambiato > sd/Saves/fbneo/gioco.srm; echo cambiate > sd/System/stats.tsv
q -T b.zip "$U/api/restore" >/dev/null; wait_rst 1
check "parti contate (salvataggi)" "1" "$(cut -d' ' -f3 stato)"
touch salvataggi; wait_rst 3
check "salvataggio ripristinato da solo" "salvataggio" "$(cat sd/Saves/fbneo/gioco.srm)"
check "impostazioni non toccate" "cambiate" "$(cat sd/System/stats.tsv)"
check "file dei browser non elencato" "no" "$(q "$U/api/list?path=System" | grep -q browsers && echo si || echo no)"
# mosaik.local: domanda mDNS da un'altra porta (come fanno i programmi semplici), risposta diretta
cat > mdns.py <<'P'
import socket, struct, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
q = struct.pack('>HHHHHH', 0x1234, 0, 1, 0, 0, 0) + b'\x06mosaik\x05local\x00' + struct.pack('>HH', 1, 1)
s.sendto(q, ('127.0.0.1', 5353))
try: d, _ = s.recvfrom(1500)
except Exception: print('nessuna risposta'); sys.exit()
print('%d.%d.%d.%d' % tuple(d[-4:]) if struct.unpack('>H', d[:2])[0] == 0x1234 else 'id sbagliato')
P
check "mosaik.local risponde con l'indirizzo" "$(ip -4 addr show eth0 | awk '/inet /{print $2}' | cut -d/ -f1)" "$(python3 mdns.py)"
kill $PID; wait $PID || true
check "chiusura e riepilogo" "caricati 4, cambiato 1" "$(tail -1 srv.log)"
# backup copiato sulla scheda e importato alla prima schermata, senza il server
cat > imp.c <<'C'
#include "fileserver.h"
#include <stdio.h>
#include <stdlib.h>
const char *T(const char *k) { return k; }
const char *plat_model(void) { return "prova"; }
const char *update_current(void) { return "0.0.0"; }
int main(int argc, char **argv) {
    fs_backup_t bk;
    if (argc < 3 || !fs_backup_info(argv[2], &bk)) { puts("non un backup"); return 1; }
    printf("%d %s\n", bk.files, fs_backup_import(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : FS_PARTS_ALL) ? "importato" : "errore");
    return 0;
}
C
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -D_GNU_SOURCE -Wno-format-truncation -fsanitize=undefined -fsanitize-undefined-trap-on-error -I$S/launcher -I$S/common \
    -o imp imp.c $S/launcher/fileserver.c $S/launcher/zipread.c -lz -lpthread
check "zip qualsiasi: non un backup" "non un backup" "$(./imp $W/sd saves.zip 2>&1)"
echo cambiato > sd/Saves/fbneo/gioco.srm
rm -rf sd/Saves/nuovo
mkdir -p rz/Saves/nuovo && echo nuovo > rz/Saves/nuovo/a.srm && rm -f r2.zip && (cd rz && zip -qr -9 ../r2.zip .)
check "backup importato" "importato" "$(./imp $W/sd r2.zip 2>&1 | cut -d' ' -f2)"
check "salvataggio importato" "vecchio" "$(cat sd/Saves/fbneo/gioco.srm)"
check "cartella nuova creata" "nuovo" "$(cat sd/Saves/nuovo/a.srm)"
check "core non toccato dall'importazione" "core" "$(cat sd/Cores/core.so)"
check "nessun .part dall'importazione" "0" "$(find sd -name '*.part' | wc -l | tr -d ' ')"
echo cambiato > sd/Saves/fbneo/gioco.srm; echo cambiato > sd/Saves/nuovo/a.srm
check "importazione dei soli BIOS" "importato" "$(./imp $W/sd r2.zip 4 2>&1 | cut -d' ' -f2)"
check "salvataggi non toccati dall'importazione parziale" "cambiato" "$(cat sd/Saves/fbneo/gioco.srm)"
# backup con un file successivo rovinato (CRC sbagliato): niente sostituito (verifica audit BUG-007)
echo cambiato > sd/Saves/fbneo/gioco.srm
rm -rf rz3; cp -R rz rz3; mkdir -p rz3/Saves/zz && echo b > rz3/Saves/zz/b.srm && rm -f r3.zip && (cd rz3 && zip -qr -9 ../r3.zip .)
python3 - r3.zip <<'P'
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, "rb").read())
i = b.find(b"PK\x01\x02")
while i >= 0:
    nl = struct.unpack("<H", b[i + 28:i + 30])[0]
    if b[i + 46:i + 46 + nl] == b"Saves/zz/b.srm": b[i + 16:i + 20] = struct.pack("<I", 12345)
    i = b.find(b"PK\x01\x02", i + 4)
open(p, "wb").write(bytes(b))
P
check "backup con un file rovinato: rifiutato" "errore" "$(./imp $W/sd r3.zip 2>&1 | cut -d' ' -f2)"
check "file rovinato: nessun salvataggio sostituito" "cambiato" "$(cat sd/Saves/fbneo/gioco.srm)"
check "file rovinato: nessun temporaneo rimasto" "0" "$(find sd -name '*.part' | wc -l | tr -d ' ')"
check "file rovinato: niente scritto del file guasto" "no" "$([ -e sd/Saves/zz/b.srm ] && echo si || echo no)"
head -c 300 r2.zip > tronco.zip
check "zip tronco: non un backup" "non un backup" "$(./imp $W/sd tronco.zip 2>&1)"
# codice QR: testi di varie lunghezze, letti da zbarimg
cat > qr.c <<'C'
#include "qrcode.h"
#include <stdio.h>
int main(int argc, char **argv) {
    static unsigned char m[QR_MAX_SIDE * QR_MAX_SIDE];
    if (argc < 3) return 2;
    int s = qr_encode(argv[1], m), k = 6, q = 4, w = (s + 2 * q) * k;
    if (!s) return 1;
    FILE *f = fopen(argv[2], "wb");
    fprintf(f, "P5 %d %d 255\n", w, w);
    for (int y = 0; y < w; y++)
        for (int x = 0; x < w; x++) {
            int r = y / k - q, c = x / k - q;
            fputc(r >= 0 && c >= 0 && r < s && c < s && m[r * s + c] ? 0 : 255, f);
        }
    fclose(f);
    return 0;
}
C
gcc -O1 -Wall -Wextra -Werror -I$S/launcher -o qr qr.c $S/launcher/qrcode.c
for t in "http://192.168.1.23/#pin=123456" "http://10.0.0.200:8080/#pin=000001" "a" \
         "$(head -c 60 /dev/urandom | base64 | tr -d '\n' | head -c 80)" "$(head -c 200 /dev/urandom | base64 | tr -d '\n' | head -c 200)"; do
    ./qr "$t" q.pgm
    check "QR di ${#t} caratteri" "$t" "$(zbarimg -q --raw q.pgm 2>/dev/null)"
done
if grep -q "runtime error\|Illegal instruction" srv.log; then ko=$((ko+1)); echo "  ERRORE: sanitizer"; cat srv.log; fi
echo
if [ $ko -eq 0 ]; then echo "TUTTO OK ($ok prove)"; else echo "$ko PROVE FALLITE"; exit 1; fi
