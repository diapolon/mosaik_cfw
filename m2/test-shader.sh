#!/bin/sh
# Filtri shader compilati come li compila il runner sulla console: GLSL ES 1.00 (#version 100), una
# volta con VERTEX e una con FRAGMENT (runner/glshader.c). Sul Mac il runner non usa OpenGL e un
# errore si vede solo sulla console (14 novembre 2026: "flat" è una parola riservata).
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-shader.sh
apk add -q glslang > /dev/null 2>&1
ok=0; ko=0
for g in $(find /trimui/frontend/filters /trimui/frontend/package/filters -name '*.glsl' | sort); do
    for st in vert frag; do
        [ $st = vert ] && D=VERTEX || D=FRAGMENT
        { printf '#version 100\n#define %s\n#define PARAMETER_UNIFORM\n' $D; cat "$g"; } > /tmp/s.$st
        if out=$(glslangValidator -S $st /tmp/s.$st 2>&1); then
            ok=$((ok + 1))
        else
            ko=$((ko + 1))
            echo "  ERRORE: ${g#/trimui/} ($st)"
            echo "$out" | grep -i error | head -3 | sed 's/^/    /'
        fi
    done
done
echo
if [ $ko -eq 0 ]; then echo "TUTTO OK ($ok compilazioni)"; else echo "$ko COMPILAZIONI FALLITE"; exit 1; fi
