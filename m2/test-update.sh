#!/bin/sh
# Prova dell'aggiornamento via Wi-Fi in un Linux arm64, con il server vero e il modulo vero:
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-update.sh
set -e
apk add -q build-base curl-dev openssl-dev python3 openssl coreutils >/dev/null
W=/tmp/upd; rm -rf $W; mkdir -p $W/src; cd $W
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; exit 1; }

openssl ecparam -name prime256v1 -genkey -noout -out key.pem 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out altra.pem 2>/dev/null
cp /trimui/frontend/src/launcher/update.c /trimui/frontend/src/launcher/update.h /trimui/frontend/src/launcher/games.h src/
{ echo 'static const char UPDATE_PUBKEY[] ='; openssl ec -in key.pem -pubout 2>/dev/null | sed 's/.*/"&\\n"/'; echo ';'; } > src/update_pubkey.h
cat > src/t.c <<'C'
#include "update.h"
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ui_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
/* emulatori: "md" è una console (il suo indice lo scrive lo scraper), "fbneo" no */
#include "games.h"
static emulator_t md_emu = { .id = "md", .console = true };
const emulator_t *emulator_find(const char *id) { return !strcmp(id, "md") ? &md_emu : NULL; }
int main(int c, char **v) {
    if (c > 2 && !strcmp(v[2], "notify")) {
        update_notify_start(v[1], 0);
        for (int i = 0; i < 60 && !update_notify_version()[0]; i++) usleep(100000);
        puts(update_notify_version()[0] ? update_notify_version() : "niente");
        return 0;
    }
    if (c > 2 && !strcmp(v[2], "resume-notify")) {   /* prima la ripresa (alla prima schermata), poi l'avviso (AGG-01) */
        update_resume_start(v[1]);
        int tries = 0;
        while (!update_notify_start(v[1], 0) && tries++ < 100) usleep(50000);
        for (int i = 0; i < 60 && !update_notify_version()[0]; i++) usleep(100000);
        puts(update_notify_version()[0] ? update_notify_version() : "niente");
        return 0;
    }
    if (c > 2 && !strcmp(v[2], "resume")) {
        update_resume_start(v[1]); update_join();
        const update_status_t *r = update_status();
        if (c > 3 && r->phase == UPD_READY) { update_apply(); update_join(); }
        printf("%d %d %d\n", r->phase, r->error, r->files);
        return 0;
    }
    if (c > 2 && !strcmp(v[2], "download")) {
        update_check(v[1]); update_join();
        if (update_status()->phase == UPD_AVAILABLE) { update_install(); update_join(); }
        printf("%d %d %d\n", update_status()->phase, update_status()->error, update_status()->files);
        return 0;
    }
    if (c > 2 && !strcmp(v[2], "confirm")) { update_confirm_boot(v[1]); printf("%d\n", (int)update_boot_failed()); return 0; }
    update_check(v[1]); update_join();
    const update_status_t *s = update_status();
    if (c > 2 && !strcmp(v[2], "reinstall")) { printf("%d\n", s->phase == UPD_AVAILABLE ? (int)s->reinstall : -1); return 0; }
    if (c > 2 && s->phase == UPD_AVAILABLE) { update_install(); update_join(); if (s->phase == UPD_READY) { if (getenv("TEST_DISCARD")) update_discard(); else update_apply(); update_join(); } }
    printf("%d %d %d\n", s->phase, s->error, s->files);
    return 0;
}
C
gcc -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DUPDATE_REAL -DUPDATE_TEST -DARCADE_VERSION='"0.0.0"' -DSYS_BLOCK_DIR='"/tmp/upd/block"' -o t src/t.c src/update.c -lcurl -lcrypto -lpthread
echo "0.8.0 20260101.0000" > old.txt; echo "0.9.0 20270101.0000" > same.txt; echo "1.0.0 20300101.0000" > newer.txt

