#!/bin/sh
# Launcher compilato in Alpine con il video finto di SDL (dummy): schermate con --auto e controllo dei
# pixel. Avvisi lunghi dentro lo schermo (LAU-18), chiusura della musica che non torna (BT-09),
# preferenza dei doppi (LAU-15) e home con i doppi (LAU-16), audit 0.9.88.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-ui.sh
apk add -q build-base pkgconf sdl2-dev sdl2_ttf-dev sdl2_image-dev sdl2_mixer-dev curl-dev zlib-dev openssl-dev py3-pillow font-dejavu > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

mkdir -p /w && cp -r /trimui/frontend /trimui/VERSION /w/ && cp /trimui/UPDATE_URL /w/ 2> /dev/null
cd /w/frontend && rm -rf build sdcard
make build/launcher build/gamemenu > /tmp/make.log 2>&1 || { cat /tmp/make.log; echo "launcher non compilato"; exit 1; }
# prova /trimui/frontend/src/launcher/launcher.c, /trimui/frontend/src/common/ui.c,
# /trimui/frontend/src/launcher/music.c, /trimui/frontend/src/common/settings.c e
# /trimui/frontend/src/tools/gamemenu.c

R=/tmp/sd/Arcade
mkdir -p $R/System $R/Logs $R/Lang $R/Themes $R/Roms $R/Emulators
cp lang/*.json $R/Lang/
cp -R themes/. $R/Themes/
# i font condivisi non sono nel repository (li mette setup-mac.sh nella SD simulata): quelli, se ci
# sono, altrimenti DejaVu con lo stesso nome
mkdir -p $R/Themes/_shared/fonts
if [ -f /trimui/frontend/sdcard/Themes/_shared/fonts/Nunito.ttf ]; then
    cp /trimui/frontend/sdcard/Themes/_shared/fonts/*.ttf $R/Themes/_shared/fonts/
else
    for f in Nunito PressStart2P; do cp /usr/share/fonts/dejavu/DejaVuSans.ttf $R/Themes/_shared/fonts/$f.ttf; done
fi
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ARCADE_NO_AUDIO=1

# righe dello schermo in cui due schermate differiscono, nella colonna x (in millesimi della larghezza)
cat > /tmp/diff.py <<'X'
import sys
from PIL import Image
a, b = Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")
w, h = a.size
x = int(w * int(sys.argv[3]) / 1000)
print(sum(1 for y in range(h // 3) if a.getpixel((x, y)) != b.getpixel((x, y))))
X

echo "--- avviso lungo (LAU-18)"
# 160 caratteri, come bluetooth.pair_failed in tedesco con un nome lungo (senza virgole: --auto le usa
# per separare i passi)
LONG="Kopplung mit Kabellose Kopfhörer Pro Max Edition fehlgeschlagen: Gerät einschalten und in den Kopplungsmodus versetzen und dann erneut versuchen bitte jetzt"
./build/launcher --root $R --auto "wait:40,shot:/tmp/prima.png,wait:5,toast:$LONG,wait:5,shot:/tmp/dopo.png,wait:5,quit" > /tmp/l.log 2>&1
check "schermate scritte" '[ -s /tmp/prima.png ] && [ -s /tmp/dopo.png ]'
c=$(python3 /tmp/diff.py /tmp/prima.png /tmp/dopo.png 500)
check "l'avviso c'è (centro cambiato in $c righe)" '[ "$c" -gt 10 ]'
l=$(python3 /tmp/diff.py /tmp/prima.png /tmp/dopo.png 15)
r=$(python3 /tmp/diff.py /tmp/prima.png /tmp/dopo.png 985)
check "l'avviso non arriva ai bordi (sinistra $l, destra $r righe cambiate)" '[ "$l" = 0 ] && [ "$r" = 0 ]'
SHORT="Salvato"
./build/launcher --root $R --auto "wait:40,toast:$SHORT,wait:5,shot:/tmp/breve.png,wait:5,quit" > /tmp/l.log 2>&1
s=$(python3 /tmp/diff.py /tmp/prima.png /tmp/breve.png 500)
check "un avviso lungo va su due righe, più alto di uno breve ($c contro $s righe)" '[ "$c" -gt $((s + 20)) ]'

# audit 0.9.88, BT-09: cambiata l'uscita audio (cuffie collegate o scollegate) con la musica del menu,
# Mix_CloseAudio che non torna fermava il launcher finché il cane da guardia non lo chiudeva (12 s,
# poi musica spenta per sempre all'accensione). Ora la chiusura ha un limite e si prosegue senza audio.
echo "--- musica del menu e chiusura del mixer che non torna (BT-09)"
mkdir -p $R/Music/Menu
python3 -c "
import wave, math, struct
w = wave.open('$R/Music/Menu/prova.wav', 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b''.join(struct.pack('<hh', v, v) for v in (int(8000 * math.sin(i / 20)) for i in range(44100 * 20))))
w.close()"
printf '{ "language": "it", "menu_music": true, "music_volume": 5 }\n' > $R/System/settings.json
rm -f $R/Logs/Arcade.log /tmp/mosaik-musica-* $R/System/musica-bloccata.txt
(unset ARCADE_NO_AUDIO; ./build/launcher --root $R --auto "wait:40,shot:/tmp/m1.png,route,wait:20,shot:/tmp/m2.png,quit" > /tmp/l.log 2>&1)
check "musica partita" 'grep -q "musica: prova" $R/Logs/Arcade.log'
check "uscita cambiata: la musica riparte" 'grep -q "dopo la sospensione riparte prova" $R/Logs/Arcade.log'
rm -f $R/Logs/Arcade.log /tmp/m2.png
t0=$(date +%s)
(unset ARCADE_NO_AUDIO; ARCADE_MIX_CLOSE_BLOCCA=1 timeout 40 ./build/launcher --root $R --auto "wait:40,route,wait:20,shot:/tmp/m2.png,quit" > /tmp/l.log 2>&1); rc=$?
dt=$(($(date +%s) - t0))
check "chiusura bloccata: il launcher non viene chiuso dal cane da guardia (uscita $rc)" '[ $rc != 3 ] && [ ! -f /tmp/mosaik-musica-bloccata ]'
check "chiusura bloccata: il launcher continua ($dt s)" '[ -s /tmp/m2.png ] && [ $dt -lt 12 ]'
check "chiusura bloccata: lo dice nel registro" 'grep -q "si prosegue senza audio" $R/Logs/Arcade.log'

# pixel diversi fra due schermate fuori dalla barra in alto (l'ora cambia)
cat > /tmp/area.py <<'X'
import sys
from PIL import Image
a, b = Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")
w, h = a.size
print(sum(1 for y in range(h // 7, h * 6 // 7, 2) for x in range(0, w, 2) if a.getpixel((x, y)) != b.getpixel((x, y))))
X
dup_root() {   # FBNeo e MAME 2003-Plus con lo stesso gioco, più il NES
    rm -rf $R/Emulators $R/Roms $R/System/recent.txt $R/System/favorites.txt $R/System/chiavi $R/Music
    mkdir -p $R/Emulators $R/Roms/FBNEO $R/Roms/MAME2003P $R/Roms/NES
    cp emulators/fbneo.json emulators/mame2003p.json emulators/nes.json $R/Emulators/
    python3 -c "
import zipfile
for d in ('FBNEO', 'MAME2003P'):
    z = zipfile.ZipFile('$R/Roms/%s/gioco.zip' % d, 'w'); z.writestr('a.bin', b'x' * 64); z.close()"
}
setting() { sed -n "s/.*\"$1\": *\"\{0,1\}\([^\",]*\).*/\1/p" $R/System/settings.json; }

