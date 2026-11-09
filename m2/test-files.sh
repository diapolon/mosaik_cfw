#!/bin/sh
# Prova delle scritture che non devono perdere dati e del blocco del Wi-Fi. Il codice è quello vero,
# estratto dai sorgenti; gli errori della scheda sono simulati:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-files.sh
set -e
apk add -q build-base >/dev/null
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
#define fsync test_fsync
enum { BTN_COUNT = 20, ROT_NONE = 0 };
static const int default_map[16] = { 0 };
static struct { struct { int aspect, bezel; char filter[32]; } set; } UI;
static const char *filter_legacy_id(int n) { (void)n; return "nitido"; }
static void ui_log(const char *f, ...) { (void)f; }
static void game_file(char *out, size_t n, const char *ext) { snprintf(out, n, "%s/g.%s", dir, ext); }
typedef struct SDL_Surface SDL_Surface;
static SDL_Surface *thumb;
static const char *payload = "";
static size_t ser_size(void) { return strlen(payload); }
static bool ser(void *b, size_t n) { memcpy(b, payload, n); return true; }
static struct { size_t (*serialize_size)(void); bool (*serialize)(void *, size_t); } core = { ser_size, ser };
static char root[512], core_path[8] = "c", rom_path[8] = "r", title[8] = "t";
static void IMG_SavePNG(SDL_Surface *s, const char *p) { (void)s; FILE *f = fopen(p, "w"); fputs(payload, f); fclose(f); }
C
awk '/^static struct \{$/ {s=""; on=1} on {s=s $0 "\n"} /^} [a-z_]*;/ {if ($2=="gcfg;") {printf "%s", s; exit} on=0}' $R
grep -E '^enum \{ NSLOTS|^static const char \*const save_ext' $R
for f in gamecfg_load state_path; do fn $R "^static void $f"; done
fn $R '^static bool copy_whole'
fn $R '^static void auto_keep_previous'
fn $R '^static bool state_save'
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
    save("B", true);
    file_is("secondo salvataggio", "auto.state", "B");
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
    same("scritta", get(p), "libretro\tc\tr\tt\tresume\n");
    put(p, "vecchia"); fail_fsync = 1; write_resume(); fail_fsync = 0;
    same("scrittura fallita: resta quella di prima", get(p), "vecchia");
    snprintf(q, sizeof q, "%s.tmp", p); same("nessun provvisorio", get(q), "(manca)");

    puts("--- preferiti, recenti, collezioni, nascosti");
    char keys[2][96] = { "uno", "due" };
    snprintf(p, sizeof p, "%s/lista.txt", root); snprintf(q, sizeof q, "%s.tmp", p);
    put(p, "vecchia\n");
    write_list(p, keys, 2);
    same("scritta", get(p), "uno\ndue\n");
    put(p, "vecchia\n"); fail_fsync = 1; write_list(p, keys, 2); fail_fsync = 0;
    same("scrittura fallita: resta quella di prima", get(p), "vecchia\n");
    same("nessun provvisorio", get(q), "(manca)");
    fail_rename_to = p; write_list(p, keys, 2); fail_rename_to = NULL;
    same("sostituzione fallita: resta quella di prima", get(p), "vecchia\n");
    same("nessun provvisorio", get(q), "(manca)");
    return errors != 0;
}
C
} > t.c
gcc -g -O0 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -fsanitize=undefined -fsanitize-undefined-trap-on-error -o t t.c
./t

echo "--- blocco del Wi-Fi"
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; exit 1; }
export ROOT=$W/sd ARCADE_LOG_DIR=$W/sd/Logs WIFI_LOCK=$W/lock WIFI_LOCK_TRIES=10
LOG=$W/sd/Logs/wifi.log
CTL=/trimui/m2/wifi-ctl
sh $CTL off && grep -q 'Wi-Fi spento' $LOG && ok "blocco libero: il comando viene eseguito"
sh -c "exec 9> $WIFI_LOCK; flock -n 9; exec sleep 30" &
HOLD=$!
for i in $(seq 50); do (exec 8> $WIFI_LOCK; flock -n 8) || break; sleep 0.1; done
: > $LOG
if sh $CTL off; then ko "comando eseguito senza il blocco"; fi
grep -q rinuncio $LOG && ! grep -q 'Wi-Fi spento' $LOG && ok "blocco occupato: il comando non viene eseguito"
if (exec 8> $WIFI_LOCK; flock -n 8); then ko "blocco tolto a chi lo teneva"; fi
ok "il blocco resta a chi lo teneva"
kill $HOLD; wait $HOLD 2>/dev/null || true
: > $LOG
sh $CTL off && grep -q 'Wi-Fi spento' $LOG && ok "comando interrotto: il blocco si libera da solo"
[ "$(grep -c 'wpa_.*9>&-' $CTL)" = 2 ] && ok "i demoni non ereditano il blocco" || ko "i demoni ereditano il blocco"
echo "tutte le prove passate"
