#!/bin/sh
# Prova dell'aggiornamento via Wi-Fi in un Linux arm64, con il server vero e il modulo vero:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-update.sh
set -e
apk add -q build-base curl-dev openssl-dev python3 openssl coreutils >/dev/null
W=/tmp/upd; rm -rf $W; mkdir -p $W/src; cd $W
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; exit 1; }

openssl ecparam -name prime256v1 -genkey -noout -out key.pem 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out altra.pem 2>/dev/null
cp /trimui/frontend/src/launcher/update.c /trimui/frontend/src/launcher/update.h src/
{ echo 'static const char UPDATE_PUBKEY[] ='; openssl ec -in key.pem -pubout 2>/dev/null | sed 's/.*/"&\\n"/'; echo ';'; } > src/update_pubkey.h
cat > src/t.c <<'C'
#include "update.h"
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ui_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
int main(int c, char **v) {
    if (c > 2 && !strcmp(v[2], "notify")) {
        update_notify_start(v[1], 0);
        for (int i = 0; i < 60 && !update_notify_version()[0]; i++) usleep(100000);
        puts(update_notify_version()[0] ? update_notify_version() : "niente");
        return 0;
    }
    update_check(v[1]); update_join();
    const update_status_t *s = update_status();
    if (c > 2 && s->phase == UPD_AVAILABLE) { update_install(); update_join(); if (s->phase == UPD_READY) { if (getenv("TEST_DISCARD")) update_discard(); else update_apply(); update_join(); } }
    printf("%d %d %d\n", s->phase, s->error, s->files);
    return 0;
}
C
gcc -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DUPDATE_REAL -DARCADE_VERSION='"0.0.0"' -DSYS_BLOCK_DIR='"/tmp/upd/block"' -o t src/t.c src/update.c -lcurl -lcrypto -lpthread
echo "0.8.0 20260101.0000" > old.txt; echo "0.9.0 20270101.0000" > same.txt; echo "1.0.0 20300101.0000" > newer.txt

# pacchetto sul "Mac"
D=$W/dist; A=$D/SD/Arcade; mkdir -p $A/System $A/Lang $A/Cores $A/Themes/t $A/Dats/history $A/Emulators $A/Filters $A/Bezels
head -c 3000000 /dev/urandom > $D/arcade-system.squashfs
echo "0.9.0 20270101.0000" > $D/arcade-version.txt
echo certs > $A/System/cacert.pem; echo '{"it":1}' > $A/Lang/it.json; head -c 5000000 /dev/urandom > $A/Cores/a.so
echo tema > $A/Themes/t/theme.json; echo idx > $A/Dats/fbneo.tsv; echo storia > $A/Dats/history/x.txt; echo pacchetto > $A/Dats/arcadedb.tsv

card() {  # scheda con il sistema vecchio e i dati dell'utente
    rm -rf $W/sd; S=$W/sd; mkdir -p $S/System $S/Lang $S/Cores $S/Roms
    head -c 1000000 /dev/urandom > $S/System/arcade-system.squashfs
    cp $S/System/arcade-system.squashfs $W/vecchio
    echo vecchio > $S/Lang/it.json; echo '{"mio":1}' > $S/System/settings.json; echo rom > $S/Roms/a.zip
    mkdir -p $S/Dats; echo scaricato > $S/Dats/arcadedb.tsv
    echo "http://127.0.0.1:8765/" > $S/System/update-url.txt
}
serve() { python3 /trimui/toolchain/update-server.py --dist $D --http-port 8765 --udp-port 8766 --key "$1" >/dev/null & SRV=$!; for i in $(seq 50); do nc -z 127.0.0.1 8765 2>/dev/null && break; sleep 0.1; done; }
stop() { kill $SRV 2>/dev/null; wait $SRV 2>/dev/null || true; }
expect() { [ "$1" = "$2" ] && ok "$3 ($1)" || ko "$3: atteso $2, ottenuto $1"; }