# pacchetto sul "Mac"
D=$W/dist; A=$D/SD/Arcade; mkdir -p $A/System $A/Lang $A/Cores $A/Themes/t $A/Dats/history $A/Emulators $A/Filters $A/Bezels
head -c 3000000 /dev/urandom > $D/arcade-system.squashfs
echo "0.9.0 20270101.0000" > $D/arcade-version.txt
echo certs > $A/System/cacert.pem; echo '{"it":1}' > $A/Lang/it.json; head -c 5000000 /dev/urandom > $A/Cores/a.so
echo tema > $A/Themes/t/theme.json; echo storia > $A/Dats/history/x.txt; echo pacchetto > $A/Dats/arcadedb.tsv
# indice arcade fatto dai DAT: solo <id>.roms qui (un file in meno da contare); indice di una
# console e dati libretro scritti dallo scraper: non vanno nell'elenco
echo roms > $A/Dats/fbneo.roms; echo console > $A/Dats/md.tsv; mkdir -p $A/Dats/libretro/genre; echo dat > $A/Dats/libretro/genre/x.dat

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
expect "$(./t $W/sd resume-notify 2>/dev/null)" "0.9.0 20270101.0000" "avviso anche dopo la ripresa di uno scaricamento (audit 0.9.72, AGG-01)"
card
expect "$(./t $W/sd notify 2>/dev/null)" "0.9.0 20270101.0000" "versione nuova segnalata"
[ -f $S/System/update-notice.txt ] && [ ! -e $S/System/update-pending ] && cmp $W/vecchio $S/System/arcade-system.squashfs && ok "controllo senza toccare nulla"
echo "http://127.0.0.1:1/" > $S/System/update-url.txt
expect "$(./t $W/sd notify 2>/dev/null)" "0.9.0 20270101.0000" "entro un giorno non ricontrolla e ricorda l'avviso"
card
echo "--- errore a metà: si rimette tutto com'era"
card
mkdir -p $S/Themes/t/theme.json
expect "$(./t $W/sd install 2>/dev/null)" "11 6 6" "aggiornamento fermato"
[ "$(cat $S/Lang/it.json)" = vecchio ] && ok "file già sostituito tornato com'era" || ko "file non ripristinato: $(cat $S/Lang/it.json)"
cmp $W/vecchio $S/System/arcade-system.squashfs && ok "il sistema resta il vecchio"
[ ! -e $S/Cores/a.so ] && ok "file nuovi tolti" || ko "file nuovo rimasto"
[ -z "$(find $S -name '*.annulla' -o -name '*.nuovo')" ] && ok "nessun file provvisorio" || ko "file provvisori rimasti: $(find $S -name '*.annulla' -o -name '*.nuovo')"
[ ! -e $S/System/update-journal.txt ] && ok "nessun giornale dopo l'errore" || ko "giornale rimasto"
card
echo "--- errore a metà e un originale che non torna: il giornale resta (audit A56-001)"
mkdir -p $S/Themes/t/theme.json
expect "$(UPDATE_UNDO_FAIL=Lang/it.json ./t $W/sd install 2>/dev/null)" "11 8 6" "aggiornamento fermato, ripristino incompleto"
J=$S/System/update-journal.txt
grep -qx 'stato applico' $J && grep -qx 'file Lang/it.json' $J && ok "giornale tenuto per system-init" || ko "giornale: $(cat $J 2>&1)"
[ "$(cat $S/Lang/it.json.annulla)" = vecchio ] && ok "l'originale resta come .annulla" || ko "originale perso"
cmp $W/vecchio $S/System/arcade-system.squashfs && ok "il sistema resta il vecchio"
expect "$(./t $W/sd install 2>/dev/null)" "11 8 6" "altra installazione rifiutata finché c'è il giornale"
[ "$(cat $S/Lang/it.json.annulla)" = vecchio ] && grep -qx 'stato applico' $J && ok "giornale e originale non toccati" || ko "giornale riscritto"
card
echo "--- impossibile scrivere il segno degli avvii di prova"
mkdir $S/System/update-pending
expect "$(./t $W/sd install 2>/dev/null)" "11 6 6" "aggiornamento fermato"
cmp $W/vecchio $S/System/arcade-system.squashfs && [ ! -e $S/System/arcade-system.squashfs.prec ] && ok "sistema precedente rimesso a posto"
[ "$(cat $S/Lang/it.json)" = vecchio ] && ok "anche i dati tornano com'erano" || ko "dati non ripristinati"
echo "--- manca il sistema principale: la console gira sulla copia precedente"
card
mv $S/System/arcade-system.squashfs $S/System/arcade-system.squashfs.prec
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato senza sistema principale"
cmp $D/arcade-system.squashfs $S/System/arcade-system.squashfs && ok "sistema nuovo al suo posto"
cmp $W/vecchio $S/System/arcade-system.squashfs.prec && ok "copia precedente non cancellata"
echo "--- sistema principale rovinato: la console gira sulla copia precedente"
SF=$S/System/arcade-system.squashfs
in_uso() { rm -rf $W/block; mkdir -p $W/block/${2:-loop0}/loop; echo "/mnt/SDCARD/Arcade/System/$1" > $W/block/${2:-loop0}/loop/backing_file; }
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
echo "--- sistema in uso su un dispositivo loop oltre i primi otto"
card; cp $SF $SF.prec; echo rovinato > $SF; in_uso arcade-system.squashfs.prec loop12
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato"
cmp $W/vecchio $SF.prec && ok "la copia buona resta quella precedente"
echo "--- due dispositivi col sistema: vale quello montato come radice"
card; cp $SF $SF.prec; echo rovinato > $SF; rm -rf $W/block
mkdir -p $W/block/loop0/loop $W/block/loop5/loop
echo /mnt/SDCARD/Arcade/System/arcade-system.squashfs > $W/block/loop0/loop/backing_file; echo 7:0 > $W/block/loop0/dev
echo /mnt/SDCARD/Arcade/System/arcade-system.squashfs.prec > $W/block/loop5/loop/backing_file; stat -c '%Hd:%Ld' / > $W/block/loop5/dev
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato"
cmp $W/vecchio $SF.prec && ok "riconosciuto il sistema della radice: copia buona conservata" || ko "copia buona sostituita"
echo "--- due dispositivi col sistema, nessuno è la radice: non si sa quale"
card; echo copia-buona > $SF.prec; rm -rf $W/block
mkdir -p $W/block/loop0/loop $W/block/loop5/loop
echo /mnt/SDCARD/Arcade/System/arcade-system.squashfs > $W/block/loop0/loop/backing_file
echo /mnt/SDCARD/Arcade/System/arcade-system.squashfs.prec > $W/block/loop5/loop/backing_file
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato"
[ "$(cat $SF.prec)" = copia-buona ] && ok "copia precedente conservata" || ko "copia precedente sostituita"
echo "--- non si sa quale sistema è in uso: la copia precedente non si tocca"
card; echo copia-buona > $SF.prec; rm -rf $W/block; mkdir -p $W/block/loop0/loop
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "installato"
[ "$(cat $SF.prec)" = copia-buona ] && ok "copia precedente conservata" || ko "copia precedente sostituita"
cmp $D/arcade-system.squashfs $SF && ok "sistema nuovo al suo posto"
card; rm -rf $W/block; mkdir -p $W/block
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "senza copia precedente: installato"
cmp $W/vecchio $SF.prec && ok "il sistema di prima diventa la copia precedente"
rm -rf $W/block
card
expect "$(./t $W/sd install 2>/dev/null)" "10 0 6" "fine installazione, 6 file"
cmp $D/arcade-system.squashfs $S/System/arcade-system.squashfs && ok "sistema nuovo identico"
[ -z "$(find $S -name '*.nuovo')" ] && ok "nessun file scaricato rimasto dopo l'installazione" || ko "file .nuovo rimasti"
[ "$(cat $S/Lang/it.json.annulla 2>/dev/null)" = vecchio ] && ok "l'originale resta come .annulla fino alla conferma (R03)" || ko "copia .annulla mancante"
J=$S/System/update-journal.txt
grep -qx 'stato installato' $J && grep -qx 'version 0.9.0 20270101.0000' $J && grep -qx 'file Lang/it.json' $J && grep -qx 'nuovo Cores/a.so' $J \
    && ok "giornale: versione, stato, file sostituiti e file nuovi" || ko "giornale: $(cat $J 2>&1)"
