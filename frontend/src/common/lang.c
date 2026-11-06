#include "lang.h"
#include "json.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static jval *cur, *fallback;

bool lang_load(const char *root, const char *code) {
    char p[1100];
    json_free(cur);
    json_free(fallback);
    snprintf(p, sizeof p, "%s/Lang/%s.json", root, code);
    cur = json_load(p);
    snprintf(p, sizeof p, "%s/Lang/en.json", root);
    fallback = strcmp(code, "en") ? json_load(p) : NULL;
    return cur != NULL;
}

const char *T_opt(const char *key) {
    const char *s = json_str(cur, key, NULL);
    return s ? s : json_str(fallback, key, NULL);
}

const char *T(const char *key) {
    const char *s = T_opt(key);
    return s ? s : key;
}

int lang_list(const char *root, char codes[][8], char names[][32], int max) {
    char dirp[1100];
    snprintf(dirp, sizeof dirp, "%s/Lang", root);
    DIR *d = opendir(dirp);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        /* i file che iniziano con un punto sono nascosti (anche i ._ di macOS) */
        if (e->d_name[0] == '.' || len < 6 || len > 12 || strcmp(e->d_name + len - 5, ".json")) continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dirp, e->d_name);
        jval *j = json_load(p);
        if (!j) continue;
        snprintf(codes[n], 8, "%.*s", (int)(len - 5), e->d_name);
        snprintf(names[n], 32, "%s", json_str(j, "_name", codes[n]));
        json_free(j);
        n++;
    }
    closedir(d);
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (strcasecmp(names[a], names[b]) > 0) {
                char tc[8], tn[32];
                memcpy(tc, codes[a], 8); memcpy(codes[a], codes[b], 8); memcpy(codes[b], tc, 8);
                memcpy(tn, names[a], 32); memcpy(names[a], names[b], 32); memcpy(names[b], tn, 32);
            }
    return n;
}
