/* Salvataggi delle console separati per console (issue #5).
 *
 * Fino alla 0.9.17 i salvataggi stavano in Saves/<core>/<rom>.<tipo>: Mega Drive, Master
 * System e Game Gear (Genesis Plus GX), Game Boy e Game Boy Color (Gambatte) scrivevano nella
 * stessa cartella, e due giochi con lo stesso nome file su due console condividevano .srm,
 * stati e salvataggio automatico. Ora ogni console ha Saves/<id della console>/.
 *
 * All'avvio del launcher i file vecchi di ogni gioco per console si copiano nella cartella
 * della sua console (scrittura sicura: file provvisorio, fsync, rename); se due console hanno
 * un gioco con lo stesso nome ricevono una copia ciascuna, perché il file era di entrambe.
 * Gli originali restano dove sono (si può tornare alla versione di prima con i salvataggi) e
 * Saves/<core>/.spostati-per-console.txt registra ogni copia ("<console><TAB><file>"): una
 * copia già fatta non si rifà, così un salvataggio tolto con "Reimposta il gioco" non torna.
 * Se la console ha già un file con quel nome e contenuto diverso non si sovrascrive nulla: il
 * conflitto si registra (anche nel log) e restano tutti e due. Si copiano solo i file con
 * un'estensione dei salvataggi (sotto), così un gioco "A" non prende i file di "A.B". Audit
 * prelancio 0.9.17, R01. */
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

/* il registro delle copie di una cartella del core, in memoria */
typedef struct { char **line; int n, cap; FILE *out; } journal_t;

static void journal_open(journal_t *j, const char *old_dir) {
    char p[1200], line[600];
    memset(j, 0, sizeof *j);
    snprintf(p, sizeof p, "%s/.spostati-per-console.txt", old_dir);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (j->n == j->cap) j->line = realloc(j->line, sizeof(char *) * (size_t)(j->cap = j->cap ? j->cap * 2 : 64));
        j->line[j->n++] = strdup(line);
    }
    if (f) fclose(f);
    j->out = fopen(p, "a");
}

static bool journal_has(const journal_t *j, const char *console, const char *file) {
    char k[600];
    snprintf(k, sizeof k, "%s\t%s", console, file);
    for (int i = 0; i < j->n; i++)
        if (!strncmp(j->line[i], k, strlen(k)) && (j->line[i][strlen(k)] == 0 || j->line[i][strlen(k)] == '\t')) return true;
    return false;
}

static void journal_add(journal_t *j, const char *console, const char *file, const char *note) {
    if (!j->out) return;
    fprintf(j->out, "%s\t%s%s%s\n", console, file, note ? "\t" : "", note ? note : "");
    fflush(j->out);
    fsync(fileno(j->out));
}

static void journal_close(journal_t *j) {
    for (int i = 0; i < j->n; i++) free(j->line[i]);
    free(j->line);
    if (j->out) fclose(j->out);
}

static bool same_content(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    bool same = x && y;
    while (same) {
        int c1 = fgetc(x), c2 = fgetc(y);
        if (c1 != c2) same = false;
        if (c1 == EOF || c2 == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

int saves_split(const char *root, const game_t *games, int ngames) {
    int copied = 0, failed = 0, conflicts = 0;
    for (int e = 0; e < NEMUS; e++) {
        const emulator_t *emu = &EMUS[e];
        if (!emu->console) continue;
        char cname[64], old_dir[1100];
        core_dir_name(emu, cname, sizeof cname);
        if (!strcmp(cname, emu->id)) continue;   /* stessa cartella: niente da fare */
        snprintf(old_dir, sizeof old_dir, "%s/Saves/%s", root, cname);
        if (!exists(old_dir)) continue;
        journal_t j;
        bool opened = false;
        for (int i = 0; i < ngames; i++) {
            if (strcmp(games[i].core, emu->id) || games[i].missing) continue;
            /* i file vecchi hanno il nome della ROM: sono del gioco che tiene il nome come
               identità, non degli omonimi con "nome [sottocartella]" (R04) */
            if (games[i].key[0] && strcmp(games[i].key, games[i].name)) continue;
            for (int x = 0; x < NEXTS; x++) {
                char file[600], src[1400], dst[1400], new_dir[1100];
                snprintf(file, sizeof file, "%s.%s", games[i].name, EXTS[x]);
                snprintf(src, sizeof src, "%s/%s", old_dir, file);
                if (!exists(src)) continue;
                if (!opened) { journal_open(&j, old_dir); opened = true; }
                if (journal_has(&j, emu->id, file)) continue;   /* già fatto in un avvio prima */
                snprintf(new_dir, sizeof new_dir, "%s/Saves/%s", root, emu->id);
                mkdir(new_dir, 0755);
                snprintf(dst, sizeof dst, "%s/%s", new_dir, file);
                if (exists(dst)) {
                    /* la console ha già il suo: uguale, niente da fare; diverso, conflitto */
                    if (same_content(src, dst)) journal_add(&j, emu->id, file, "uguale");
                    else {
                        conflicts++;
                        journal_add(&j, emu->id, file, "conflitto: resta quello della console");
                        ui_log("salvataggi: conflitto per %s, Saves/%s ha già un file diverso: restano tutti e due", file, emu->id);
                    }
                    continue;
                }
                if (copy_file(src, dst)) {
                    copied++;
                    journal_add(&j, emu->id, file, NULL);
                    ui_log("salvataggi: %s -> Saves/%s", file, emu->id);
                } else {
                    failed++;   /* non registrato: si riprova al prossimo avvio */
                    ui_log("salvataggi: copia di %s non riuscita (%s)", src, strerror(errno));
                }
            }
        }
        if (opened) journal_close(&j);
    }
    if (copied || failed || conflicts) {
        sync();
        ui_log("salvataggi separati per console: %d file copiati, %d conflitti, %d non riusciti (gli originali restano in Saves/<core>)", copied, conflicts, failed);
    }
    return failed;
}