grep -qx 'sistema si' $J && ok "giornale: aggiornamento con il sistema nuovo, \"sistema si\" (AGG-12)" || ko "giornale senza \"sistema si\": $(cat $J 2>&1)"
cmp $W/vecchio $S/System/arcade-system.squashfs.prec && ok "sistema vecchio conservato"
cmp $A/Cores/a.so $S/Cores/a.so && cmp $A/Lang/it.json $S/Lang/it.json && cmp $A/Themes/t/theme.json $S/Themes/t/theme.json && ok "dati aggiornati"
cmp $A/Dats/fbneo.roms $S/Dats/fbneo.roms && [ ! -e $S/Dats/md.tsv ] && [ ! -e $S/Dats/libretro ] && ok "indice arcade sì, indice della console e dati libretro no" || ko "indici sbagliati"
[ "$(cat $S/System/settings.json)" = '{"mio":1}' ] && [ -f $S/Roms/a.zip ] && ok "impostazioni e ROM intatte"
[ -f $S/System/update-pending ] && ok "segno di prova del sistema nuovo"
[ ! -e $S/Dats/history ] && [ "$(cat $S/Dats/arcadedb.tsv)" = scaricato ] && ok "dati dello scraper non toccati"
[ -z "$(find $S -name '*.nuovo')" ] && ok "nessun file provvisorio rimasto"

