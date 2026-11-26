#!/bin/sh
# Scheda tolta durante un gioco (25 novembre): guardia vera (m2/guardia-sd.c), FAT vero su un loop
# montato come nell'initramfs e runner vero compilato in Alpine col video finto di SDL, con un core di
# prova che conta i fotogrammi in /tmp/frames e cambia la sua SRAM a ogni fotogramma. Con un .cue
# il core tiene aperto il file del gioco, come l'immagine di un disco della PlayStation.
# Togliere la scheda è svuotare il file del loop e far rileggere la dimensione (come in
# test-guardia-sd.sh); GUARDIA_TEST evita riavvii e spegnimenti veri. Serve --privileged:
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-runner-sd.sh
# prova /trimui/frontend/src/runner/runner.c, /trimui/frontend/src/common/ui.c, /trimui/m2/guardia-sd.c
# e /trimui/frontend/src/tools/gamewatch.c (giochi Linux e app)
apk add -q build-base pkgconf sdl2-dev sdl2_ttf-dev sdl2_image-dev sdl2_mixer-dev curl-dev zlib-dev openssl-dev mesa-dev font-dejavu dosfstools util-linux > /dev/null 2>&1
errors=0
ok() { echo "  ok: $*"; }
ko() { echo "  ERRORE: $*"; errors=$((errors + 1)); }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

mkdir -p /w && cp -r /trimui/frontend /trimui/VERSION /w/ && cp /trimui/UPDATE_URL /w/ 2> /dev/null
cd /w/frontend && rm -rf build sdcard
make build/runner > /tmp/make.log 2>&1 || { cat /tmp/make.log; echo "runner non compilato"; exit 1; }
gcc -std=gnu11 -O2 -Wall -Wextra -o /tmp/guardia /trimui/m2/guardia-sd.c || { echo "guardia non compilata"; exit 1; }

cat > /tmp/core.c <<'X'
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libretro.h"
static uint32_t fb[64 * 64];
static unsigned frame;
static uint8_t sram[64];
static int disc = -1, out = -1;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_environment_t env_cb;
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb) { env_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
RETRO_API void retro_init(void) { out = open("/tmp/frames", O_WRONLY | O_CREAT | O_TRUNC, 0644); }
RETRO_API void retro_deinit(void) {}
RETRO_API void retro_get_system_info(struct retro_system_info *i) {
    memset(i, 0, sizeof *i);
    i->library_name = "prova"; i->library_version = "1"; i->valid_extensions = "bin|cue"; i->need_fullpath = true;
}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *i) {
    memset(i, 0, sizeof *i);
    i->geometry.base_width = i->geometry.max_width = 64;
    i->geometry.base_height = i->geometry.max_height = 64;
    i->timing.fps = 60; i->timing.sample_rate = 48000;
}
RETRO_API void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
RETRO_API void retro_reset(void) { frame = 0; }
RETRO_API bool retro_load_game(const struct retro_game_info *g) {
    enum retro_pixel_format f = RETRO_PIXEL_FORMAT_XRGB8888;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
    size_t n = g && g->path ? strlen(g->path) : 0;
    if (n > 4 && !strcmp(g->path + n - 4, ".cue")) disc = open(g->path, O_RDONLY);   /* un disco: resta aperto */
    return true;
}
RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
RETRO_API void retro_unload_game(void) { if (disc >= 0) close(disc); }
RETRO_API void retro_run(void) {
    poll_cb();
    frame++;
    memcpy(sram, &frame, sizeof frame);
    char b[16];
    if (disc >= 0) { (void)pread(disc, b, 1, 0); }
    for (int i = 0; i < 64 * 64; i++) fb[i] = frame * 2654435761u;
    video_cb(fb, 64, 64, 64 * 4);
    int l = 0; unsigned v = frame; char t[16];
    do { t[l++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < l; i++) b[i] = t[l - 1 - i];
    b[l] = '\n';
    if (out >= 0) (void)pwrite(out, b, (size_t)l + 1, 0);
    if (out >= 0) (void)ftruncate(out, l + 1);
}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API size_t retro_serialize_size(void) { return sizeof frame; }
RETRO_API bool retro_serialize(void *d, size_t n) { if (n < sizeof frame) return false; memcpy(d, &frame, sizeof frame); return true; }
RETRO_API bool retro_unserialize(const void *d, size_t n) { if (n < sizeof frame) return false; memcpy(&frame, d, sizeof frame); return true; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
RETRO_API void *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof sram : 0; }
X
gcc -shared -fPIC -O2 -o /tmp/prova_libretro.so /tmp/core.c -I src/runner || { echo "core di prova non compilato"; exit 1; }

export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ARCADE_NO_AUDIO=1 ARCADE_STANDALONE=1 ARCADE_SD_RUN=/tmp/run
export GUARDIA_TEST=/tmp/azione GUARDIA_RUN=/tmp/run GUARDIA_MNT=/tmp/mnt GUARDIA_USB=/tmp/usb GUARDIA_LED=/tmp/led
mkdir -p /tmp/run /tmp/mnt /tmp/led; echo 0 > /tmp/usb
frames() { v=$(cat /tmp/frames 2> /dev/null); case $v in ''|*[!0-9]*) echo 0 ;; *) echo "$v" ;; esac; }
aspetta() {   # aspetta() SECONDI CONDIZIONE
    i=0; while [ $i -lt $(($1 * 10)) ] && ! eval "$2"; do sleep 0.1; i=$((i + 1)); done; eval "$2"
}

