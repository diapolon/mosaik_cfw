#!/bin/sh
# Prova delle scritture che non devono perdere dati e del blocco del Wi-Fi. Il codice è quello vero,
# estratto dai sorgenti; gli errori della scheda sono simulati:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-files.sh
set -e
apk add -q build-base linux-headers python3 >/dev/null
W=/tmp/fl; rm -rf $W; mkdir -p $W/d $W/sd/Logs; cd $W
R=/trimui/frontend/src/runner/runner.c; G=/trimui/frontend/src/launcher/games.c
fn() { awk -v re="$2" '$0 ~ re {on=1} on {print} on && /^}/ {exit}' "$1"; }

{
cat <<'C'
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static const char *fail_rename_to; static int fail_fsync; static char dir[512]; static int errors;
static int test_rename(const char *a, const char *b) { if (fail_rename_to && !strcmp(b, fail_rename_to)) { errno = EIO; return -1; } return rename(a, b); }
static int test_fsync(int fd) { if (fail_fsync) { errno = EIO; return -1; } return fsync(fd); }
#define rename test_rename
static int syncs;
static void test_sync(void) { syncs++; }
#define sync test_sync
#define fsync test_fsync
#include "/trimui/frontend/src/common/durable.h"
#define LIST_KEY 208
#define RETRO_MEMORY_SAVE_RAM 0
#define RETRO_MEMORY_RTC 1
enum { BTN_COUNT = 20, ROT_NONE = 0 };
static const int default_map[16] = { 0 };
static int emu_map[16];   /* tasti di partenza dell'emulatore (runner.c): nulli come default_map */
static struct { struct { int aspect, bezel; char filter[32]; } set; } UI;
static const char *filter_legacy_id(int n) { (void)n; return "nitido"; }
static void ui_log(const char *f, ...) { (void)f; }
static void game_file(char *out, size_t n, const char *ext) { snprintf(out, n, "%s/g.%s", dir, ext); }
typedef struct SDL_Surface SDL_Surface;
static SDL_Surface *thumb;
static const char *payload = "";
static unsigned rotation;
static size_t ser_size(void) { return strlen(payload); }
static bool ser(void *b, size_t n) { memcpy(b, payload, n); return true; }
static struct { size_t (*serialize_size)(void); bool (*serialize)(void *, size_t); } core = { ser_size, ser };
static char root[512], core_path[8] = "c", rom_path[8] = "r", title[8] = "t";
static char system_id[32];   /* vuoto: un gioco arcade */
static char game_key[200];   /* vuoto: il nome della ROM */
static void IMG_SavePNG(SDL_Surface *s, const char *p) { (void)s; FILE *f = fopen(p, "w"); fputs(payload, f); fclose(f); }
C
awk '/^static struct \{$/ {s=""; on=1} on {s=s $0 "\n"} /^} [a-z_]*;/ {if ($2=="gcfg;") {printf "%s", s; exit} on=0}' $R
grep -E '^enum \{ NSLOTS|^static const char \*const save_ext' $R
for f in gamecfg_load state_path; do fn $R "^static void $f"; done
fn $R '^static bool copy_whole'
fn $R '^static void auto_keep_previous'
fn $R '^static bool state_save'
sed -n '/^\/\* -* salvataggi a batteria \*\/$/,/^static void sram_load/p' $R | sed '$d'
fn $R '^static void sram_load'
fn $R '^static void sram_save'
fn $R '^static void write_resume'
fn $G '^static void commit_file'
fn $G '^static void write_list'
cat <<'C'
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static const char *get(const char *p) { static char b[256]; FILE *f = fopen(p, "r"); if (!f) return "(manca)"; size_t n = fread(b, 1, 255, f); b[n] = 0; fclose(f); return b; }
static void same(const char *what, const char *got, const char *want) {
    if (!strcmp(got, want)) printf("  ok: %s\n", what);
    else { printf("  ERRORE: %s: atteso '%s', ottenuto '%s'\n", what, want, got); errors++; }
}
static void file_is(const char *what, const char *ext, const char *want) { char p[600]; game_file(p, sizeof p, ext); same(what, get(p), want); }
static void aspect_case(const char *line, int want) {
    char p[600], m[64], g[16], w[16];
    game_file(p, sizeof p, "cfg"); put(p, line);
    gamecfg_load();
    snprintf(m, sizeof m, "impostazioni: %.*s", (int)strcspn(line, "\n"), line);
    snprintf(g, sizeof g, "%d", gcfg.aspect); snprintf(w, sizeof w, "%d", want);
    same(m, g, w);
}
static void save(const char *s, bool want) { payload = s; if (state_save(true) != want) { printf("  ERRORE: salvataggio %s: esito inatteso\n", s); errors++; } }
int main(void) {
    char p[600], q[600];
    snprintf(dir, sizeof dir, "/tmp/fl/d"); snprintf(root, sizeof root, "/tmp/fl");
    UI.set.aspect = 1;
    puts("--- valori impossibili nel file di un gioco");
    aspect_case("aspect=-1\n", 1);
    aspect_case("aspect=7\n", 1);
    aspect_case("aspect=-2147483648\n", 1);
    aspect_case("aspect=2\n", 2);

    puts("--- salvataggio automatico");
    thumb = (SDL_Surface *)1;
    save("A", true);
    file_is("primo salvataggio", "auto.state", "A");
    int s0 = syncs;
    save("B", true);
    file_is("secondo salvataggio", "auto.state", "B");
    if (syncs > s0) puts("  ok: salvataggio scritto sulla scheda dopo la sostituzione"); else { puts("  ERRORE: nessun sync dopo la sostituzione del salvataggio"); errors++; }
    file_is("il primo resta come copia", "auto.state.prev", "A");
    file_is("con la sua anteprima", "auto.state.prev.png", "A");
    game_file(p, sizeof p, "auto.state");
    fail_rename_to = p; save("C", false);
    file_is("ultimo passaggio fallito: resta quello di prima", "auto.state", "B");
    file_is("nessun file provvisorio", "auto.state.tmp", "(manca)");
    game_file(q, sizeof q, "auto.state.prev");
    fail_rename_to = q; save("D", true);
    file_is("copia non riuscita: il nuovo si salva lo stesso", "auto.state", "D");
    file_is("nessun provvisorio della copia", "auto.state.prev.tmp", "(manca)");
    fail_rename_to = NULL; thumb = NULL; save("E", true);
    file_is("senza anteprima nuova non resta quella vecchia", "auto.state.png", "(manca)");
    file_is("la copia conserva la sua", "auto.state.prev.png", "D");

    puts("--- ripresa della partita");
    snprintf(p, sizeof p, "%s/System", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/System/resume.txt", root);
    write_resume();
    same("scritta, con console e identità (\"-\" se mancano)", get(p), "libretro\tc\tr\tt\tresume\t-\t-\n");
    put(p, "vecchia"); fail_fsync = 1; write_resume(); fail_fsync = 0;
    same("scrittura fallita: resta quella di prima", get(p), "vecchia");
    snprintf(q, sizeof q, "%s.tmp", p); same("nessun provvisorio", get(q), "(manca)");

    puts("--- preferiti, recenti, collezioni, nascosti");
    char keys[2][LIST_KEY] = { "uno", "due" };
    snprintf(p, sizeof p, "%s/lista.txt", root); snprintf(q, sizeof q, "%s.tmp", p);
    put(p, "vecchia\n");
    int s1 = syncs;
    write_list(p, keys, 2);
    same("scritta", get(p), "uno\ndue\n");
    if (syncs > s1) puts("  ok: elenco scritto sulla scheda dopo la sostituzione"); else { puts("  ERRORE: nessun sync dopo la sostituzione dell'elenco"); errors++; }
    s1 = syncs;
    put(p, "vecchia\n"); fail_fsync = 1; write_list(p, keys, 2); fail_fsync = 0;
    if (syncs == s1) puts("  ok: scrittura fallita, nessun sync"); else { puts("  ERRORE: sync dopo una scrittura fallita"); errors++; }
    same("scrittura fallita: resta quella di prima", get(p), "vecchia\n");
    same("nessun provvisorio", get(q), "(manca)");
    fail_rename_to = p; write_list(p, keys, 2); fail_rename_to = NULL;
    same("sostituzione fallita: resta quella di prima", get(p), "vecchia\n");
    same("nessun provvisorio", get(q), "(manca)");

    puts("--- salvataggi a batteria (.srm)");
    static unsigned char ram[16], rtc[8];
    static int with_rtc;
    size_t fsz(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof ram : id == RETRO_MEMORY_RTC && with_rtc ? sizeof rtc : 0; }
    void *fdat(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? ram : id == RETRO_MEMORY_RTC && with_rtc ? rtc : NULL; }
    #define FORGET() for (int b_ = 0; b_ < NBATT; b_++) { free(batt[b_].copy); batt[b_].copy = NULL; batt[b_].len = 0; }
    mem_size = fsz; mem_data = fdat;
    game_file(p, sizeof p, "srm");
    sram_load();
    sram_save();
    same("memoria mai cambiata: nessun file", get(p), "(manca)");
    int s2 = syncs;
    ram[0] = 'Z'; ram[1] = 'e';
    sram_save();
    { char b[8]; FILE *f = fopen(p, "rb"); size_t k = f ? fread(b, 1, 2, f) : 0; if (f) fclose(f); b[k] = 0; same("memoria cambiata: file scritto", b, "Ze"); }
    if (syncs > s2) puts("  ok: scritto sulla scheda dopo la sostituzione"); else { puts("  ERRORE: nessun sync"); errors++; }
    remove(p); sram_save();
    same("nessun cambiamento: non si riscrive", get(p), "(manca)");
    ram[2] = 'l'; sram_save();
    memset(ram, 0, sizeof ram); FORGET();
    sram_load();
    same("partita dopo: memoria ricaricata dal file", (char[]){ (char)ram[0], (char)ram[1], (char)ram[2], 0 }, "Zel");
    put(p, "AB"); memset(ram, 'x', sizeof ram); FORGET();
    sram_load();
    same("file più corto: si legge quello che c'è", (char[]){ (char)ram[0], (char)ram[1], (char)ram[2], 0 }, "ABx");
    ram[5] = 'q'; fail_rename_to = p; sram_save(); fail_rename_to = NULL;
    same("sostituzione fallita: resta il file di prima", get(p), "AB");
    snprintf(q, sizeof q, "%s.tmp", p); same("nessun provvisorio", get(q), "(manca)");
    snprintf(q, sizeof q, "%s.orig", p); same("file più corto: l'originale resta in .srm.orig (R06)", get(q), "AB");
    put(q, "AB"); sram_save();
    { char b[8] = ""; FILE *f = fopen(p, "rb"); if (f) { if (fread(b, 1, 6, f) != 6) b[5] = 0; fclose(f); }
      same("al tentativo dopo si scrive", b[5] == 'q' ? "q" : "vecchio", "q"); }
    same(".srm.orig non riscritto", get(q), "AB");
    ram[6] = 'r'; sram_save();
    same("e non riscritto ai salvataggi successivi", get(q), "AB");
    remove(q);
    put(p, "0123456789abcdefXYZ"); memset(ram, 0, sizeof ram); FORGET();
    sram_load();
    same("file più lungo: si carica la parte che serve", (char[]){ (char)ram[0], (char)ram[15], 0 }, "0f");
    ram[0] = 'L'; sram_save();
    same("… e l'originale intero resta in .srm.orig", get(q), "0123456789abcdefXYZ");
    remove(q); remove(p); mkdir(p, 0755);
    memset(ram, 'k', sizeof ram); FORGET();
    sram_load();
    same("file illeggibile: memoria del gioco non toccata", (char[]){ (char)ram[0], (char)ram[15], 0 }, "kk");
    ram[0] = 'N'; sram_save();
    { struct stat st_; same("… e non si scrive sopra senza averlo messo da parte", stat(p, &st_) == 0 && S_ISDIR(st_.st_mode) ? "intatto" : "cambiato", "intatto"); }
    rmdir(p); FORGET(); memset(ram, 0, sizeof ram); sram_load(); ram[0] = 'Z'; ram[1] = 'e'; ram[2] = 'l'; ram[5] = 'q'; sram_save();
    mem_size = NULL; mem_data = NULL; FORGET();
    sram_load(); sram_save();
    puts("  ok: core senza memoria a batteria (FBNeo, MAME): niente da fare");
    puts("--- orologio della cartuccia (.rtc, Gambatte)");
    mem_size = fsz; mem_data = fdat; with_rtc = 1; FORGET();
    game_file(q, sizeof q, "rtc");
    remove(p); memset(ram, 0, sizeof ram); memset(rtc, 0, sizeof rtc);
    sram_load();
    rtc[0] = 'T'; rtc[1] = '1'; sram_save();
    same("orologio cambiato: .rtc scritto", get(q), "T1");
    same("memoria della partita invariata: niente .srm", get(p), "(manca)");
    memset(rtc, 0, sizeof rtc); FORGET(); sram_load();
    same("partita dopo: orologio ricaricato", (char[]){ (char)rtc[0], (char)rtc[1], 0 }, "T1");
    ram[0] = 'P'; sram_save();
    { char b[4] = ""; FILE *f = fopen(p, "rb"); if (f) { if (fread(b, 1, 1, f) != 1) b[0] = 0; fclose(f); } same("e la memoria a parte, nel suo .srm", b, "P"); }
    same(".rtc non riscritto se non cambia", get(q), "T1");
    return errors != 0;
}
C
} > t.c
gcc -g -O0 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -fsanitize=undefined -fsanitize-undefined-trap-on-error -o t t.c
./t

echo "--- blocco del Wi-Fi"
ok() { echo "  ok: $*"; }
FAIL=0
ko() { echo "  ERRORE: $*"; FAIL=1; }
check() { m=$1; shift; if "$@"; then ok "$m"; else ko "$m"; fi; }
export ROOT=$W/sd ARCADE_LOG_DIR=$W/sd/Logs WIFI_LOCK=$W/lock WIFI_LOCK_TRIES=10
LOG=$W/sd/Logs/wifi.log
CTL=/trimui/m2/wifi-ctl
if sh $CTL off && grep -q 'Wi-Fi spento' $LOG; then ok "blocco libero: il comando viene eseguito"; else ko "blocco libero"; fi
sh -c "exec 9> $WIFI_LOCK; flock -n 9; exec sleep 30" &
HOLD=$!
for i in $(seq 50); do (exec 8> $WIFI_LOCK; flock -n 8) || break; sleep 0.1; done
: > $LOG
if sh $CTL off; then ko "comando eseguito senza il blocco"; fi
if grep -q rinuncio $LOG && ! grep -q 'Wi-Fi spento' $LOG; then ok "blocco occupato: il comando non viene eseguito"; else ko "blocco occupato"; fi
if (exec 8> $WIFI_LOCK; flock -n 8); then ko "blocco tolto a chi lo teneva"; fi
ok "il blocco resta a chi lo teneva"
kill $HOLD; wait $HOLD 2>/dev/null || true
: > $LOG
if sh $CTL off && grep -q 'Wi-Fi spento' $LOG; then ok "comando interrotto: il blocco si libera da solo"; else ko "comando interrotto"; fi
[ "$(grep -c 'wpa_.*9>&-' $CTL)" = 2 ] && ok "i demoni non ereditano il blocco" || ko "i demoni ereditano il blocco"

check "ricerca delle reti non affidata al driver" grep -q 'wpa_supplicant -B .*-I /run/wpa-overlay.conf' $CTL
check "file delle reti vuoto: si ricrea" grep -q 'if \[ ! -s "$CONF" \]; then' $CTL
check "impostazione scritta nel file aggiuntivo" grep -q 'disable_scan_offload=1 > /run/wpa-overlay.conf' $CTL
check "attesa prima di wpa_supplicant" grep -q '^    sleep 0.5' $CTL

echo "--- trimui_inputd fermo mentre il Wi-Fi si accende"
sed -n '/^INPUTD_PAUSE=/,/^case "\$1" in/p' $CTL | sed '$d' > $W/led.sh
: > $W/i2c-3; rm -f $W/inputd-pausa $W/kill.log
inp() { (WIFI_I2C=$W/i2c-3 WIFI_INPUTD_PAUSE=$W/inputd-pausa; pidof() { echo 4242; }; kill() { echo "$*" >> $W/kill.log; }; . $W/led.sh; "$@"); }
inp inputd_pause A
[ "$(cat $W/kill.log)" = "-STOP 4242" ] && [ -e $W/inputd-pausa ] && ok "accensione: trimui_inputd fermo" || ko "non fermato: $(cat $W/kill.log)"
inp inputd_pause B; : > $W/kill.log
inp inputd_resume A
[ ! -s $W/kill.log ] && ok "il timer di un'accensione vecchia non lo fa ripartire" || ko "ripartito dal timer vecchio"
inp inputd_resume B
[ "$(cat $W/kill.log)" = "-CONT 4242" ] && [ ! -e $W/inputd-pausa ] && ok "riparte al collegamento" || ko "non ripartito: $(cat $W/kill.log)"
: > $W/kill.log; inp inputd_resume ""
[ ! -s $W/kill.log ] && ok "senza pausa in corso non succede nulla" || ko "ripresa senza pausa"
rm -f $W/i2c-3; inp inputd_pause C
[ ! -s $W/kill.log ] && [ ! -e $W/inputd-pausa ] && ok "senza bus I2C 3 (Smart Pro) non lo ferma" || ko "fermato senza bus I2C 3"
awk '/exec "\$0" resume "\$tok"/{t=NR} /inputd_pause "\$tok"/{p=NR} END{exit !(t&&p>t)}' $CTL && ok "il timer parte prima della pausa" || ko "timer dopo la pausa"
awk '/^off\)/{o=1} o&&/inputd_resume ""/{f=1} END{exit !f}' $CTL && ok "spegnendo il Wi-Fi riparte comunque" || ko "off non lo fa ripartire"
grep -q 'wifi-ctl resume' /trimui/m2/wifi-event && ok "wifi-event lo fa ripartire al collegamento" || ko "wifi-event non lo riprende"
awk '/inputd_pause "\$tok"/{p=NR} /insmod/{if(p&&!i)i=NR} END{exit !(p&&i>p)}' $CTL && ok "pausa prima di caricare il driver" || ko "pausa dopo il driver"
# ripresa dopo la prima ricerca delle reti: wpa_cli finto, risultati al terzo controllo
scan() {   # scan RETI_SALVATE: stampa i comandi di wpa_cli e le chiamate a kill
    : > $W/kill.log; : > $W/wpa.log; rm -f $W/n; printf 'A\n4242\n' > $W/inputd-pausa
    [ "$1" = 1 ] && printf 'network={\n}\n' > $W/conf || : > $W/conf
    (WIFI_I2C=$W/i2c-3 WIFI_INPUTD_PAUSE=$W/inputd-pausa CONF=$W/conf CTRL=/run/wpa WIFI_SCAN_TRIES=${2:-45}
     log() { echo "$*" >> $W/scan-log; }; sleep() { :; }; kill() { echo "$*" >> $W/kill.log; }
     wpa_cli() { eval "c=\${$#}"; echo "$c" >> $W/wpa.log
         if [ "$c" = scan_results ]; then n=$(($(cat $W/n 2>/dev/null || echo 0) + 1)); echo $n > $W/n
             echo "bssid / frequency / signal level / flags / ssid"
             [ $n -ge 3 ] && [ "${NORETI:-}" != 1 ] && printf '00:11:22:33:44:55\t2412\t-50\t[WPA2]\tuno\naa:bb:cc:dd:ee:ff\t2437\t-70\t[ESS]\tdue\n'; fi; }
     . $W/led.sh; inputd_after_scan A)
}
: > $W/scan-log; scan 1
check "dopo la prima ricerca: tasti ripresi" [ "$(cat $W/kill.log)" = "-CONT 4242" ]
check "con reti salvate nessuna ricerca chiesta" [ "$(grep -c '^scan$' $W/wpa.log)" = 0 ]
check "registro: tasti ripresi con il numero di reti" grep -q 'prima ricerca delle reti (2)' $W/scan-log
scan 0
check "senza reti salvate la ricerca si chiede" [ "$(head -1 $W/wpa.log)" = scan ]
check "e i tasti riprendono" [ "$(cat $W/kill.log)" = "-CONT 4242" ]
NORETI=1 scan 1 5
check "nessuna rete intorno: restano fermi (timer di 10 s)" [ ! -s $W/kill.log ]
check "nessuna rete intorno: il segno della pausa resta" [ -e $W/inputd-pausa ]
printf 'B\n4242\n' > $W/inputd-pausa; : > $W/kill.log
(WIFI_I2C=$W/i2c-3 WIFI_INPUTD_PAUSE=$W/inputd-pausa CONF=$W/conf CTRL=/run/wpa; log() { :; }; sleep() { :; }; kill() { echo "$*" >> $W/kill.log; }
 wpa_cli() { echo "00:11:22:33:44:55	2412	-50	[ESS]	uno"; }; . $W/led.sh; inputd_after_scan A)
check "accensione più nuova: non riprende la pausa di un'altra" [ ! -s $W/kill.log ]
rm -f $W/inputd-pausa
awk '/logs "Wi-Fi acceso"/{a=NR} /inputd_after_scan "\$tok" \) .*9>&- &/{b=NR} END{exit !(a&&b>a)}' $CTL && ok "controllo della ricerca in fondo, senza il blocco" || ko "controllo della ricerca mancante"
grep -q 'exec "\$0" resume "\$tok" ) > /dev/null 2>&1 9>&- &' $CTL && ok "il timer non tiene il blocco" || ko "timer con il blocco"
! grep -q 'led_pause\|led-anim\|led_anim' $CTL && ok "LED non toccati dal Wi-Fi" || ko "pausa dei LED ancora presente"

echo "--- avvio ritardato del Wi-Fi"
mkdir -p $W/fake; printf '#!/bin/sh\necho "$*" >> %s/sleep.log\n' $W > $W/fake/sleep; printf '#!/bin/sh\necho "$*" >> %s/ctl.log\n' $W > $W/fake/wifi-ctl; chmod +x $W/fake/*
RIGA=$(grep 'sleep 15' /trimui/m2/system-init)
rm -f /run/wifi-touched
(PATH=$W/fake:$PATH; eval "$RIGA"; wait)
check "aspetta 15 secondi" [ "$(cat $W/sleep.log 2>/dev/null)" = 15 ]
check "poi accende il Wi-Fi" [ "$(cat $W/ctl.log 2>/dev/null)" = on ]
rm -f $W/ctl.log; touch /run/wifi-touched
(PATH=$W/fake:$PATH; eval "$RIGA"; wait)
check "se si è già toccato il Wi-Fi dal menu non lo riaccende" [ ! -e $W/ctl.log ]
rm -f /run/wifi-touched

echo "--- calibrazione delle levette"
cat > cal.c <<'C'
#include "stickcal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* lo stesso calcolo di trimui_inputd: valore mandato a SDL da valore grezzo e configurazione */
static int to_sdl(int raw, int min, int max, int zero) {
    int f = 0;
    if (raw > zero) { f = (raw - zero) * 32760 / (max - zero); if (f > 32760) f = 32760; }
    else if (raw < zero) { f = (raw - zero) * 32760 / (zero - min); if (f < -32760) f = -32760; }
    return -f;
}
static int bad;
static void chk(const char *m, int cond) { printf("  %s: %s\n", cond ? "ok" : "ERRORE", m); if (!cond) bad = 1; }
static char *slurp(const char *p) { static char b[512]; FILE *f = fopen(p, "r"); if (!f) return NULL; size_t n = fread(b, 1, 511, f); b[n] = 0; fclose(f); return b; }
static void put(const char *p, const char *t) { FILE *f = fopen(p, "w"); fputs(t, f); fclose(f); }
int main(void) {
    int worst = 0;
    for (int raw = 700; raw <= 3700; raw += 7) { int d = stickcal_raw(to_sdl(raw, 0, 4095, 300)) - raw; if (d < 0) d = -d; if (d > worst) worst = d; }
    chk("dal valore mandato a SDL si risale a quello grezzo", worst <= 1);
    chk("un valore fuori scala non viene accettato", stickcal_raw(32000) == -1);
    stickcal_t c; for (int a = 0; a < SC_AXES; a++) { c.min[a] = 1000 + a; c.zero[a] = 2100; c.max[a] = 3200 - a; }
    chk("misura plausibile accettata", stickcal_ok(&c));
    stickcal_t d = c; d.max[SC_RY] = 2200; chk("levetta non portata in fondo: rifiutata", !stickcal_ok(&d));
    d = c; d.zero[SC_LX] = 1000; chk("centro impossibile: rifiutato", !stickcal_ok(&d));
    d = c; d.max[SC_LX] = 2400; chk("corsa corta: rifiutata con la soglia della Smart Pro", !stickcal_ok(&d));
    stickcal_set_min_span(250); chk("corsa corta: accettata con la soglia della Brick Pro", stickcal_ok(&d));
    d.max[SC_LX] = 2300; chk("corsa troppo corta: rifiutata anche sulla Brick Pro", !stickcal_ok(&d));
    stickcal_set_min_span(450);
    const char *S = "/tmp/fl/cal/sys", *U = "/tmp/fl/cal/ud";
    chk("all'inizio nessuna calibrazione", !stickcal_has_custom(S));
    chk("valori di misura scritti", stickcal_begin(U, false));
    chk("zero di misura lontano dal centro vero", strstr(slurp("/tmp/fl/cal/ud/joypad.config"), "x_zero=300") != NULL);
    stickcal_cancel(U, false);
    chk("annullando senza file di prima: file tolto", slurp("/tmp/fl/cal/ud/joypad.config") == NULL);
    put("/tmp/fl/cal/ud/joypad.config", "x_min=1\nz_min=111\nz_max=999\ndeadzone=0.33\n");
    stickcal_begin(U, true);
    char *t = slurp("/tmp/fl/cal/ud/joypad.config");
    chk("Brick Pro: zona morta spenta durante la misura", strstr(t, "deadzone=0.00") && strstr(t, "z_min=111") && strstr(t, "z_max=999"));
    stickcal_cancel(U, true);
    chk("annullando torna il file di prima", strstr(slurp("/tmp/fl/cal/ud/joypad.config"), "x_min=1\n") != NULL);
    stickcal_begin(U, true);
    chk("salvataggio riuscito", stickcal_save(S, U, &c, true));
    t = slurp("/tmp/fl/cal/sys/joypad.config");
    chk("sulla SD il sinistro con limiti un filo dentro", strstr(t, "x_min=1033\n") && strstr(t, "x_max=3167\n") && strstr(t, "x_zero=2100\n"));
    chk("Brick Pro: grilletti e zona morta conservati", strstr(t, "z_min=111") && strstr(t, "deadzone=0.33"));
    chk("destro scritto nel suo file", strstr(slurp("/tmp/fl/cal/ud/joypad_right.config"), "x_min=1034\n") != NULL);
    chk("calibrazione presente", stickcal_has_custom(S));
    d = c; d.zero[0] = 1;
    put("/tmp/fl/cal/sys/joypad.config", "buono");
    stickcal_begin(U, false);
    chk("misura sbagliata non salvata", !stickcal_save(S, U, &d, false));
    chk("la SD resta com'era", !strcmp(slurp("/tmp/fl/cal/sys/joypad.config"), "buono"));
    stickcal_reset(S, U, false);
    chk("ripristino: file tolti", !stickcal_has_custom(S) && slurp("/tmp/fl/cal/ud/joypad.config") == NULL);
    return bad;
}
C
mkdir -p $W/cal/sys $W/cal/ud
gcc -g -O0 -Wall -Wextra -D_DEFAULT_SOURCE -I/trimui/frontend/src/launcher -fsanitize=undefined -fsanitize-undefined-trap-on-error -o calprova cal.c /trimui/frontend/src/launcher/stickcal.c
if ./calprova; then :; else ko "calibrazione delle levette"; fi

echo "--- JSON senza memoria"
cat > oom.c <<'C'
#include <stdlib.h>
#include "json.h"
static int left;
void *tm(size_t n) { return left-- == 0 ? NULL : malloc(n); }
void *tc(size_t a, size_t b) { return left-- == 0 ? NULL : calloc(a, b); }
int main(void) {
    const char *t = "{\"a\":[1,\"x\",{\"b\":true,\"n\":null,\"f\":false}],\"c\":\"y\"}";
    int failed = 0;
    for (int n = 0; n < 40; n++) { left = n; jval *v = json_parse(t); if (!v) failed++; json_free(v); }
    left = 1 << 30;
    jval *v = json_parse(t);
    int ok = v != NULL && failed > 0;
    json_free(v);
    return ok ? 0 : 1;
}
C
gcc -g -O0 -c -Dmalloc=tm -Dcalloc=tc -I/trimui/frontend/src/common -o json-oom.o /trimui/frontend/src/common/json.c
gcc -g -O0 -I/trimui/frontend/src/common -o oom oom.c json-oom.o -lm
check "ogni allocazione può fallire senza far cadere il programma" ./oom

echo "--- processi lasciati dai giochi Linux"
gcc -std=c11 -O2 -D_DEFAULT_SOURCE -o gw /trimui/frontend/src/tools/gamewatch.c
cat > gioco.sh <<SH
(trap "" TERM; exec sleep 60) &
echo \$! > $W/figlio-duro
sleep 60 &
echo \$! > $W/figlio
exit 7
SH
vivo() { [ -d /proc/$1 ] && ! grep -q ') Z ' /proc/$1/stat; }
set +e; ./gw -- sh gioco.sh 2>/dev/null; rc=$?; set -e
check "il codice di uscita del gioco resta quello" [ "$rc" = 7 ]
A=$(cat $W/figlio); B=$(cat $W/figlio-duro)
if vivo $A; then ko "processo rimasto attivo"; else ok "processo normale chiuso"; fi
if vivo $B; then ko "processo che ignora la chiusura rimasto attivo"; else ok "processo che ignora la chiusura eliminato"; fi

echo "--- controllo delle librerie nella costruzione"
LC="python3 -I /trimui/toolchain/libclosure.py"
R2=$W/rootfs; mkdir -p $R2/lib
cat > lib.c <<'C'
int f(void) { return 1; }
C
cat > uno.map <<'M'
FOO_1 { global: f; local: *; };
M
cat > due.map <<'M'
FOO_2 { global: f; local: *; };
M
echo 'int f(void); int main(void) { return f(); }' > prog.c
gcc -shared -Wl,-soname,libfoo.so.1 -Wl,--version-script=due.map -o libfoo-2.so lib.c
gcc -shared -Wl,-soname,libfoo.so.1 -Wl,--version-script=uno.map -o libfoo-1.so lib.c
cp libfoo-2.so libfoo.so; gcc -o prog prog.c -L. -l:libfoo.so -Wl,-rpath-link,.
cp /lib/ld-musl-*.so.1 $R2/lib/; L=$(readelf -d prog | sed -n 's/.*NEEDED.*\[\(libc[^]]*\)\].*/\1/p'); cp /lib/ld-musl-*.so.1 $R2/lib/$L
cp libfoo-2.so $R2/lib/libfoo.so.1
check "tutto presente: la costruzione prosegue" $LC $R2 $W/prog
cp libfoo-1.so $R2/lib/libfoo.so.1
if $LC $R2 $W/prog >/dev/null 2>&1; then ko "versione di simboli mancante non rilevata"; else ok "versione di simboli mancante: si ferma"; fi
rm $R2/lib/libfoo.so.1
if $LC $R2 $W/prog >/dev/null 2>&1; then ko "libreria mancante non rilevata"; else ok "libreria mancante: si ferma"; fi
echo testo > nonelf
if $LC $R2 $W/nonelf >/dev/null 2>&1; then ko "file non valido non rilevato"; else ok "file non valido: si ferma"; fi
[ $FAIL = 0 ] || { echo "CI SONO ERRORI"; exit 1; }
echo "tutte le prove passate"