echo "--- conferma del primo avvio: i core nuovi si devono caricare (R03)"
export ARCADE_VERSION_FILE=$W/same.txt
expect "$(./t $W/sd confirm 2>/dev/null)" "1" "core che non si carica: niente conferma"
[ "$(cat $S/System/update-pending)" = 2 ] && [ -f $J ] && [ -f $S/Lang/it.json.annulla ] && ok "prossimo avvio = terza prova: si torna indietro, copie e giornale restano" || ko "dopo il core rotto: $(cat $S/System/update-pending)"
printf 'unsigned retro_api_version(void) { return 1; }\n' > $W/core.c && gcc -shared -fPIC -o $S/Cores/a.so $W/core.c
expect "$(./t $W/sd confirm 2>/dev/null)" "0" "core caricabile: confermato"
[ ! -e $S/System/update-pending ] && [ ! -e $J ] && [ -z "$(find $S -name '*.annulla')" ] && ok "conferma: via segno di prova, copie e giornale" || ko "dopo la conferma: $(ls $S/System; find $S -name '*.annulla')"
cp $A/Cores/a.so $S/Cores/a.so

echo "--- seconda volta: già aggiornato"
export ARCADE_VERSION_FILE=$W/same.txt
expect "$(./t $W/sd install 2>/dev/null)" "4 0 0" "stessa versione"
echo "--- versione offerta più vecchia"
export ARCADE_VERSION_FILE=$W/newer.txt
expect "$(./t $W/sd install 2>/dev/null)" "4 0 0" "niente ritorno indietro"

echo "--- cambia solo una lingua: il sistema non si tocca"
export ARCADE_VERSION_FILE=$W/old.txt
rm -f $S/System/update-pending $S/System/arcade-system.squashfs.prec; echo modificata > $S/Lang/it.json
before=$(cd $S && find . -type f | sort | xargs md5sum | md5sum)
expect "$(./t $W/sd 2>/dev/null)" "5 0 6" "versione nuova annunciata subito, col massimo di file (confronto solo dopo Scarica)"
[ "$(cd $S && find . -type f | sort | xargs md5sum | md5sum)" = "$before" ] && ok "il controllo non tocca la scheda"
expect "$(./t $W/sd install 2>/dev/null)" "10 0 1" "un solo file"
cmp $A/Lang/it.json $S/Lang/it.json && [ ! -e $S/System/arcade-system.squashfs.prec ] && ok "sistema non toccato"
[ -f $S/System/update-pending ] && grep -qx 'file Lang/it.json' $S/System/update-journal.txt && ok "anche senza sistema nuovo: avvio di prova e giornale" || ko "aggiornamento dei soli dati senza prova"
grep -qx 'sistema no' $S/System/update-journal.txt && ok "soli dati: \"sistema no\" nel giornale, system-init non scambia il sistema (AGG-12)" || ko "soli dati senza \"sistema no\": $(cat $S/System/update-journal.txt 2>&1)"
rm -f $S/System/update-pending $S/System/update-journal.txt   # come dopo la conferma
echo "--- versione nuova ma file già tutti uguali"
expect "$(./t $W/sd install 2>/dev/null)" "4 0 0" "dopo il confronto: già aggiornato, niente scaricato"
[ ! -e $S/System/update-pending ] && ok "nessun avvio di prova"
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
for bad in ../fuori.txt /etc/passwd Roms/a.zip System/settings.json Lang/../../x Dats/arcadedb.tsv Dats/history/a.txt Dats/md.tsv Dats/libretro/genre/x.dat; do
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