echo "--- installazione completa, indirizzo da update-url.txt"
card; serve key.pem
export ARCADE_VERSION_FILE=$W/old.txt
cp -a $W/sd $W/sd-prima
expect "$(TEST_DISCARD=1 ./t $W/sd install 2>/dev/null)" "11 7 6" "annullato alla conferma"
[ -z "$(find $W/sd -name '*.nuovo')" ] || ko "file provvisori rimasti dopo l'annullamento"
# le cartelle vuote create per i file provvisori non contano: sulle schede vere esistono già
sums() { (cd "$1" && find . -type f | sort | xargs sha256sum); }
[ "$(sums $W/sd)" = "$(sums $W/sd-prima)" ] || { diff -r $W/sd $W/sd-prima; ko "file cambiati dopo l'annullamento"; }
ok "annullando la scheda resta com'era"
rm -rf $W/sd-prima
echo "--- controllo silenzioso degli aggiornamenti"
card
expect "$(./t $W/sd notify 2>/dev/null)" "0.9.0 20270101.0000" "versione nuova segnalata"
[ -f $S/System/update-notice.txt ] && [ ! -e $S/System/update-pending ] && cmp $W/vecchio $S/System/arcade-system.squashfs && ok "controllo senza toccare nulla"
echo "http://127.0.0.1:1/" > $S/System/update-url.txt
expect "$(./t $W/sd notify 2>/dev/null)" "0.9.0 20270101.0000" "entro un giorno non ricontrolla e ricorda l'avviso"
card
echo "--- errore a metà: limite noto, l'installazione resta parziale (vedi TODO)"
card
mkdir -p $S/Themes/t/theme.json
expect "$(./t $W/sd install 2>/dev/null)" "11 6 6" "aggiornamento fermato"
cmp $A/Lang/it.json $S/Lang/it.json && cmp $W/vecchio $S/System/arcade-system.squashfs && ok "file già sostituiti restano, il sistema resta il vecchio"
card
echo "--- impossibile scrivere il segno degli avvii di prova"
mkdir $S/System/update-pending
expect "$(./t $W/sd install 2>/dev/null)" "11 6 6" "aggiornamento fermato"
cmp $W/vecchio $S/System/arcade-system.squashfs && [ ! -e $S/System/arcade-system.squashfs.prec ] && ok "sistema precedente rimesso a posto"
echo "--- manca il sistema principale: la console gira sulla copia precedente"
card
mv $S/System/arcade-system.squashfs $S/System/arcade-system.squashfs.prec
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato senza sistema principale"
cmp $D/arcade-system.squashfs $S/System/arcade-system.squashfs && ok "sistema nuovo al suo posto"
cmp $W/vecchio $S/System/arcade-system.squashfs.prec && ok "copia precedente non cancellata"
echo "--- sistema principale rovinato: la console gira sulla copia precedente"
SF=$S/System/arcade-system.squashfs
in_uso() { mkdir -p $W/block/loop0/loop; echo "/mnt/SDCARD/Arcade/System/$1" > $W/block/loop0/loop/backing_file; }
card; cp $SF $SF.prec; echo rovinato > $SF; cp $SF $W/rovinato; in_uso arcade-system.squashfs.prec
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato sopra il sistema rovinato"
cmp $D/arcade-system.squashfs $SF && ok "sistema nuovo al suo posto"
cmp $W/vecchio $SF.prec && ok "la copia buona resta quella precedente"
card; cp $SF $SF.prec; echo rovinato > $SF; in_uso arcade-system.squashfs.prec
mkdir $S/System/update-pending
expect "$(./t $W/sd install 2>/dev/null)" "11 6 6" "aggiornamento fermato"
cmp $W/rovinato $SF && cmp $W/vecchio $SF.prec && ok "se si ferma, i due file restano com'erano"
echo "--- la console gira sul sistema principale: diventa la copia precedente"
card; echo copia-vecchia > $SF.prec; in_uso arcade-system.squashfs
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato"
cmp $W/vecchio $SF.prec && ok "il sistema in uso diventa la copia precedente"
rm -rf $W/block
card
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "fine installazione, 6 file"
cmp $D/arcade-system.squashfs $S/System/arcade-system.squashfs && ok "sistema nuovo identico"
cmp $W/vecchio $S/System/arcade-system.squashfs.prec && ok "sistema vecchio conservato"
cmp $A/Cores/a.so $S/Cores/a.so && cmp $A/Lang/it.json $S/Lang/it.json && cmp $A/Themes/t/theme.json $S/Themes/t/theme.json && ok "dati aggiornati"
[ "$(cat $S/System/settings.json)" = '{"mio":1}' ] && [ -f $S/Roms/a.zip ] && ok "impostazioni e ROM intatte"
[ -f $S/System/update-pending ] && ok "segno di prova del sistema nuovo"
[ ! -e $S/Dats/history ] && [ "$(cat $S/Dats/arcadedb.tsv)" = scaricato ] && ok "dati dello scraper non toccati"
[ -z "$(find $S -name '*.nuovo')" ] && ok "nessun file provvisorio rimasto"

echo "--- seconda volta: già aggiornato"
export ARCADE_VERSION_FILE=$W/same.txt
expect "$(./t $W/sd install 2>/dev/null)" "4 0 0" "stessa versione"
echo "--- versione offerta più vecchia"
export ARCADE_VERSION_FILE=$W/newer.txt
expect "$(./t $W/sd install 2>/dev/null)" "4 0 0" "niente ritorno indietro"

