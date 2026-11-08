#!/bin/sh
# Prova di impostazioni e lettore JSON con valori impossibili, sotto UBSan (si ferma al primo errore):
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-settings.sh
set -e
apk add -q build-base >/dev/null
W=/tmp/st; rm -rf $W; mkdir -p $W/System; cd $W
cat > t.c <<'C'
#include "settings.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const char *filter_legacy_id(int n) { static const char *ids[] = { "nitido", "morbido", "scanline" }; return ids[n >= 0 && n < 3 ? n : 0]; }
int main(int c, char **v) {
    (void)c;
    settings_t s;
    settings_defaults(&s);
    settings_load(&s, v[1]);
    printf("%d %d %d %d %d %d %d %d %d %d %s\n", s.sort_mode, s.cpu_profile, s.saver, s.aspect, s.brightness, s.volume, s.dup_mode, s.autosleep_min, s.autosuspend_min, s.log_level, s.filter);
    const char *bad[] = { "{\"a\":\"\\", "[-]", "-", "{\"x\":-}", NULL };
    for (int i = 0; bad[i]; i++) json_free(json_parse(bad[i]));
    char *b = malloc(2000002);
    memset(b, '[', 2000001);
    b[2000001] = 0;
    json_free(json_parse(b));
    free(b);
    return 0;
}
C
gcc -g -O0 -fsanitize=undefined,float-cast-overflow -fsanitize-undefined-trap-on-error -I/trimui/frontend/src/common -o t t.c /trimui/frontend/src/common/settings.c /trimui/frontend/src/common/json.c
try() { echo "$1" > System/settings.json; ./t . ; }
expect() { [ "$1" = "$2" ] && echo "  ok: $3" || { echo "  ERRORE: $3: atteso [$2], ottenuto [$1]"; exit 1; }; }
D="0 1 0 0 8 12 1 0 5 0"
expect "$(try '{"sort":-1,"cpu_profile":-1,"screensaver":-1,"aspect":-7,"brightness":99,"volume":1e300,"duplicates":-3,"autosleep_min":-5,"autosuspend_min":99999999,"debug_level":9}')" "$D nitido" "valori impossibili"
expect "$(try '{"filter":1e300}')" "$D nitido" "filtro numerico enorme"
expect "$(try '{"filter":2}')" "$D scanline" "filtro numerico valido"
expect "$(try '{"sort":3,"cpu_profile":2,"debug_level":3}')" "3 2 0 0 8 12 1 0 5 3 nitido" "valori validi"
echo "TUTTO OK"