echo "--- invito a riscrivere la SD (area di avvio vecchia)"
card; export ARCADE_VERSION_FILE=$W/old.txt
python3 /trimui/toolchain/update-server.py --dist $D --http-port 8765 --udp-port 8766 --key key.pem --reinstall 2 >/dev/null & SRV=$!
for i in $(seq 50); do nc -z 127.0.0.1 8765 2>/dev/null && break; sleep 0.1; done
expect "$(./t $W/sd reinstall 2>/dev/null)" "1" "scheda senza generazione: invito"
echo 1 > $S/System/area-avvio.txt
expect "$(./t $W/sd reinstall 2>/dev/null)" "1" "generazione 1 sotto la soglia 2: invito"
echo 2 > $S/System/area-avvio.txt
expect "$(./t $W/sd reinstall 2>/dev/null)" "0" "generazione alla soglia: niente invito"
stop
serve key.pem
rm -f $S/System/area-avvio.txt
expect "$(./t $W/sd reinstall 2>/dev/null)" "0" "soglia 0 (nessuna riga reinstall): niente invito"
stop

echo "--- pausa di prova chiesta dall'elenco firmato (R03)"
card; export ARCADE_VERSION_FILE=$W/old.txt
MOSAIK_PROVA_PAUSA=1 serve key.pem
out=$(./t $W/sd install 2>$W/pausa.log)
expect "$out" "10 0 6" "installato dopo la pausa"
grep -q "pausa di 1 s" $W/pausa.log && ok "pausa fatta" || ko "pausa non fatta: $(cat $W/pausa.log | tail -3)"
stop

echo "--- scaricato e non installato: ripreso all'avvio senza rete"
card; export ARCADE_VERSION_FILE=$W/old.txt; serve key.pem
expect "$(./t $W/sd download 2>/dev/null)" "8 0 6" "scaricato, poi la console si spegne"
[ -f $S/System/update-ready.txt ] && [ -f $S/System/update-ready.sig ] && [ -f $S/Lang/it.json.nuovo ] && ok "elenco firmato tenuto da parte con i file scaricati" || ko "niente da riprendere: $(ls $S/System)"
stop
echo "http://127.0.0.1:1/" > $S/System/update-url.txt   # nessuna rete
expect "$(./t $W/sd resume 2>/dev/null)" "8 0 6" "all'avvio: pronto da installare, senza rete"
expect "$(./t $W/sd resume apply 2>/dev/null)" "10 0 6" "installato dal ripreso"
cmp $D/arcade-system.squashfs $SF && cmp $A/Lang/it.json $S/Lang/it.json && [ ! -e $S/System/update-ready.txt ] && [ -z "$(find $S -name '*.nuovo')" ] && ok "sistema e dati nuovi, niente rimasto da parte" || ko "dopo l'installazione ripresa"
echo "--- scaricato ma rovinato: si toglie"
card; serve key.pem
expect "$(./t $W/sd download 2>/dev/null)" "8 0 6" "scaricato"
stop
echo rovinato > $S/Lang/it.json.nuovo
expect "$(./t $W/sd resume 2>/dev/null)" "0 0 6" "file non integro: niente domanda"
[ ! -e $S/System/update-ready.txt ] && [ -z "$(find $S -name '*.nuovo')" ] && ok "elenco e file scaricati tolti" || ko "rimasti: $(find $S -name '*.nuovo')"
echo "--- versione già installata: lo scaricato si toglie"
card; serve key.pem
./t $W/sd download >/dev/null 2>&1
stop
export ARCADE_VERSION_FILE=$W/same.txt
expect "$(./t $W/sd resume 2>/dev/null | cut -d' ' -f1)" "0" "niente domanda"
[ -z "$(find $S -name '*.nuovo')" ] && [ ! -e $S/System/update-ready.txt ] && ok "tolto" || ko "rimasto"
export ARCADE_VERSION_FILE=$W/old.txt
echo "--- file .nuovo abbandonati (rete caduta, nessun elenco): tolti all'avvio"
card; mkdir -p $S/Themes/t; echo x > $S/Lang/xx.json.nuovo; echo y > $S/Themes/t/a.png.nuovo; echo r > $S/Roms/gioco.nuovo
./t $W/sd resume >/dev/null 2>&1
[ ! -e $S/Lang/xx.json.nuovo ] && [ ! -e $S/Themes/t/a.png.nuovo ] && ok "tolti nelle cartelle del sistema" || ko "rimasti"
[ -f $S/Roms/gioco.nuovo ] && ok "le ROM non si toccano" || ko "toccato un file in Roms"
echo "--- scaricamento ripreso: file già intero e file a metà"
card; serve key.pem
cp $D/arcade-system.squashfs $SF.nuovo
head -c 2500000 $A/Cores/a.so > $S/Cores/a.so.nuovo
expect "$(./t $W/sd install 2>$W/ripresa.log)" "10 0 6" "installato"
grep -q "arcade-system.squashfs già scaricato" $W/ripresa.log && ok "file già intero non riscaricato" || ko "riscaricato: $(grep aggiornamento $W/ripresa.log | head)"
grep -q "a.so, riprendo da 2500000" $W/ripresa.log && ok "file a metà ripreso (o ricominciato se il server non lo permette)" || ko "file a metà: $(grep a.so $W/ripresa.log)"
cmp $A/Cores/a.so $S/Cores/a.so && ok "core giusto dopo la ripresa" || ko "core sbagliato"
stop

