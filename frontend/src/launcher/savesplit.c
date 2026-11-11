/* Salvataggi delle console separati per console (issue #5).
 *
 * Fino alla 0.9.17 i salvataggi stavano in Saves/<core>/<rom>.<tipo>: Mega Drive, Master
 * System e Game Gear (Genesis Plus GX), Game Boy e Game Boy Color (Gambatte) scrivevano nella
 * stessa cartella, e due giochi con lo stesso nome file su due console condividevano .srm,
 * stati e salvataggio automatico. Ora ogni console ha Saves/<id della console>/.
 *
 * All'avvio del launcher i file vecchi di ogni gioco per console si copiano nella cartella
 * della sua console (con la scrittura sicura: file provvisorio, fsync, rename); se due console
 * hanno un gioco con lo stesso nome ricevono una copia ciascuna, perché il file era di
 * entrambe. Il file vecchio si toglie solo quando tutte le copie sono riuscite. Si spostano solo
 * i file con un'estensione dei salvataggi (sotto), così un gioco "A" non prende i file di "A.B";
 * quelli di giochi che non sono più sulla scheda restano dove sono. */
#include "savesplit.h"
#include "../common/durable.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void ui_log(const char *fmt, ...);

/* i file di un gioco che scrivono runner e core: <rom>.<estensione> */
static const char *const EXTS[] = {
    "srm", "rtc", "flash", "cfg", "opt",
    "state", "2.state", "3.state", "auto.state", "auto.state.prev", "auto.state.rot",
    "state.png", "2.state.png", "3.state.png", "auto.state.png", "auto.state.prev.png",
};
#define NEXTS (int)(sizeof EXTS / sizeof EXTS[0])

static bool exists(const char *p) { struct stat s; return stat(p, &s) == 0; }

/* copia sicura: o il file intero o niente */
static bool copy_file(const char *src, const char *dst) {
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    int in = open(src, O_RDONLY);
    if (in < 0) return false;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return false; }
    char buf[65536];
    bool ok = true;
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) { ok = false; break; }
    if (r < 0) ok = false;
    if (fsync(out) != 0) ok = false;
    if (close(out) != 0) ok = false;
    close(in);
    if (!ok || durable_replace(tmp, dst) != 0) { unlink(tmp); return false; }
    return true;
}

/* nome della cartella del core di un emulatore: fceumm_libretro -> fceumm */
static void core_dir_name(const emulator_t *e, char *out, size_t n) {
    snprintf(out, n, "%s", e->core);
    char *us = strstr(out, "_libretro");
    if (us) *us = 0;
}

int saves_split(const char *root, const game_t *games, int ngames) {
    int moved = 0, failed = 0;
    for (int e = 0; e < NEMUS; e++) {
        const emulator_t *emu = &EMUS[e];
        if (!emu->console) continue;
        char cname[64], old_dir[1100];
        core_dir_name(emu, cname, sizeof cname);
        if (!strcmp(cname, emu->id)) continue;   /* stessa cartella: niente da fare */
        snprintf(old_dir, sizeof old_dir, "%s/Saves/%s", root, cname);
        if (!exists(old_dir)) continue;
        /* ogni file vecchio di un gioco di questo emulatore */
        for (int i = 0; i < ngames; i++) {
            if (strcmp(games[i].core, emu->id) || games[i].missing) continue;
            for (int x = 0; x < NEXTS; x++) {
                char src[1400], dst[1400], new_dir[1100];
                snprintf(src, sizeof src, "%s/%s.%s", old_dir, games[i].name, EXTS[x]);
                if (!exists(src)) continue;
                snprintf(new_dir, sizeof new_dir, "%s/Saves/%s", root, emu->id);
                mkdir(new_dir, 0755);
                /* ogni console con un gioco di questo nome e lo stesso core riceve la sua copia */
                bool all = true;
                for (int f = 0; f < NEMUS; f++) {
                    const emulator_t *o = &EMUS[f];
                    char oc[64];
                    if (!o->console) continue;
                    core_dir_name(o, oc, sizeof oc);
                    if (strcmp(oc, cname)) continue;
                    bool has = false;
                    for (int k = 0; k < ngames && !has; k++)
                        has = !strcmp(games[k].core, o->id) && !games[k].missing && !strcmp(games[k].name, games[i].name);
                    if (!has) continue;
                    char od[1100];
                    snprintf(od, sizeof od, "%s/Saves/%s", root, o->id);
                    mkdir(od, 0755);
                    snprintf(dst, sizeof dst, "%s/%s.%s", od, games[i].name, EXTS[x]);
                    if (exists(dst)) continue;   /* la console ha già il suo: non si tocca */
                    if (copy_file(src, dst)) ui_log("salvataggi: %s.%s -> Saves/%s", games[i].name, EXTS[x], o->id);
                    else { all = false; ui_log("salvataggi: copia di %s non riuscita (%s)", src, strerror(errno)); }
                }
                if (all && unlink(src) == 0) moved++;
                else failed++;
            }
        }
        rmdir(old_dir);   /* solo se è rimasta vuota */
    }
    if (moved || failed) {
        sync();
        ui_log("salvataggi separati per console: %d file spostati, %d rimasti al loro posto", moved, failed);
    }
    return failed;
}