echo "--- cambia solo una lingua: il sistema non si tocca"
export ARCADE_VERSION_FILE=$W/old.txt
rm -f $S/System/update-pending $S/System/arcade-system.squashfs.prec; echo modificata > $S/Lang/it.json
expect "$(./t $W/sd install 2>/dev/null)" "10 0 1" "un solo file"
cmp $A/Lang/it.json $S/Lang/it.json && [ ! -e $S/System/update-pending ] && [ ! -e $S/System/arcade-system.squashfs.prec ] && ok "sistema non toccato"
stop

echo "--- firma con un'altra chiave"
card; before=$(cd $S && find . -type f | sort | xargs md5sum | md5sum); serve altra.pem
expect "$(./t $W/sd install 2>/dev/null)" "11 2 0" "firma rifiutata"
[ "$(cd $S && find . -type f | sort | xargs md5sum | md5sum)" = "$before" ] && ok "scheda intatta"
stop

mk_static() {  # $1 cartella con update.txt e files/: firma con la chiave giusta
    openssl dgst -sha256 -sign key.pem -out $1/update.sig $1/update.txt
    python3 -m http.server --directory $1 8765 >/dev/null 2>&1 & SRV=$!
    for i in $(seq 50); do nc -z 127.0.0.1 8765 2>/dev/null && break; sleep 0.1; done
}
echo "--- file alterato dopo la firma (server statico, indirizzo da update-url.txt)"
card; echo "http://127.0.0.1:8765/" > $S/System/update-url.txt
before=$(cd $S && find . -type f | sort | xargs md5sum | md5sum)
X=$W/st; rm -rf $X; mkdir -p $X; echo buono > $X/buono; h=$(sha256sum $X/buono | cut -d' ' -f1)
printf 'arcade-update 1\nversion 0.9.0 20270101.0000\nfile %s 6 Lang/it.json\n' $h > $X/update.txt
echo cattivo > $X/file-0; mk_static $X
expect "$(./t $W/sd install 2>/dev/null)" "11 5 1" "impronta sbagliata rifiutata"
[ "$(cd $S && find . -type f | sort | xargs md5sum | md5sum)" = "$before" ] && ok "scheda intatta"
stop

echo "--- elenco firmato che punta fuori dalle cartelle del sistema"
for bad in ../fuori.txt /etc/passwd Roms/a.zip System/settings.json Lang/../../x Dats/arcadedb.tsv Dats/history/a.txt; do
    printf 'arcade-update 1\nversion 0.9.0 20270101.0000\nfile %s 6 %s\n' $h "$bad" > $X/update.txt; mk_static $X
    expect "$(./t $W/sd install 2>/dev/null)" "11 3 0" "$bad"
    stop
done

echo "--- righe che questa versione non conosce"
card; echo "http://127.0.0.1:8765/" > $S/System/update-url.txt; cp $X/buono $X/file-0
printf 'arcade-update 1\nminboot 2\nversion 0.9.0 20270101.0000\nfile %s 6 Lang/it.json\nqualcosa di nuovo\n' $h > $X/update.txt; mk_static $X
expect "$(./t $W/sd install 2>/dev/null)" "10 0 1" "riga sconosciuta ignorata"
cmp $X/buono $S/Lang/it.json && ok "file installato"
stop
card; echo "http://127.0.0.1:8765/" > $S/System/update-url.txt
printf 'arcade-update 1\nversion 0.9.0 20270101.0000\nfile nonvalida\n' > $X/update.txt; mk_static $X
expect "$(./t $W/sd install 2>/dev/null)" "11 3 0" "riga file malformata rifiutata"
stop

echo "--- file pubblicati online: statici, dietro un reindirizzamento come GitHub"
card; export ARCADE_VERSION_FILE=$W/old.txt
python3 /trimui/toolchain/update-server.py --dist $D --key key.pem --out $W/pub >/dev/null
python3 -m http.server --directory $W/pub 8770 >/dev/null 2>&1 & SRV2=$!
cat > $W/redir.py <<'PY'
import http.server
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(302); self.send_header("Location", "http://127.0.0.1:8770" + self.path); self.end_headers()
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("", 8765), H).serve_forever()
PY
python3 $W/redir.py & SRV=$!
for p in 8765 8770; do for i in $(seq 50); do nc -z 127.0.0.1 $p 2>/dev/null && break; sleep 0.1; done; done
echo "http://127.0.0.1:8765/" > $S/System/update-url.txt
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installata da file statici"
cmp $D/arcade-system.squashfs $S/System/arcade-system.squashfs && cmp $A/Cores/a.so $S/Cores/a.so && ok "file identici"
stop; kill $SRV2 2>/dev/null; wait $SRV2 2>/dev/null || true
rm -f $S/System/update-url.txt

echo "--- nessun server"
rm -f $S/System/update-url.txt
expect "$(./t $W/sd install 2>/dev/null)" "3 0 0" "nessun server"
echo TUTTO OK
