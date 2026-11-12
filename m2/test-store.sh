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
    if (!strcmp(cmd, "offline")) {
        char p[1200], n[64], l[32];
        if (!store_offline_find(root, p, sizeof p, n, sizeof n, l, sizeof l)) { puts("niente"); return 0; }
        printf("%s %s\n", n, store_offline_install(root, p) ? "installato" : "errore");
        return 0;
    }
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
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DUPDATE_DEFAULT_URL='""' -Isrc -o t src/t.c src/store.c src/zipread.c common/json.c -lcurl -lcrypto -lz -lpthread -ldl

# pacchetti: "alfa" con un core e due emulatori, "beta" con un core rotto
mk() {   # mk VERSIONE: prepara pkgsrc e pubblica in www
    rm -rf pkgsrc; mkdir -p pkgsrc/fbneo/Cores pkgsrc/fbneo/Emulators pkgsrc/fbneo/Dats pkgsrc/mgba/Cores pkgsrc/mgba/Emulators
    cp good_libretro.so pkgsrc/fbneo/Cores/fbneo_libretro.so
    echo '{"name":"FinalBurn Neo"}' > pkgsrc/fbneo/Emulators/fbneo.json
    echo "indice $1" > pkgsrc/fbneo/Dats/fbneo.tsv
    [ "$1" = 1 ] && echo vecchio > pkgsrc/fbneo/Dats/solo-v1.roms
    [ "$1" = 2 ] && { mkdir -p pkgsrc/fbneo/Cores/extra; echo nuovo > pkgsrc/fbneo/Cores/extra/dati.bin; }
    cp bad_libretro.so pkgsrc/mgba/Cores/mgba_libretro.so
    echo '{"name":"Game Boy Advance"}' > pkgsrc/mgba/Emulators/gba.json
    printf 'fbneo d15071a\nmgba 7a12d6d\n' > labels.txt
    rm -rf www; python3 /trimui/toolchain/make-store.py --src pkgsrc --out www --key ${2:-key.pem} --labels labels.txt > /dev/null
}
mk 1
cd www; python3 -m http.server 8099 --bind 127.0.0.1 >/dev/null 2>&1 & SRV=$!; cd ..
sleep 1
echo "http://127.0.0.1:8099" > sd/System/update-url.txt
R=$W/sd
check "catalogo: due pacchetti assenti" "2 0 2 fbneo:0 mgba:0" "$(./t $R list 2>/dev/null)"
check "installazione" "4 0 1" "$(./t $R install fbneo 2>/dev/null)"
check "file del pacchetto sulla scheda" "indice 1" "$(cat sd/Dats/fbneo.tsv)"
check "registro con la versione" "si" "$(grep -q '^version ' sd/System/pacchetti/fbneo.txt && echo si || echo no)"
check "nessun .part rimasto" "0" "$(find sd -name '*.part' | wc -l | tr -d ' ')"
check "zip scaricato tolto" "0" "$(ls sd/System/.store | wc -l | tr -d ' ')"
check "core che non si carica: errore" "5 7 0" "$(./t $R install mgba 2>/dev/null)"
check "core rotto tolto" "no" "$([ -e sd/Cores/mgba_libretro.so ] && echo si || echo no)"
check "e il suo emulatore" "no" "$([ -e sd/Emulators/gba.json ] && echo si || echo no)"
check "catalogo tenuto sulla scheda, senza rete" "2 0 2 fbneo:1 mgba:0" "$(./t $R cache 2>/dev/null)"
# versione nuova: un file in meno, una cartella in più
mk 2
check "aggiornamento disponibile" "2 0 2 fbneo:2 mgba:0" "$(./t $R list 2>/dev/null)"
check "aggiornamento" "4 0 1" "$(./t $R install fbneo 2>/dev/null)"
check "file nuovo" "nuovo" "$(cat sd/Cores/extra/dati.bin)"
check "file che non c'è più tolto" "no" "$([ -e sd/Dats/solo-v1.roms ] && echo si || echo no)"
check "indice aggiornato" "indice 2" "$(cat sd/Dats/fbneo.tsv)"
# pacchetto manomesso: l'impronta del catalogo non torna
Z=$(ls www/store-fbneo-*.zip); cp $Z orig.zip; printf 'x' >> $Z
rm sd/System/pacchetti/fbneo.txt
check "senza registro ma con l'emulatore: arrivato col sistema" "2 0 2 fbneo:3 mgba:0" "$(./t $R list 2>/dev/null)"
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
check "firma sbagliata: errore, catalogo di prima" "2 2 2 fbneo:0 mgba:0" "$(./t $R list 2>/dev/null)"
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
kill $SRV
echo
if [ $ko -eq 0 ]; then echo "TUTTO OK ($ok prove)"; else echo "$ko PROVE FALLITE"; exit 1; fi