# audit 0.9.88, AGG-14: uno scaricamento a metà (rete caduta) non sopravviveva allo spegnimento: il
# controllo all'avvio toglieva i .nuovo e con Wi-Fi instabile si ripartiva ogni volta da zero
echo "--- scaricamento a metà, console spenta, poi ripreso"
cat > $W/taglia.py <<'PY'
# server con Range; con TAGLIA=N il file-0 si interrompe dopo N byte (rete che cade)
import http.server, os, sys
root, cut = sys.argv[1], int(os.environ.get("TAGLIA", "0"))
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        p = os.path.join(root, self.path.lstrip("/"))
        if not os.path.isfile(p): self.send_error(404); return
        data = open(p, "rb").read(); start = 0
        r = self.headers.get("Range")
        if r and r.startswith("bytes="): start = int(r[6:].split("-")[0])
        self.send_response(206 if start else 200)
        self.send_header("Content-Length", str(len(data) - start))
        if start: self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
        self.end_headers()
        body = data[start:]
        if cut and os.path.basename(p) == "file-0": body = body[:cut]
        self.wfile.write(body)
http.server.HTTPServer(("127.0.0.1", 8765), H).serve_forever()
PY
cut_serve() { TAGLIA=$2 python3 $W/taglia.py $1 & SRV=$!; for i in $(seq 50); do nc -z 127.0.0.1 8765 2>/dev/null && break; sleep 0.1; done; }
Y=$W/meta; rm -rf $Y; mkdir -p $Y; head -c 3000000 /dev/urandom > $Y/file-0; hy=$(sha256sum $Y/file-0 | cut -d' ' -f1)
printf 'arcade-update 1\nversion 0.9.0 20270101.0000\nfile %s 3000000 Cores/grande.so\n' $hy > $Y/update.txt
openssl dgst -sha256 -sign key.pem -out $Y/update.sig $Y/update.txt
card; echo "http://127.0.0.1:8765/" > $S/System/update-url.txt; export ARCADE_VERSION_FILE=$W/old.txt
cut_serve $Y 1000000
./t $W/sd download > /dev/null 2>&1 || true
stop
[ "$(stat -c %s $S/Cores/grande.so.nuovo 2>/dev/null)" = 1000000 ] && ok "rete caduta: 1000000 byte scaricati" || ko "scaricamento a metà non preparato: $(ls -la $S/Cores)"
echo "http://127.0.0.1:1/" > $S/System/update-url.txt   # spenta e riaccesa, ancora senza rete
./t $W/sd resume > /dev/null 2>&1
[ "$(stat -c %s $S/Cores/grande.so.nuovo 2>/dev/null)" = 1000000 ] && ok "dopo lo spegnimento la parte scaricata resta" || ko "parte scaricata tolta all'avvio"
echo "http://127.0.0.1:8765/" > $S/System/update-url.txt
cut_serve $Y 0
expect "$(./t $W/sd install 2>$W/meta.log)" "10 0 1" "installato"
stop
grep -q "grande.so, riprendo da 1000000" $W/meta.log && ok "ripreso da dov'era" || ko "non ripreso: $(grep grande $W/meta.log)"
cmp $Y/file-0 $S/Cores/grande.so && ok "file giusto dopo la ripresa" || ko "file sbagliato"
[ ! -e $S/System/update-partial.txt ] && ok "nessun elenco a metà rimasto dopo l'installazione" || ko "elenco a metà rimasto"
echo "--- a metà di una versione, poi ne esce un'altra: la parte vecchia non si usa"
card; echo "http://127.0.0.1:8765/" > $S/System/update-url.txt
cut_serve $Y 1000000; ./t $W/sd download > /dev/null 2>&1 || true; stop
Z=$W/meta2; rm -rf $Z; mkdir -p $Z; head -c 3000000 /dev/urandom > $Z/file-0; hz=$(sha256sum $Z/file-0 | cut -d' ' -f1)
printf 'arcade-update 1\nversion 0.9.1 20270201.0000\nfile %s 3000000 Cores/grande.so\n' $hz > $Z/update.txt
openssl dgst -sha256 -sign key.pem -out $Z/update.sig $Z/update.txt
cut_serve $Z 0
expect "$(./t $W/sd install 2>$W/meta2.log)" "10 0 1" "versione nuova installata"
stop
! grep -q "grande.so, riprendo da" $W/meta2.log && ok "la parte della versione di prima non viene ripresa" || ko "ripresa una parte di un'altra versione"
cmp $Z/file-0 $S/Cores/grande.so && ok "file della versione nuova" || ko "file sbagliato"