prepara() {   # SD con Arcade, il core e il gioco $1 (gioco.bin o gioco.cue), guardia e runner avviati
    dd if=/dev/zero of=/tmp/sd.img bs=1M count=64 2> /dev/null
    mkfs.vfat -F 32 -i 1234ABCD /tmp/sd.img > /dev/null
    LOOP=$(losetup -f) && losetup "$LOOP" /tmp/sd.img || { echo "serve --privileged"; exit 1; }
    mount -t vfat -o rw,noatime,flush "$LOOP" /tmp/mnt
    R=/tmp/mnt/Arcade
    mkdir -p $R/System $R/Logs $R/Lang $R/Themes/_shared/fonts $R/Roms/PROVA $R/Cores $R/Saves
    cp lang/*.json $R/Lang/; cp -R themes/. $R/Themes/
    # i font condivisi non sono nel repository: quelli della SD simulata, come in test-ui.sh
    cp /trimui/frontend/sdcard/Themes/_shared/fonts/*.ttf $R/Themes/_shared/fonts/ 2> /dev/null ||
        for f in Nunito PressStart2P; do cp /usr/share/fonts/dejavu/DejaVuSans.ttf $R/Themes/_shared/fonts/$f.ttf; done
    cp /tmp/prova_libretro.so $R/Cores/
    head -c 4096 /dev/urandom > $R/Roms/PROVA/$1
    printf 'prova\tgioco\tGioco\t-\t0\t-\t0\t0\n' > $R/System/cambio.tsv   # come lo scrive il launcher
    sync
    rm -rf /tmp/run/* /tmp/azione /tmp/frames
    /tmp/guardia "${LOOP#/dev/}" "$LOOP" 60 & gpid=$!
    # uscita e errori nel registro di system-init sulla scheda, come sulla console (Logs/avvio.log)
    ( cd /w/frontend && exec ./build/runner --root $R --core $R/Cores/prova_libretro.so --rom $R/Roms/PROVA/$1 ) >> $R/Logs/avvio.log 2>&1 & rpid=$!
    aspetta 20 '[ "$(frames)" -gt 60 ]' || ko "il gioco non parte: $(tail -5 $R/Logs/avvio.log)"
}
togli() { sync; cp /tmp/sd.img /tmp/sd.bak; truncate -s 0 /tmp/sd.img; losetup -c "$LOOP"; echo 3 > /proc/sys/vm/drop_caches; }
rimetti() { cat /tmp/sd.bak > /tmp/sd.img; losetup -c "$LOOP"; }
fine() {
    kill $rpid $gpid 2> /dev/null; wait $rpid $gpid 2> /dev/null
    while umount /tmp/mnt 2> /dev/null; do :; done; losetup -d "$LOOP" 2> /dev/null; true
}

echo "--- gioco su cartuccia: scheda tolta e rimessa"
prepara gioco.bin
check "core bloccato in memoria ($(grep -o 'core in memoria.*' $R/Logs/Arcade.log | head -1))" 'grep -q "core in memoria: [1-9][0-9]* KB$" $R/Logs/Arcade.log'
togli
aspetta 5 '[ -f /tmp/run/scheda-tolta ]' || ko "guardia: scheda tolta non vista"
sleep 1; f1=$(frames); sleep 1.5; f2=$(frames)
check "scheda fuori: il gioco è fermo ($f1, poi $f2)" '[ "$f1" = "$f2" ]'
check "scheda fuori: il runner è vivo" 'kill -0 $rpid 2> /dev/null'
rimetti
aspetta 15 '[ ! -f /tmp/run/scheda-tolta ] && [ -f /tmp/run/scheda-rimessa ]' || ko "guardia: non rimontata ($(cat /tmp/azione 2> /dev/null))"
sleep 1; f3=$(frames); sleep 1; f4=$(frames)
check "scheda rimessa: il gioco riparte ($f2, poi $f3, $f4)" '[ "$f4" -gt "$f3" ] && [ "$f3" -ge "$f2" ]'
check "scheda rimessa: nessun riavvio" '[ ! -f /tmp/azione ]'
check "scheda rimessa: il runner è vivo" 'kill -0 $rpid 2> /dev/null'; grep -E "scheda|aperto|riapre|memoria" $R/Logs/Arcade.log | sed 's/^/     registro: /'
check "registro: il runner riprende a scrivere" 'grep -q "scheda rimessa e rimontata: si riprende" $R/Logs/Arcade.log'
aspetta 15 '[ -s $R/Saves/prova/gioco.srm ]'
check "salvataggio a batteria scritto sulla scheda rimessa" '[ "$(wc -c < $R/Saves/prova/gioco.srm 2> /dev/null)" = 64 ]'
fine

echo "--- gioco su disco (file tenuto aperto): si salva e si riapre dal launcher"
prepara gioco.cue
togli
aspetta 5 '[ -f /tmp/run/scheda-tolta ]'; sleep 1
rimetti
aspetta 15 '! kill -0 $rpid 2> /dev/null'
wait $rpid; rc=$?
check "il runner torna al launcher (uscita $rc)" '[ $rc = 0 ]'
check "il file del disco è stato visto aperto sulla scheda tolta" 'grep -q "resta aperto sulla scheda tolta" $R/Logs/Arcade.log'
check "cambio.txt chiede di riaprire lo stesso gioco" '[ "$(cat $R/System/cambio.txt 2> /dev/null)" = "$(printf "prova\tgioco")" ]'
check "stato automatico salvato sulla scheda rimessa" 'ls $R/Saves/prova/ 2> /dev/null | grep -q "^gioco\.auto"'
fine

echo "--- gioco Linux sotto gamewatch: fermo a scheda fuori, chiuso alla scheda rimessa"
gcc -std=c11 -O2 -D_DEFAULT_SOURCE -o /tmp/gw /trimui/frontend/src/tools/gamewatch.c || ko "gamewatch non compila"
rm -rf /tmp/run/* /tmp/tick /tmp/gioco-chiuso; echo "salvato prima" > /tmp/salvataggio
cat > /tmp/gioco.sh <<'X'
# in uscita il gioco salva: con la scheda appena rimessa il salvataggio potrebbe restare a metà
trap 'echo mezzo > /tmp/salvataggio; echo chiuso > /tmp/gioco-chiuso; exit 0' TERM
i=0; while :; do i=$((i + 1)); echo $i > /tmp/tick; sleep 0.1; done
X
/tmp/gw -- sh /tmp/gioco.sh 2> /tmp/gw.log & gw=$!
aspetta 5 '[ -s /tmp/tick ]'
echo "$(( $(cut -d. -f1 /proc/uptime) + 600 )) spegni" > /tmp/run/scheda-tolta   # la guardia ha visto la scheda tolta
sleep 1; t1=$(cat /tmp/tick); sleep 1; t2=$(cat /tmp/tick)
check "scheda fuori: il gioco è fermo ($t1, poi $t2)" '[ "$t1" = "$t2" ]'
check "scheda fuori: gamewatch aspetta" 'kill -0 $gw 2> /dev/null'
rm -f /tmp/run/scheda-tolta   # rimontata
aspetta 10 '! kill -0 $gw 2> /dev/null'
wait $gw; rc=$?
t3=$(cat /tmp/tick)
check "scheda rimessa: si torna al launcher (uscita $rc)" '[ $rc = 0 ]'
check "scheda rimessa: il gioco non riparte ($t2, poi $t3) e non scrive il salvataggio in uscita" '[ "$t3" = "$t2" ] && [ "$(cat /tmp/salvataggio)" = "salvato prima" ] && [ ! -f /tmp/gioco-chiuso ]'
check "scheda rimessa: del gioco non resta niente" '! pgrep -f gioco.sh > /dev/null'
check "segno per il launcher: gioco chiuso per la scheda" '[ -f /tmp/run/gioco-chiuso-scheda ]'

echo
[ $errors = 0 ] && echo "tutte le prove passate" || { echo "$errors prove fallite"; exit 1; }