# audit 0.9.88, LAU-15: la preferenza dei doppi passava dalla posizione al nome alla prima ricarica e
# prendeva il primo emulatore installato anche se era una console: installato prima il NES, "Preferisci
# NES" per sempre e i doppi arcade tutti visibili
echo "--- preferenza dei doppi su una scheda nuova (LAU-15)"
rm -rf $R/Emulators $R/Roms $R/Music; mkdir -p $R/Emulators $R/Roms/NES
cp emulators/nes.json $R/Emulators/
printf '{ "language": "it", "setup_done": true, "duplicates": 1, "duplicates_emu": "" }\n' > $R/System/settings.json
./build/launcher --root $R --auto "wait:40,quit" > /tmp/l.log 2>&1
check "solo il NES installato: la preferenza non diventa il NES ($(setting duplicates_emu))" '[ "$(setting duplicates_emu)" != nes ]'
cp emulators/fbneo.json emulators/mame2003p.json $R/Emulators/
./build/launcher --root $R --auto "wait:40,quit" > /tmp/l.log 2>&1
check "installato FBNeo: preferito FBNeo ($(setting duplicates_emu))" '[ "$(setting duplicates_emu)" = fbneo ] || { [ -z "$(setting duplicates_emu)" ] && [ "$(setting duplicates)" != 0 ]; }'
printf '{ "language": "it", "setup_done": true, "duplicates": 3, "duplicates_emu": "nes" }\n' > $R/System/settings.json
./build/launcher --root $R --auto "wait:40,quit" > /tmp/l.log 2>&1
check "scheda col NES già preferito (dalla 0.9.73): torna FBNeo ($(setting duplicates_emu))" '[ "$(setting duplicates_emu)" = fbneo ]'
printf '{ "language": "it", "setup_done": true, "duplicates": 0, "duplicates_emu": "" }\n' > $R/System/settings.json
./build/launcher --root $R --auto "wait:40,quit" > /tmp/l.log 2>&1
check "«mostra tutti» scelto dall'utente resta ($(setting duplicates))" '[ "$(setting duplicates)" = 0 ] && [ -z "$(setting duplicates_emu)" ]'

