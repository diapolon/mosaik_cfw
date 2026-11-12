#!/bin/sh
# Controllo di compatibilità (verifica audit): la glibc chiesta da ogni programma, app e core non
# deve superare quella del firmware TrimUI, altrimenti sulla console non parte.
#   sh toolchain/check-glibc.sh VERSIONE_DEL_FIRMWARE FILE...
# Esce con 1 se un file chiede di più. Lo usa "make -f Makefile.tg5040 check".
max=${1:-2.33}
[ -n "$max" ] || max=2.33
shift
bad=0
for f in "$@"; do
    [ -f "$f" ] || continue
    v=$(aarch64-linux-gnu-objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1 | sed 's/GLIBC_//')
    echo "$f: GLIBC_$v"
    [ -n "$v" ] || continue
    top=$(printf '%s\n%s\n' "$v" "$max" | sort -V | tail -1)
    if [ "$top" != "$max" ]; then
        echo "  ERRORE: chiede glibc $v, il firmware ha $max"
        bad=1
    fi
done
echo "glibc del firmware: $max"
exit $bad