# revisione indipendente della 0.9.99, punto 3: con lo scaricamento a metà tenuto, il controllo dello
# spazio chiedeva tutta la misura dei file, senza contare la parte già scaricata, e con UPE_SPACE i
# file restavano: «spazio insufficiente» per sempre, con lo spazio occupato proprio da loro
echo "--- scaricamento a metà su una scheda quasi piena"
small_card() {   # scheda piccola (tmpfs, $1 KB) con la parte già scaricata e il suo elenco
    umount $W/small 2>/dev/null || true; mkdir -p $W/small
    mount -t tmpfs -o size=${1}k tmpfs $W/small || return 1
    mkdir -p $W/small/System $W/small/Cores
    echo "http://127.0.0.1:8765/" > $W/small/System/update-url.txt
    head -c 1000000 $Y/file-0 > $W/small/Cores/grande.so.nuovo
    cp $Y/update.txt $W/small/System/update-partial.txt; cp $Y/update.sig $W/small/System/update-partial.sig
}
if small_card 4400; then
    export ARCADE_VERSION_FILE=$W/old.txt; cut_serve $Y 0
    out=$(./t $W/small install 2>$W/small.log)
    check_ok=$([ "$out" = "10 0 1" ] && cmp -s $Y/file-0 $W/small/Cores/grande.so && echo si || echo no)
    [ "$check_ok" = si ] && ok "c'è posto per la parte che manca: si riprende e si installa" || ko "spazio contato male: $out $(grep -i spazio $W/small.log)"
    stop
    small_card 2600; cut_serve $Y 0
    ./t $W/small install > /dev/null 2>&1 || true
    stop
    [ ! -e $W/small/Cores/grande.so.nuovo ] && [ ! -e $W/small/System/update-partial.txt ] && ok "spazio insufficiente: la parte scaricata si toglie, e lo spazio torna" || ko "parte scaricata rimasta: $(ls $W/small/Cores $W/small/System)"
    umount $W/small
else
    echo "  (scheda piccola non provata: serve docker --privileged)"
fi

echo "--- nessun server"
rm -f $S/System/update-url.txt
expect "$(./t $W/sd install 2>/dev/null)" "3 0 0" "nessun server"
echo TUTTO OK