# audit 0.9.88, LAU-16: home, carosello e Galleria guardavano i doppi nascosti: un gioco avviato
# dall'emulatore non preferito non diventava la carta «Riprendi» e i suoi preferiti mancavano
echo "--- home con un doppio dell'emulatore non preferito (LAU-16)"
dup_root
printf '{ "language": "it", "setup_done": true, "duplicates": 1, "duplicates_emu": "fbneo" }\n' > $R/System/settings.json
./build/launcher --root $R --auto "wait:60,shot:/tmp/h0.png,quit" > /tmp/l.log 2>&1
printf 'mame2003p\tgioco\n' > $R/System/recent.txt
./build/launcher --root $R --auto "wait:60,shot:/tmp/h1.png,quit" > /tmp/l.log 2>&1
printf 'fbneo\tgioco\n' > $R/System/recent.txt
./build/launcher --root $R --auto "wait:60,shot:/tmp/h2.png,quit" > /tmp/l.log 2>&1
n2=$(python3 /tmp/area.py /tmp/h0.png /tmp/h2.png)
n1=$(python3 /tmp/area.py /tmp/h0.png /tmp/h1.png)
check "giocato da FBNeo: la home lo mostra ($n2 pixel cambiati)" '[ "$n2" -gt 500 ]'
check "giocato da MAME 2003-Plus (non preferito): la home lo mostra lo stesso ($n1 pixel cambiati)" '[ "$n1" -gt 500 ]'
rm -f $R/System/recent.txt
printf 'mame2003p\tgioco\n' > $R/System/favorites.txt
./build/launcher --root $R --auto "wait:60,shot:/tmp/h3.png,quit" > /tmp/l.log 2>&1
n3=$(python3 /tmp/area.py /tmp/h0.png /tmp/h3.png)
check "preferito da MAME 2003-Plus: la home lo mostra ($n3 pixel cambiati)" '[ "$n3" -gt 500 ]'

