#!/bin/sh
# Sottotitoli .srt dell'app Video (frontend/src/apps/srt.h), sotto UBSan (trap), con file
# a capo Unix, Windows e vecchio Mac, righe vuote con spazi, blocchi senza riga
# vuota, BOM, Latin-1, tag, ordine, file vuoti o rovinati.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-srt.sh
set -e
apk add -q build-base > /dev/null
W=/tmp/srt; rm -rf $W; mkdir -p $W; cd $W
errors=0

cat > t.c <<'C'
#include "srt.h"
static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = malloc(4 << 20);   /* come l'app Video */
    size_t n = fread(b, 1, (4 << 20) - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}
/* stampa: numero di battute, Latin-1, poi una riga per battuta con inizio, fine e testo (\n come |) */
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char *b = slurp(argv[1]);
    if (!b) return 2;
    sub_t *s = NULL;
    bool l1 = false;
    int n = srt_parse(b, &s, &l1);
    printf("%d %s\n", n, l1 ? "latin1" : "utf8");
    for (int i = 0; i < n; i++) {
        printf("%.3f %.3f ", s[i].a, s[i].b);
        for (char *p = s[i].text; *p; p++) putchar(*p == '\n' ? '|' : *p);
        putchar('\n');
        free(s[i].text);
    }
    free(s);
    free(b);
    return 0;
}
C
gcc -O1 -g -Wall -Wextra -Werror -fsanitize=undefined,float-cast-overflow -fsanitize-undefined-trap-on-error -I/trimui/frontend/src/apps -o t t.c

check() {   # descrizione, file, uscita attesa
    got=$(./t "$2" 2>&1) || true
    if [ "$got" = "$3" ]; then echo "  ok: $1"; else echo "  ERRORE: $1"; echo "    atteso:  $(echo "$3" | tr '\n' '/')"; echo "    ottenuto: $(echo "$got" | tr '\n' '/')"; errors=$((errors + 1)); fi
}

TRE='3 utf8
1.000 2.500 Prima battuta
3.000 4.000 Seconda|su due righe
5.000 6.000 Terza'

printf '1\n00:00:01,000 --> 00:00:02,500\nPrima battuta\n\n2\n00:00:03,000 --> 00:00:04,000\nSeconda\nsu due righe\n\n3\n00:00:05,000 --> 00:00:06,000\nTerza\n' > lf.srt
check "a capo Unix (\\n): tre battute (audit 0.9.88, APP-01)" lf.srt "$TRE"
sed 's/$/\r/' lf.srt > crlf.srt
check "a capo Windows (\\r\\n)" crlf.srt "$TRE"
tr '\n' '\r' < lf.srt > cr.srt
check "a capo del vecchio Mac (\\r)" cr.srt "$TRE"
printf '1\n00:00:01,000 --> 00:00:02,500\nPrima battuta\n   \n2\n00:00:03,000 --> 00:00:04,000\nSeconda\nsu due righe\n\t\n3\n00:00:05,000 --> 00:00:06,000\nTerza' > spazi.srt
check "righe vuote con spazi e tab, niente a capo in fondo" spazi.srt "$TRE"
printf '1\n00:00:01,000 --> 00:00:02,500\nPrima battuta\n2\n00:00:03,000 --> 00:00:04,000\nSeconda\nsu due righe\n3\n00:00:05,000 --> 00:00:06,000\nTerza\n' > attaccati.srt
check "blocchi senza riga vuota in mezzo" attaccati.srt "$TRE"
printf '\n\n\n1\n00:00:01,000 --> 00:00:02,500\nPrima battuta\n\n\n\n2\n00:00:03,000 --> 00:00:04,000\nSeconda\nsu due righe\n\n\n3\n00:00:05,000 --> 00:00:06,000\nTerza\n\n\n' > vuote.srt
check "più righe vuote" vuote.srt "$TRE"
{ printf '\357\273\277'; cat crlf.srt; } > bom.srt
check "BOM UTF-8" bom.srt "$TRE"
printf '2\r\n00:00:05,000 --> 00:00:06,000\r\n<i>Dopo</i>\r\n\r\n1\r\n00:00:01.000 --> 00:00:02.000\r\n{\\an8}Prima\r\n' > ordine.srt
check "tag tolti, punto al posto della virgola, ordine per tempo" ordine.srt '2 utf8
1.000 2.000 Prima
5.000 6.000 Dopo'
printf '1\n00:00:01,000 --> 00:00:02,000\nPerch\351 s\354\n' > latin1.srt
check "Latin-1 portato in UTF-8" latin1.srt "$(printf '1 latin1\n1.000 2.000 Perch\303\251 s\303\254')"
printf '1\n00:00:01,000 --> 00:00:02,000\n\n2\n00:00:03,000 --> 00:00:04,000\nUnica\n' > senzatesto.srt
check "battuta senza testo saltata" senzatesto.srt '1 utf8
3.000 4.000 Unica'
: > vuoto.srt
check "file vuoto" vuoto.srt '0 utf8'
printf '\357\273\277' > solobom.srt
check "solo il BOM" solobom.srt '0 utf8'
printf 'non un sottotitolo\n-->\n00:00 --> x\n' > rotto.srt
check "file rovinato" rotto.srt '0 utf8'
# battute lunghe e tante: nessun errore di memoria, testo tagliato a 600 byte
awk 'BEGIN { for (i = 1; i <= 3000; i++) { printf "%d\n00:%02d:%02d,000 --> 00:%02d:%02d,500\n", i, (i/60)%60, i%60, (i/60)%60, i%60; for (j = 0; j < 40; j++) printf "riga lunga lunga lunga "; printf "\n\n" } }' > grande.srt
n=$(./t grande.srt | head -n 1)
[ "$n" = "3000 utf8" ] && echo "  ok: 3000 battute lunghe" || { echo "  ERRORE: 3000 battute lunghe ($n)"; errors=$((errors + 1)); }
head -c 300000 /dev/urandom > caso.srt
./t caso.srt > /dev/null && echo "  ok: byte a caso senza errori" || { echo "  ERRORE: byte a caso"; errors=$((errors + 1)); }

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
