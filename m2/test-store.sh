#!/bin/sh
# Prova dello Store (frontend/src/launcher/store.c) con il modulo vero, un server HTTP locale,
# una chiave di prova e pacchetti fatti da toolchain/make-store.py:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-store.sh
set -e
apk add -q build-base curl-dev openssl-dev zlib-dev python3 openssl >/dev/null
W=/tmp/store; rm -rf $W; mkdir -p $W/src $W/sd/System $W/sd/Emulators $W/sd/Cores $W/www; cd $W
ok=0; ko=0
check() { if [ "$2" = "$3" ]; then ok=$((ok+1)); echo "  ok: $1"; else ko=$((ko+1)); echo "  ERRORE: $1: atteso [$2], ottenuto [$3]"; fi; }
L=/trimui/frontend/src/launcher

openssl ecparam -name prime256v1 -genkey -noout -out key.pem 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out altra.pem 2>/dev/null
cp $L/store.c $L/store.h $L/zipread.c $L/zipread.h src/
mkdir -p src/../common; cp /trimui/frontend/src/common/json.c /trimui/frontend/src/common/json.h /trimui/frontend/src/common/durable.h common/ 2>/dev/null || { mkdir -p common; cp /trimui/frontend/src/common/json.c /trimui/frontend/src/common/json.h /trimui/frontend/src/common/durable.h common/; }
{ echo 'static const char UPDATE_PUBKEY[] ='; openssl ec -in key.pem -pubout 2>/dev/null | sed 's/.*/"&\\n"/'; echo ';'; } > src/update_pubkey.h

# core libretro finto ma vero (si carica) e uno rotto
printf 'unsigned retro_api_version(void) { return 1; }\n' > core.c
gcc -shared -fPIC -o good_libretro.so core.c
echo "non sono un core" > bad_libretro.so

cat > src/t.c <<'C'
#include "store.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
void ui_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
static int find(const char *id) { for (int i = 0; i < store_count(); i++) if (!strcmp(store_pkg(i)->id, id)) return i; return -1; }
int main(int c, char **v) {
    const char *root = v[1], *cmd = v[2];
    if (!strcmp(cmd, "recover")) { store_recover(root); return 0; }
    if (!strcmp(cmd, "pending")) { printf("%d\n", store_restore_pending(root)); return 0; }
    if (!strcmp(cmd, "restore")) {
        store_open(root, 0, true);
        store_join();
        store_install_restore();
        store_join();
        printf("%d %d\n", store_status()->phase, store_status()->error);
        return 0;
    }
    if (!strcmp(cmd, "offline")) {
        char p[1200], n[64], l[32];
        if (!store_offline_find(root, p, sizeof p, n, sizeof n, l, sizeof l)) { puts("niente"); return 0; }
        int r = store_offline_install(root, p);
        printf("%s %s\n", n, r == SE_NONE ? "installato" : r == SE_UNSIGNED ? "non verificato" : "errore");
        return 0;
    }
    store_lang(getenv("LINGUA") ? getenv("LINGUA") : "en");
    store_open(root, 0, strcmp(cmd, "cache") != 0);
    store_join();
    const store_status_t *s = store_status();
    if (!strcmp(cmd, "list") || !strcmp(cmd, "cache")) {
        printf("%d %d %d", s->phase, s->error, store_count());
        for (int i = 0; i < store_count(); i++) printf(" %s:%d", store_pkg(i)->id, store_pkg(i)->state);
        puts("");
        return 0;
    }
    int i = find(v[3]);
    if (i < 0) { puts("manca"); return 0; }
    if (!strcmp(cmd, "title")) { printf("%s|%s|%s\n", store_pkg(i)->title, store_pkg(i)->systems, store_pkg(i)->category); return 0; }
    if (!strcmp(cmd, "usage")) {
        store_usage_t u;
        store_usage(i, &u);
        printf("%d %lld %d %d\n", u.games, u.games_bytes, u.saves, u.media);
        return 0;
    }
    if (!strcmp(cmd, "install")) store_install(i); else store_remove(i, !strcmp(cmd, "delete"));
    store_join();
    printf("%d %d %d\n", s->phase, s->error, store_pkg(i)->state);
    return 0;
}
C
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DSTORE_TEST -DUPDATE_DEFAULT_URL='""' -Isrc -o t src/t.c src/store.c src/zipread.c common/json.c -lcurl -lcrypto -lz -lpthread -ldl

