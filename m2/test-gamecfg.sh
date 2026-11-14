#!/bin/sh
# Impostazioni a strati del runner (frontend/src/runner/gamecfg.c): file vecchi, strato Console,
# scrittura solo delle scelte, parametri dello shader, scheda in sola lettura.
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-gamecfg.sh
set -e
apk add -q build-base > /dev/null 2>&1
W=$(mktemp -d)
S=/trimui/frontend/src
cat > $W/t.c <<'EOF'
#include "gamecfg.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok: %s\n", msg); else { printf("  ERRORE: %s\n", msg); fails++; } } while (0)

static const char *legacy(int n) { return n == 1 ? "scanline" : "nitido"; }
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int has_line(const char *p, const char *l) {
    char b[200]; FILE *f = fopen(p, "r"); int r = 0;
    if (!f) return 0;
    while (fgets(b, sizeof b, f)) { b[strcspn(b, "\n")] = 0; if (!strcmp(b, l)) r = 1; }
    fclose(f);
    return r;
}

int main(int argc, char **argv) {
    char p[300], q[300];
    const char *d = argv[1];
    (void)argc;
    gc_layer_t g, c;
    gc_vals_t gen = { .aspect = 0, .bezel = 1, .cpu = 0, .filter = "nitido" };
    for (int i = 0; i < 16; i++) gen.map[i] = i;
    bool v2;

    /* 1. file vecchio: voci uguali alle generali ereditate, le altre restano */
    snprintf(p, sizeof p, "%s/vecchio.cfg", d);
    put(p, "aspect=0\nfilter=crt\nbezel=1\nrotate=1\ncpu=0\nslot=2\nmap.0=5\nsp.curv=0.5\n");
    CHECK(gc_read(p, &g, &v2, legacy) && !v2, "file vecchio letto, senza versione");
    gc_migrate(&g, &gen);
    CHECK(!g.has_aspect && !g.has_bezel && !g.has_cpu, "formato, cornice e processore uguali alle generali: ereditati");
    CHECK(g.has_filter && !strcmp(g.filter, "crt"), "filtro diverso: resta scelta del gioco");
    CHECK(g.rotate == 1 && g.slot == 2, "rotazione e slot restano");
    CHECK(g.has_map[0] && g.map[0] == 5 && g.nsp == 1, "tasti e parametri dello shader restano");
    CHECK(gc_write(p, &g), "riscritto");
    CHECK(has_line(p, "versione=2") && has_line(p, "filter=crt") && !has_line(p, "aspect=0") && !has_line(p, "bezel=1"), "nel file nuovo solo le scelte, con versione=2");
    gc_layer_t g2;
    CHECK(gc_read(p, &g2, &v2, legacy) && v2 && g2.has_filter && !g2.has_aspect, "riletto uguale, versione 2: niente migrazione");

    /* 2. filtro vecchio scritto come numero */
    snprintf(q, sizeof q, "%s/numero.cfg", d);
    put(q, "filter=1\n");
    gc_read(q, &g2, &v2, legacy);
    CHECK(g2.has_filter && !strcmp(g2.filter, "scanline"), "filtro numerico tradotto");

    /* 3. strati: generale -> console -> gioco */
    gc_clear(&c);
    c.has_aspect = true; c.aspect = 2;
    c.has_filter = true; snprintf(c.filter, sizeof c.filter, "lcd");
    gc_clear(&g);
    g.has_filter = true; snprintf(g.filter, sizeof g.filter, "crt");
    gc_vals_t v = gen;
    gc_apply(&v, &c);
    gc_apply(&v, &g);
    CHECK(v.aspect == 2, "formato dalla console");
    CHECK(!strcmp(v.filter, "crt"), "filtro dal gioco, sopra la console");
    CHECK(v.bezel == 1, "cornice dalle generali");

    /* 4. file vuoto: si toglie */
    gc_clear(&g);
    CHECK(gc_empty(&g), "strato vuoto");
    CHECK(gc_write(p, &g) && access(p, F_OK) != 0, "strato vuoto: file tolto");
    CHECK(gc_write(p, &g), "togliere un file che non c'è non è un errore");
    g.slot = 1;
    CHECK(!gc_empty(&g) && gc_write(p, &g) && has_line(p, "slot=1"), "solo lo slot: il file resta");

    /* 5. parametri dello shader */
    gc_clear(&g);
    gc_sp_set(&g, "curv", 0.3f);
    gc_sp_set(&g, "scan", 0.7f);
    gc_sp_set(&g, "curv", 0.4f);
    CHECK(g.nsp == 2 && g.sp_val[gc_sp_find(&g, "curv")] > 0.39f, "parametro cambiato, non doppio");
    gc_sp_del(&g, "curv");
    CHECK(g.nsp == 1 && gc_sp_find(&g, "curv") < 0 && gc_sp_find(&g, "scan") == 0, "parametro tolto");
    char lungo[80];
    memset(lungo, 'x', 60); lungo[60] = 0;
    gc_sp_set(&g, lungo, 1);
    CHECK(g.nsp == 1, "nome troppo lungo ignorato");

    /* 6. valori fuori misura */
    snprintf(q, sizeof q, "%s/strano.cfg", d);
    put(q, "aspect=9\ncpu=7\nmap.40=1\nmap.3=999\nsp.=1\nslot=5\n");
    gc_read(q, &g2, &v2, legacy);
    CHECK(!g2.has_aspect && !g2.has_cpu && !g2.has_map[3] && g2.nsp == 0 && g2.slot == 0, "valori fuori misura scartati");

    /* 7. file che non c'è */
    snprintf(q, sizeof q, "%s/non-ce.cfg", d);
    CHECK(!gc_read(q, &g2, &v2, legacy) && gc_empty(&g2), "file assente: strato vuoto");

    /* 8. cartella in sola lettura: niente file a metà */
    if (geteuid() != 0) {
        snprintf(q, sizeof q, "%s/ro", d);
        mkdir(q, 0555);
        snprintf(p, sizeof p, "%s/ro/gioco.cfg", d);
        g.has_aspect = true;
        CHECK(!gc_write(p, &g) && access(p, F_OK) != 0, "sola lettura: scrittura fallita, nessun file");
    } else printf("  (sola lettura: saltata, si gira come root)\n");

    if (fails) { printf("PROVE FALLITE: %d\n", fails); return 1; }
    printf("TUTTO OK\n");
    return 0;
}
EOF
gcc -std=c11 -O1 -Wall -Wextra -D_DEFAULT_SOURCE -I$S/runner -o $W/t $W/t.c $S/runner/gamecfg.c
mkdir -p $W/d
$W/t $W/d
