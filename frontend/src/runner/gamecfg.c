/* Impostazioni a strati del runner (gamecfg.h). Niente SDL: lo provano m2/test-gamecfg.sh e il runner. */
#include "gamecfg.h"
#include "../common/durable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void gc_clear(gc_layer_t *l) { memset(l, 0, sizeof *l); }

bool gc_empty(const gc_layer_t *l) {
    if (l->has_aspect || l->has_filter || l->has_bezel || l->has_cpu || l->nsp || l->rotate || l->slot) return false;
    for (int i = 0; i < 16; i++)
        if (l->has_map[i]) return false;
    return true;
}

int gc_sp_find(const gc_layer_t *l, const char *name) {
    for (int i = 0; i < l->nsp; i++)
        if (!strcmp(l->sp_name[i], name)) return i;
    return -1;
}

void gc_sp_set(gc_layer_t *l, const char *name, float v) {
    int i = gc_sp_find(l, name);
    if (i < 0) {
        if (l->nsp == GC_SP || strlen(name) >= sizeof l->sp_name[0]) return;
        i = l->nsp++;
        snprintf(l->sp_name[i], sizeof l->sp_name[0], "%s", name);
    }
    l->sp_val[i] = v;
}

void gc_sp_del(gc_layer_t *l, const char *name) {
    int i = gc_sp_find(l, name);
    if (i < 0) return;
    l->nsp--;
    memmove(l->sp_name[i], l->sp_name[i + 1], sizeof l->sp_name[0] * (size_t)(l->nsp - i));
    memmove(&l->sp_val[i], &l->sp_val[i + 1], sizeof l->sp_val[0] * (size_t)(l->nsp - i));
}

bool gc_read(const char *path, gc_layer_t *l, bool *v2, const char *(*legacy_filter)(int)) {
    gc_clear(l);
    *v2 = false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        int id, v;
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "versione=2")) *v2 = true;
        else if (sscanf(line, "aspect=%d", &v) == 1) { if (v >= 0 && v < 3) { l->aspect = v; l->has_aspect = true; } }
        else if (!strncmp(line, "filter=", 7)) {
            char fid[32];
            if (sscanf(line + 7, "%31s", fid) == 1) {
                const char *use = fid[0] >= '0' && fid[0] <= '9' && legacy_filter ? legacy_filter(atoi(fid)) : fid;
                snprintf(l->filter, sizeof l->filter, "%s", use ? use : "");
                l->has_filter = l->filter[0] != 0;
            }
        }
        else if (sscanf(line, "bezel=%d", &v) == 1) { l->bezel = v != 0; l->has_bezel = true; }
        else if (sscanf(line, "cpu=%d", &v) == 1) { if (v >= 0 && v <= 4) { l->cpu = v; l->has_cpu = true; } }
        else if (sscanf(line, "rotate=%d", &v) == 1) l->rotate = v & 3;
        else if (sscanf(line, "slot=%d", &v) == 1) l->slot = v >= 0 && v <= 2 ? v : 0;
        else if (sscanf(line, "map.%d=%d", &id, &v) == 2 && id >= 0 && id < 16 && v >= -1 && v < 64) { l->map[id] = v; l->has_map[id] = true; }
        else if (!strncmp(line, "sp.", 3)) {
            char *eq = strchr(line, '=');
            if (eq && eq - line - 3 > 0 && eq - line - 3 < 48) {
                char name[48];
                snprintf(name, sizeof name, "%.*s", (int)(eq - line - 3), line + 3);
                gc_sp_set(l, name, (float)atof(eq + 1));
            }
        }
    }
    fclose(f);
    return true;
}

void gc_migrate(gc_layer_t *l, const gc_vals_t *in) {
    if (l->has_aspect && l->aspect == in->aspect) l->has_aspect = false;
    if (l->has_filter && !strcmp(l->filter, in->filter)) l->has_filter = false;
    if (l->has_bezel && l->bezel == in->bezel) l->has_bezel = false;
    if (l->has_cpu && l->cpu == in->cpu) l->has_cpu = false;
    for (int i = 0; i < 16; i++)
        if (l->has_map[i] && l->map[i] == in->map[i]) l->has_map[i] = false;
}

bool gc_write(const char *path, const gc_layer_t *l) {
    if (gc_empty(l)) return remove(path) == 0 || access(path, F_OK) != 0;
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "versione=2\n");
    if (l->has_aspect) fprintf(f, "aspect=%d\n", l->aspect);
    if (l->has_filter) fprintf(f, "filter=%s\n", l->filter);
    if (l->has_bezel) fprintf(f, "bezel=%d\n", l->bezel);
    if (l->has_cpu) fprintf(f, "cpu=%d\n", l->cpu);
    if (l->rotate) fprintf(f, "rotate=%d\n", l->rotate);
    if (l->slot) fprintf(f, "slot=%d\n", l->slot);
    for (int i = 0; i < 16; i++)
        if (l->has_map[i]) fprintf(f, "map.%d=%d\n", i, l->map[i]);
    for (int i = 0; i < l->nsp; i++) fprintf(f, "sp.%s=%g\n", l->sp_name[i], l->sp_val[i]);
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok && durable_replace(tmp, path) == 0) return true;
    remove(tmp);
    return false;
}

void gc_apply(gc_vals_t *v, const gc_layer_t *l) {
    if (!l) return;
    if (l->has_aspect) v->aspect = l->aspect;
    if (l->has_filter) snprintf(v->filter, sizeof v->filter, "%s", l->filter);
    if (l->has_bezel) v->bezel = l->bezel;
    if (l->has_cpu) v->cpu = l->cpu;
    for (int i = 0; i < 16; i++)
        if (l->has_map[i]) v->map[i] = l->map[i];
}