# pacchetti: "alfa" con un core e due emulatori, "beta" con un core rotto
mk() {   # mk VERSIONE: prepara pkgsrc e pubblica in www
    rm -rf pkgsrc; mkdir -p pkgsrc/fbneo/Cores pkgsrc/fbneo/Emulators pkgsrc/fbneo/Dats pkgsrc/mgba/Cores pkgsrc/mgba/Emulators
    if [ -n "$BADCORE" ]; then cp bad_libretro.so pkgsrc/fbneo/Cores/fbneo_libretro.so; else cp good_libretro.so pkgsrc/fbneo/Cores/fbneo_libretro.so; fi
    echo '{"name":"FinalBurn Neo"}' > pkgsrc/fbneo/Emulators/fbneo.json
    echo "indice $1" > pkgsrc/fbneo/Dats/fbneo.tsv
    [ "$1" = 1 ] && echo vecchio > pkgsrc/fbneo/Dats/solo-v1.roms
    [ "$1" = 2 ] && { mkdir -p pkgsrc/fbneo/Cores/extra; echo nuovo > pkgsrc/fbneo/Cores/extra/dati.bin; }
    cp bad_libretro.so pkgsrc/mgba/Cores/mgba_libretro.so
    echo '{"name":"Game Boy Advance"}' > pkgsrc/mgba/Emulators/gba.json
    mkdir -p pkgsrc/files/Apps/files
    cp good_libretro.so pkgsrc/files/Apps/files/files
    printf '#!/bin/sh\nexec ./files\n' > pkgsrc/files/Apps/files/launch.sh
    [ -n "$APPX" ] && echo ricompilata > pkgsrc/files/Apps/files/extra.txt
    echo '{"title":"Files","version":"'"${APPV:-1.0}"'","texts":{"it":{"title":"Esplora file","description":"Sfoglia la scheda"},"en":{"title":"Files","description":"Browse the card"}}}' > pkgsrc/files/Apps/files/app.json
    # percorsi lunghi (audit BUG-002): il registro deve elencarli interi
    L1=Apps/files/$(printf 'cartella-con-un-nome-lungo/%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18)a.bin
    L2=Apps/files/$(printf 'altra-cartella-dal-nome-lungo/%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16)b.bin
    mkdir -p "pkgsrc/files/$(dirname $L1)" "pkgsrc/files/$(dirname $L2)"; echo uno > "pkgsrc/files/$L1"; echo due > "pkgsrc/files/$L2"
    if [ -n "$TEMA" ]; then   # un tema come pacchetto: la cartella Temi dello Store
        mkdir -p pkgsrc/tema-sole/Themes/sole
        printf '{\n  // prova\n  "name": "Sole", "version": "1.1",\n  "variants": { "a": { "accent": "#FFAA00" }, "b": { "accent": "#0066FF" } }\n}\n' > pkgsrc/tema-sole/Themes/sole/theme.json
    fi
    if [ -n "$CORNICI" ]; then   # cornici: la descrizione è quella vera, in frontend/package/bezels/tv/pack.json
        mkdir -p pkgsrc/cornici-tv/Bezels/nes
        echo tv > pkgsrc/cornici-tv/Bezels/nes/_mosaik.png
    fi
    printf 'fbneo d15071a\nmgba 7a12d6d\n' > labels.txt
    rm -rf www; python3 /trimui/toolchain/make-store.py --src pkgsrc --out www --key ${2:-key.pem} --labels labels.txt > /dev/null
}
mk 1
cd www; python3 -m http.server 8099 --bind 127.0.0.1 >/dev/null 2>&1 & SRV=$!; cd ..
sleep 1
echo "http://127.0.0.1:8099" > sd/System/update-url.txt
R=$W/sd
check "catalogo: tre pacchetti assenti" "2 0 3 files:0 fbneo:0 mgba:0" "$(./t $R list 2>/dev/null)"
check "app: nome e descrizione nella lingua" "Esplora file|Sfoglia la scheda|apps" "$(LINGUA=it ./t $R title files 2>/dev/null)"
check "app: lingua mancante, inglese" "Files|Browse the card|apps" "$(LINGUA=de ./t $R title files 2>/dev/null)"
check "app installata" "4 0 1" "$(./t $R install files 2>/dev/null)"
check "programma dell'app eseguibile" "si" "$([ -x sd/Apps/files/files ] && echo si || echo no)"
check "registro con i percorsi lunghi interi" "2" "$(grep -c -e "^file $L1\$" -e "^file $L2\$" sd/System/pacchetti/files.txt)"
check "registro: una riga per file" "$(find pkgsrc/files -type f | wc -l | tr -d ' ')" "$(grep -c '^file ' sd/System/pacchetti/files.txt)"
check "app eliminata" "4 0 0" "$(./t $R delete files 2>/dev/null)"
check "cartella dell'app tolta" "no" "$([ -d sd/Apps/files ] && echo si || echo no)"
check "installazione" "4 0 1" "$(./t $R install fbneo 2>/dev/null)"
check "file del pacchetto sulla scheda" "indice 1" "$(cat sd/Dats/fbneo.tsv)"
check "registro con la versione" "si" "$(grep -q '^version ' sd/System/pacchetti/fbneo.txt && echo si || echo no)"
check "nessun .part rimasto" "0" "$(find sd -name '*.part' | wc -l | tr -d ' ')"
check "zip scaricato tolto" "0" "$(ls sd/System/.store | wc -l | tr -d ' ')"
check "core che non si carica: errore" "5 7 0" "$(./t $R install mgba 2>/dev/null)"
check "core rotto tolto" "no" "$([ -e sd/Cores/mgba_libretro.so ] && echo si || echo no)"
check "e il suo emulatore" "no" "$([ -e sd/Emulators/gba.json ] && echo si || echo no)"
check "catalogo tenuto sulla scheda, senza rete" "2 0 3 files:0 fbneo:1 mgba:0" "$(./t $R cache 2>/dev/null)"
# versione nuova: un file in meno, una cartella in più
mk 2
check "aggiornamento disponibile" "2 0 3 files:0 fbneo:2 mgba:0" "$(./t $R list 2>/dev/null)"
check "aggiornamento" "4 0 1" "$(./t $R install fbneo 2>/dev/null)"
check "file nuovo" "nuovo" "$(cat sd/Cores/extra/dati.bin)"
check "file che non c'è più tolto" "no" "$([ -e sd/Dats/solo-v1.roms ] && echo si || echo no)"
check "indice aggiornato" "indice 2" "$(cat sd/Dats/fbneo.tsv)"
# transazione (audit BUG-004): v2 installata; la v3 toglie Cores/extra/dati.bin
snap() { (cd sd && find . -type f ! -path './System/.store/*' ! -name 'store.json' ! -name 'store.sig' -exec md5sum {} + | sort | md5sum); }
V2=$(snap)
cp -R sd sd-v2
mk 3
pass=0; tot=0
for n in 1 2 3 4 5 6 7 8; do   # ogni spostamento che fallisce: si torna alla v2, subito
    tot=$((tot+1))
    r=$(STORE_FAIL_AT=$n ./t $R install fbneo 2>/dev/null)
    [ "$r" = "5 6 2" ] && [ "$(snap)" = "$V2" ] && [ ! -e sd/System/store-giornale.txt ] && pass=$((pass+1)) || echo "    spostamento $n fallito: [$r] $(cd sd && find . -name '*.annulla' -o -name '*.part' -o -name 'store-giornale.txt' | head -3)"
done
check "rename fallito a ogni passo: v2 intatta, nessun avanzo" "$tot" "$pass"
pass=0; tot=0
for n in 1 2 3 4 5 6 7 8; do   # corrente che manca a ogni passo: al riavvio si torna alla v2
    tot=$((tot+1))
    STORE_STOP_AT=$n ./t $R install fbneo >/dev/null 2>&1 || true
    ./t $R recover 2>/dev/null
    [ "$(snap)" = "$V2" ] && [ ! -e sd/System/store-giornale.txt ] && pass=$((pass+1)) || echo "    interruzione $n: $(cd sd && find . -name '*.annulla' -o -name '*.part' -o -name 'store-giornale.txt' | head -3)"
done
check "interruzione a ogni passo e riavvio: v2 intatta" "$tot" "$pass"
check "giornale senza intestazione: messo da parte, non cancellato" "si" "$(: > sd/System/store-giornale.txt; ./t $R recover 2>/dev/null; [ -e sd/System/store-giornale-guasto.txt ] && rm sd/System/store-giornale-guasto.txt && [ "$(snap)" = "$V2" ] && [ ! -e sd/System/store-giornale.txt ] && echo si || echo no)"
BADCORE=1 mk 5
check "aggiornamento con un core che non parte: errore" "5 7 2" "$(./t $R install fbneo 2>/dev/null)"
check "… e resta la versione di prima, che funziona" "si" "$([ "$(snap)" = "$V2" ] && cmp -s sd/Cores/fbneo_libretro.so good_libretro.so && echo si || echo no)"
mk 3
check "la v3 poi si installa" "4 0 1" "$(./t $R install fbneo 2>/dev/null)"
check "v3: file tolto, indice nuovo, niente avanzi" "no indice 3 0" "$([ -e sd/Cores/extra/dati.bin ] && echo si || echo no) $(cat sd/Dats/fbneo.tsv) $(find sd -name '*.annulla' -o -name 'store-giornale.txt' | wc -l | tr -d ' ')"
rm -rf sd && mv sd-v2 sd
mk 2
# pacchetti di un backup ripristinato: si rimettono tutti, restano quelli non riusciti
printf 'fbneo\nfiles\nmgba\nnonesiste\nfiles\n' > sd/System/ripristino-pacchetti.txt
check "backup: da rimettere (non i già installati, niente doppi)" "3" "$(./t $R pending 2>/dev/null)"
check "rimessi: uno non riesce (core rotto)" "5 7" "$(./t $R restore 2>/dev/null)"
check "app del backup installata" "si" "$([ -x sd/Apps/files/files ] && [ -e sd/System/pacchetti/files.txt ] && echo si || echo no)"
check "resta da rimettere solo quello non riuscito" "mgba" "$(cat sd/System/ripristino-pacchetti.txt)"
# app ricompilata con la stessa versione: nessun aggiornamento; versione nuova: aggiornamento
APPX=1 mk 2
check "app con file diversi ma stessa versione: installata" "files:1" "$(./t $R list 2>/dev/null | grep -o 'files:[0-9]')"
APPX=1 APPV=1.1 mk 2
check "app con la versione nuova: aggiornamento" "files:2" "$(./t $R list 2>/dev/null | grep -o 'files:[0-9]')"
mk 2
rm sd/System/ripristino-pacchetti.txt
check "niente da rimettere" "0" "$(./t $R pending 2>/dev/null)"
check "app tolta di nuovo" "4 0 0" "$(./t $R delete files 2>/dev/null)"
# pacchetto manomesso: l'impronta del catalogo non torna
Z=$(ls www/store-fbneo-*.zip); cp $Z orig.zip; printf 'x' >> $Z
rm sd/System/pacchetti/fbneo.txt
check "senza registro ma con l'emulatore: arrivato col sistema" "2 0 3 files:0 fbneo:3 mgba:0" "$(./t $R list 2>/dev/null)"
check "pacchetto manomesso rifiutato" "5 5 3" "$(./t $R install fbneo 2>/dev/null)"
cp orig.zip $Z
# archiviare uno arrivato col sistema: i file del catalogo, giochi e salvataggi restano
mkdir -p sd/Roms/FBNEO sd/Saves/fbneo; echo rom > sd/Roms/FBNEO/gioco.zip; echo sav > sd/Saves/fbneo/gioco.state
mkdir -p sd/Roms/FBNEO/sotto; echo rom2 > sd/Roms/FBNEO/sotto/altro.zip; touch sd/Roms/FBNEO/.nascosto
check "anteprima: giochi, byte, salvataggi, immagini (arcade: nessuna)" "2 9 1 0" "$(./t $R usage fbneo 2>/dev/null)"
check "archiviare" "4 0 4" "$(./t $R archive fbneo 2>/dev/null)"
check "archiviato: i giochi restano" "rom" "$(cat sd/Roms/FBNEO/gioco.zip)"
check "archiviato: i salvataggi restano" "sav" "$(cat sd/Saves/fbneo/gioco.state)"
check "core tolto" "no" "$([ -e sd/Cores/fbneo_libretro.so ] && echo si || echo no)"
check "riscaricare un archiviato" "4 0 1" "$(./t $R install fbneo 2>/dev/null)"
# eliminare: via anche giochi e salvataggi, la cartella dei giochi resta vuota; i BIOS no
mkdir -p sd/Bios; echo bios > sd/Bios/neogeo.zip
check "anteprima di un archiviato" "2 9 1 0" "$(./t $R usage fbneo 2>/dev/null)"
check "eliminare" "4 0 0" "$(./t $R delete fbneo 2>/dev/null)"
check "eliminato: giochi tolti, cartella vuota" "0" "$(ls sd/Roms/FBNEO | wc -l | tr -d ' ')"
check "eliminato: salvataggi tolti" "no" "$([ -e sd/Saves/fbneo ] && echo si || echo no)"
check "eliminato: BIOS restano" "bios" "$(cat sd/Bios/neogeo.zip)"
check "eliminato: niente registro" "no" "$([ -e sd/System/pacchetti/fbneo.txt ] && echo si || echo no)"
check "cartella vuota tolta" "no" "$([ -d sd/Cores/extra ] && echo si || echo no)"
check "Cores resta" "si" "$([ -d sd/Cores ] && echo si || echo no)"
# catalogo firmato con un'altra chiave: si resta con quello sulla scheda
mk 2 altra.pem
check "firma sbagliata: errore, catalogo di prima" "2 2 3 files:0 fbneo:0 mgba:0" "$(./t $R list 2>/dev/null)"
# percorso fuori dalle cartelle permesse: il catalogo intero non vale
mk 2
python3 - <<'P'
import json, subprocess
p = "/tmp/store/www/store.json"; c = json.load(open(p)); c["packages"][0]["files"].append("../System/wifi.conf")
t = json.dumps(c).encode(); open(p, "wb").write(t)
open("/tmp/store/www/store.sig", "wb").write(subprocess.run(["openssl", "dgst", "-sha256", "-sign", "/tmp/store/key.pem", p], capture_output=True).stdout)
P
check "percorso pericoloso: catalogo rifiutato" "3" "$(./t $R list 2>/dev/null | cut -d' ' -f2)"
# pacchetto copiato sulla scheda, senza rete
mk 2
cp www/store-fbneo-*.zip sd/
check "pacchetto copiato: installato" "FinalBurn Neo installato" "$(./t $R offline 2>off.log)"; cat off.log
check "e c'è il core" "si" "$([ -e sd/Cores/fbneo_libretro.so ] && echo si || echo no)"
check "non lo propone due volte" "niente" "$(./t $R offline 2>/dev/null)"
# pacchetto che nessun catalogo firmato conosce: rifiutato (verifica audit), poi accettato con
# store.json e store.sig copiati accanto
rm -f sd/store-*.zip; mk 3; cp www/store-fbneo-*.zip sd/
check "pacchetto sconosciuto: rifiutato" "FinalBurn Neo non verificato" "$(./t $R offline 2>/dev/null)"
check "rifiutato: niente installato" "indice 2" "$(cat sd/Dats/fbneo.tsv)"
check "rifiutato: si ripropone" "FinalBurn Neo non verificato" "$(./t $R offline 2>/dev/null)"
cp www/store.json www/store.sig sd/
check "con il catalogo firmato accanto: installato" "FinalBurn Neo installato" "$(./t $R offline 2>/dev/null)"
check "versione nuova sulla scheda" "indice 3" "$(cat sd/Dats/fbneo.tsv)"
echo manomesso >> sd/store.json
rm -f sd/store-*.zip; mk 4; cp www/store-fbneo-*.zip sd/
check "catalogo accanto manomesso: rifiutato" "FinalBurn Neo non verificato" "$(./t $R offline 2>/dev/null)"
# temi nello Store
rm -f sd/store.json sd/store.sig sd/store-*.zip
TEMA=1 mk 4
check "tema nel catalogo: nome e cartella Temi" "Sole||themes" "$(./t $R title tema-sole 2>/dev/null)"
check "tema: i colori delle varianti nel catalogo" "si" "$(grep -q '"colors": \[' www/store.json && grep -q '#0066FF' www/store.json && echo si || echo no)"
check "tema installato" "4 0 1" "$(./t $R install tema-sole 2>/dev/null)"
check "file del tema sulla scheda" "si" "$([ -f sd/Themes/sole/theme.json ] && echo si || echo no)"
check "tema tolto" "4 0 0" "$(./t $R delete tema-sole 2>/dev/null)"
check "cartella del tema tolta, Themes resta" "no si" "$([ -e sd/Themes/sole ] && echo si || echo no) $([ -d sd/Themes ] && echo si || echo no)"
# cornici nello Store: scrivono solo i file _mosaik, quelli dell'utente restano
CORNICI=1 mk 4
mkdir -p sd/Bezels/nes; echo mia > sd/Bezels/nes/_default.png
check "cornici nel catalogo: nome e cartella Cornici" "Television|Home consoles on an old television set|bezels" "$(./t $R title cornici-tv 2>/dev/null)"
check "cornici installate" "4 0 1" "$(./t $R install cornici-tv 2>/dev/null)"
check "cornice dello Store accanto a quella dell'utente" "tv mia" "$(cat sd/Bezels/nes/_mosaik.png) $(cat sd/Bezels/nes/_default.png)"
check "cornici tolte, quella dell'utente resta" "4 0 0 no mia" "$(./t $R delete cornici-tv 2>/dev/null) $([ -e sd/Bezels/nes/_mosaik.png ] && echo si || echo no) $(cat sd/Bezels/nes/_default.png)"
# giornale che non si legge (audit A38-003): resta dov'è e lo Store non scrive altro
mkdir sd/System/store-giornale.txt
check "giornale illeggibile: installazione rifiutata, il giornale resta" "5 6 si" "$(./t $R install cornici-tv 2>/dev/null | cut -d' ' -f1-2) $([ -d sd/System/store-giornale.txt ] && echo si || echo no)"
check "… e niente è stato scritto" "no" "$([ -e sd/Bezels/nes/_mosaik.png ] && echo si || echo no)"
rmdir sd/System/store-giornale.txt
check "tolto l'ostacolo si installa di nuovo" "4 0 1" "$(./t $R install cornici-tv 2>/dev/null)"
kill $SRV
echo
if [ $ko -eq 0 ]; then echo "TUTTO OK ($ok prove)"; else echo "$ko PROVE FALLITE"; exit 1; fi