# audit 0.9.88, GIO-16: la pagina Comandi del N64 mostra 8 righe su 10; la barra in basso non diceva
# che su e giù scorrono
echo "--- pagina Comandi del menu dei giochi (GIO-16)"
cp emulators/n64.json $R/Emulators/
printf '{ "name": "Prova", "kind": "console", "controls": [["A", "A"], ["B", "B"]] }\n' > $R/Emulators/corto.json
SDL_VIDEODRIVER=dummy ARCADE_EMU=n64 ./build/gamemenu --root $R --controls --shot /tmp/c-lungo.png > /tmp/g.log 2>&1
SDL_VIDEODRIVER=dummy ARCADE_EMU=corto ./build/gamemenu --root $R --controls --shot /tmp/c-corto.png > /tmp/g.log 2>&1
cat > /tmp/barra.py <<'X'
import sys
from PIL import Image
a, b = Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")
w, h = a.size
print(sum(1 for y in range(h * 92 // 100, h) for x in range(0, w * 80 // 100) if a.getpixel((x, y)) != b.getpixel((x, y))))
X
n=$(python3 /tmp/barra.py /tmp/c-lungo.png /tmp/c-corto.png)
check "10 comandi su 8 righe: la barra dice che si scorre ($n pixel diversi da quella con 2 comandi)" '[ "$n" -gt 50 ]'

# audit 0.9.88, LAU-19: senza emulatori Piattaforme mostrava «Tutti, 0 giochi» senza invitare allo
# Store; A apriva la libreria vuota. Ora invita, e A apre lo Store
echo "--- Piattaforme senza emulatori (LAU-19)"
rm -rf $R/Emulators $R/Roms $R/Music $R/System/recent.txt $R/System/favorites.txt; mkdir -p $R/Emulators $R/Roms
printf '{ "language": "it", "setup_done": true }\n' > $R/System/settings.json
echo http://127.0.0.1:9 > $R/System/update-url.txt   # niente rete: lo Store senza catalogo, uguale ogni volta
# dalla finestra «Nessun emulatore» della home, «Apri lo Store»: la schermata dello Store
./build/launcher --root $R --auto "wait:40,a,wait:30,shot:/tmp/store.png,quit" > /tmp/l.log 2>&1
# B chiude la finestra, A apre Piattaforme, A di nuovo
./build/launcher --root $R --auto "wait:40,b,wait:20,a,wait:30,shot:/tmp/piatt.png,a,wait:30,shot:/tmp/piatt-a.png,quit" > /tmp/l.log 2>&1
n=$(python3 /tmp/area.py /tmp/store.png /tmp/piatt-a.png)
check "da Piattaforme senza emulatori A apre lo Store ($n pixel diversi)" '[ "$n" -lt 200 ]'
rm -f $R/System/update-url.txt

# audit 0.9.88, BT-03 (console, 20 novembre): al risveglio con le cuffie l'audio non si apriva
# ("Cannot allocate memory") e poi nessun cambio d'uscita lo riapriva: muto fino al riavvio
echo "--- audio che non si apre, poi uscita cambiata (BT-03)"
printf '{ "language": "it", "setup_done": true }\n' > $R/System/settings.json
rm -f $R/Logs/Arcade.log
(unset ARCADE_NO_AUDIO; ARCADE_AUDIO_FAIL=1 ./build/launcher --root $R --auto "wait:40,route,wait:20,quit" > /tmp/l.log 2>&1)
check "apertura fallita all'avvio (prova)" 'grep -q "audio non disponibile" $R/Logs/Arcade.log'
check "uscita cambiata: l'audio si riapre" 'grep -q "audio: driver" $R/Logs/Arcade.log'

# audit 0.9.72, LAU-13 (lasciato cadere, LAU-20 del 0.9.88): con l'emulatore installato ma senza il suo
# core il launcher scriveva next.txt, metteva il gioco nei recenti e il runner falliva
echo "--- emulatore senza il suo core (LAU-13)"
dup_root; rm -f $R/System/next.txt
printf '{ "language": "it", "setup_done": true, "duplicates": 0, "duplicates_emu": "" }\n' > $R/System/settings.json
./build/launcher --root $R --auto "wait:40,a,wait:30,a,wait:30,a,wait:30,a,wait:40,shot:/tmp/nocore.png,quit" > /tmp/l.log 2>&1; rc=$?
check "non parte: niente next.txt (uscita $rc)" '[ ! -f $R/System/next.txt ] && [ $rc != 10 ]'
check "non entra fra i recenti" '[ ! -s $R/System/recent.txt ]'
check "lo dice nel registro" 'grep -q "manca il core" $R/Logs/Arcade.log'

# 21 novembre: "cpu" (1-4) valeva solo per gli emulatori; un gioco Linux pesante girava al profilo
# generale, e anche impostandola nel suo launch.sh il menu di Mosaik la rimetteva al profilo.
# Ora anche game.json ha "cpu": il launcher la passa in next.env (ARCADE_CPU_KHZ, letta dal menu)
echo "--- gioco Linux con \"cpu\" in game.json"
linux_game() {   # $1 = contenuto di game.json
    rm -rf $R/Linux $R/System/next.txt $R/System/next.env /tmp/mosaik-avvio-launcher
    mkdir -p $R/Linux/Gioco
    printf '#!/bin/sh\nexit 0\n' > $R/Linux/Gioco/launch.sh; chmod +x $R/Linux/Gioco/launch.sh
    printf '%s\n' "$1" > $R/Linux/Gioco/game.json
    printf 'linux\tGioco\n' > $R/System/avvio-gioco.txt
    printf '{ "language": "it", "setup_done": true }\n' > $R/System/settings.json
    ARCADE_TEST_AUTOBOOT=1 ./build/launcher --root $R --auto "wait:40,quit" > /tmp/l.log 2>&1
}
linux_game '{ "title": "Gioco", "min_system": "0.9.58", "cpu": 4 }'; rc=$?
check "parte (uscita $rc, next.txt)" '[ -s $R/System/next.txt ] && grep -q "Gioco" $R/System/next.txt'
check "next.env con la CPU al massimo ($(cat $R/System/next.env 2>/dev/null | tr '\n' ' '))" 'grep -qx "export ARCADE_CPU_KHZ=2000000" $R/System/next.env'
linux_game '{ "title": "Gioco", "min_system": "0.9.58", "cpu": 2, "env": { "PROVA": "1" } }'
check "con env: tutte e due le righe" 'grep -qx "export ARCADE_CPU_KHZ=1200000" $R/System/next.env && grep -qx "export PROVA='"'"'1'"'"'" $R/System/next.env'
linux_game '{ "title": "Gioco", "min_system": "0.9.58", "cpu": 9 }'
check "valore fuori da 1-4: ignorato" '! grep -q ARCADE_CPU_KHZ $R/System/next.env 2>/dev/null'
linux_game '{ "title": "Gioco", "min_system": "0.9.58" }'
check "senza cpu: niente (profilo generale)" '! grep -q ARCADE_CPU_KHZ $R/System/next.env 2>/dev/null'
rm -rf $R/Linux $R/System/avvio-gioco.txt $R/System/next.txt $R/System/next.env

# audit 0.9.72, LAU-12 (ripreso da LAU-20 del 0.9.88): mancavano ~ e ` nella tastiera, e una password
# Wi-Fi che li contiene non si poteva scrivere. Ogni carattere ASCII stampabile deve esserci
echo "--- tastiera a schermo: tutti i caratteri (LAU-12)"
{ echo '#include <stdio.h>'; awk '/^static const char \*kb_layouts/ {on=1} on {print} on && /^};/ {exit}' src/launcher/launcher.c
  cat <<'C'
int main(void) {
    int miss = 0;
    for (int ch = 0x21; ch < 0x7F; ch++) {
        int found = 0;
        for (int l = 0; l < 3; l++) for (int r = 0; r < 4; r++) for (const char *p = kb_layouts[l][r]; *p; p++) found |= *p == ch;
        if (!found) { printf("%c", ch); miss++; }
    }
    printf("|%d\n", miss);
    return 0;
}
C
} > /tmp/kb.c
gcc -o /tmp/kb /tmp/kb.c
k=$(/tmp/kb)
check "tutti i caratteri stampabili si scrivono (mancano: ${k%|*})" '[ "${k#*|}" = 0 ]'

# LAU-12, seconda parte: la ricerca confrontava i byte, "pokemon" non trovava "Pokémon"
echo "--- ricerca senza accenti (LAU-12)"
{ cat <<'C'
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
typedef struct { char title[160], name[160]; } game_t;
C
  awk '/^\/\* ricerca: parole cercate/ {on=1} on {print} on && /^static bool matches_search/ {m=1} m && /^}/ {exit}' src/launcher/launcher.c
  cat <<'C'
static int t(const char *q, const char *title) {
    game_t g; snprintf(g.title, sizeof g.title, "%s", title); snprintf(g.name, sizeof g.name, "x");
    snprintf(search, sizeof search, "%s", q);
    return matches_search(&g);
}
int main(void) {
    printf("%d%d%d%d%d%d\n", t("pokemon", "Pokémon Oro"), t("POKÉMON", "Pokemon Gold"), t("citta", "La Città"),
           t("ZÜRICH", "zurich 2000"), t("zzz", "Pokémon"), t("mon oro", "Pokémon Oro"));
    return 0;
}
C
} > /tmp/cerca.c
gcc -Wall -o /tmp/cerca /tmp/cerca.c 2>/tmp/cerca.err || cat /tmp/cerca.err
check "pokemon trova Pokémon, POKÉMON trova Pokemon, citta trova Città ($(/tmp/cerca))" '[ "$(/tmp/cerca)" = 111101 ]'

# audit 0.9.72, LAU-14: Informazioni (e la Libreria nella home) contavano anche i doppi nascosti:
# 6548 giochi contro i 4920 di «Tutti». Ora lo stesso numero di «Tutti»
echo "--- numero dei giochi uguale a «Tutti» (LAU-14)"
dup_root
printf '{ "language": "it", "setup_done": true, "duplicates": 1, "duplicates_emu": "fbneo" }\n' > $R/System/settings.json
rm -f $R/Logs/Arcade.log
./build/launcher --root $R --auto "wait:40,quit" > /tmp/l.log 2>&1
check "due copie dello stesso gioco, una nascosta: 1 gioco ($(grep -o 'giochi trovati:.*' $R/Logs/Arcade.log | tail -1))" 'grep -q "giochi trovati: 1 (2 file)" $R/Logs/Arcade.log'

# LAU-14: nelle pagine lunghe delle impostazioni niente diceva che sotto c'erano altre voci. Ora una
# barra di scorrimento a destra delle voci: scorrendo fino in fondo si sposta
echo "--- pagina lunga delle impostazioni: barra di scorrimento (LAU-14)"
printf '{ "language": "it", "setup_done": true }\n' > $R/System/settings.json
./build/launcher --root $R --auto "wait:40,start,wait:30,down,wait:20,a,wait:30,shot:/tmp/set-su.png,down,down,down,down,down,down,down,down,down,down,wait:30,shot:/tmp/set-giu.png,quit" > /tmp/l.log 2>&1
cat > /tmp/banda.py <<'X'
import sys
from PIL import Image
a, b = Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")
w, h = a.size
print(sum(1 for y in range(h * 15 // 100, h * 75 // 100) for x in range(w * 970 // 1000, w * 995 // 1000) if a.getpixel((x, y)) != b.getpixel((x, y))))
X
n=$(python3 /tmp/banda.py /tmp/set-su.png /tmp/set-giu.png)
check "in fondo alla pagina la barra a destra si è spostata ($n pixel)" '[ "$n" -gt 20 ]'


# audit 0.9.88, STO-02: un pacchetto dell'altro modello già installato (Deadcast sulla Smart Pro prima
# della 0.9.67) era invisibile nello Store e non si poteva togliere. Si mostra solo se installato, e
# la sua scheda offre solo Elimina
echo "--- Store: pacchetto dell'altro modello già installato (STO-02)"
{ cat <<'C'
#include <stdio.h>
#include <string.h>
#include "src/launcher/store.h"
static bool sto_no_data(const store_pkg_t *p) { return !strcmp(p->category, "apps"); }
C
  grep -E '^static bool sto_shown' src/launcher/launcher.c
  awk '/^enum \{ SB_INSTALL/ {on=1} on {print} on && /^}/ {exit}' src/launcher/launcher.c
  cat <<'C'
int main(void) {
    store_pkg_t p; memset(&p, 0, sizeof p);
    strcpy(p.category, "console"); p.other_model = true; p.state = PKG_ABSENT;
    int b[4], n;
    printf("%d", sto_shown(&p));                         /* non installato: nascosto */
    p.state = PKG_INSTALLED; n = sto_buttons(&p, b);
    printf(" %d %d%d", sto_shown(&p), n, n ? b[0] == SB_DELETE : 9);   /* installato: si vede, solo Elimina */
    p.state = PKG_UPDATE; n = sto_buttons(&p, b);
    printf(" %d%d\n", n, n ? b[0] == SB_DELETE : 9);      /* anche "da aggiornare": niente Aggiorna */
    return 0;
}
C
} > /tmp/sto.c
gcc -Wall -I. -o /tmp/sto /tmp/sto.c 2>/tmp/sto.err || cat /tmp/sto.err
check "nascosto se non installato; installato: visibile, solo Elimina ($(/tmp/sto))" '[ "$(/tmp/sto)" = "0 1 11 11" ]'

# audit 0.9.88, AGG-13: a scaricamento finito, Annulla o B cancellavano i circa 60 MB scaricati (anche
# dopo l'avviso della batteria bassa). Ora vuol dire «Più tardi»: lo scaricamento resta, all'avvio dopo
# la console propone di installarlo (e lì c'è Elimina)
echo "--- aggiornamento scaricato e B (AGG-13)"
printf '{ "language": "it", "setup_done": true }\n' > $R/System/settings.json
rm -rf $R/Emulators; mkdir -p $R/Emulators; rm -f $R/Logs/Arcade.log
./build/launcher --root $R --auto "wait:40,b,wait:20,start,wait:30,down,wait:15,down,wait:15,down,wait:15,right,wait:15,a,wait:30,a,wait:60,a,wait:400,b,wait:30,quit" > /tmp/l.log 2>&1
check "B a scaricamento finito: lo scaricamento resta" '! grep -q "scaricamento .* tolto" $R/Logs/Arcade.log && grep -q "tenuto per dopo" $R/Logs/Arcade.log'

# audit 0.9.72, LAU-11: nel tema Tessera chiaro il bianco sull'azzurro (righe scelte, pulsante
# principale) arrivava a 2,5:1; serve almeno 4,5:1 (WCAG AA per il testo normale)
echo "--- contrasto del tema Tessera chiaro (LAU-11)"
cat > /tmp/contrasto.py <<'X'
import json, re, sys
t = json.loads(re.sub(r"^\s*//.*$", "", open(sys.argv[1]).read(), flags=re.M))
def lum(h):
    f = lambda c: c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = [f(int(h[i:i + 2], 16) / 255) for i in (1, 3, 5)]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b
v = t["variants"]["light"]
a, b = sorted([lum(v["accent"]), lum(v["accent_text"])], reverse=True)
print("%.2f" % ((a + 0.05) / (b + 0.05)))
X
c=$(python3 /tmp/contrasto.py themes/tessera/theme.json)
check "testo sull'azzurro leggibile ($c:1, almeno 4,5)" 'python3 -c "import sys; sys.exit(0 if float(\"$c\") >= 4.5 else 1)"'

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
