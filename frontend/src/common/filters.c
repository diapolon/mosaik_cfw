#include "filters.h"
#include "json.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *filter_legacy_id(int n) {
    static const char *ids[] = { "nitido", "morbido", "scanline" };
    return ids[n >= 0 && n < 3 ? n : 0];
}

bool filter_load(const char *root, const char *lang, const char *id, filter_t *f) {
    char p[1200];
    memset(f, 0, sizeof *f);
    snprintf(f->id, sizeof f->id, "%s", id);
    snprintf(p, sizeof p, "%s/Filters/%s/filter.json", root, id);
    jval *j = json_load(p);
    if (!j) return false;
    jval *name = json_get(j, "name");
    const char *n = NULL;
    if (name && name->t == J_STR) n = name->s;
    else if (name && name->t == J_OBJ) {
        n = json_str(name, lang, NULL);
        if (!n) n = json_str(name, "en", NULL);
        if (!n && name->child && name->child->t == J_STR) n = name->child->s;
    }
    snprintf(f->name, sizeof f->name, "%s", n ? n : id);
    f->order = (int)json_num(j, "order", 100);
    f->smooth = json_bool(j, "smooth", false);
    const char *mask = json_str(j, "mask", NULL);
    if (mask) snprintf(f->mask, sizeof f->mask, "%s/Filters/%s/%s", root, id, mask);
    const char *shader = json_str(j, "shader", NULL);
    if (shader) snprintf(f->shader, sizeof f->shader, "%s/Filters/%s/%s", root, id, shader);
    f->mask_per_pixel = strcmp(json_str(j, "mask_scale", "pixel"), "screen") != 0;
    f->strength = (float)json_num(j, "mask_strength", 0.5);
    f->vignette = (float)json_num(j, "vignette", 0);
    json_free(j);
    return true;
}

int filters_list(const char *root, const char *lang, filter_t *out, int max) {
    char dirp[1100];
    snprintf(dirp, sizeof dirp, "%s/Filters", root);
    DIR *d = opendir(dirp);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof out[0].id) continue;
        if (filter_load(root, lang, e->d_name, &out[n])) n++;
    }
    closedir(d);
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (out[b].order < out[a].order || (out[b].order == out[a].order && strcasecmp(out[b].name, out[a].name) < 0)) {
                filter_t t = out[a];
                out[a] = out[b];
                out[b] = t;
            }
    return n;
}
