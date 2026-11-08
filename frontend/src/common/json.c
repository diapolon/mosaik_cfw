#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int depth; } jp;

#define MAX_DEPTH 64   /* oltre, il file non è valido: la ricorsione non deve esaurire la pila */

static void ws(jp *j) {
    for (;;) {
        while (isspace((unsigned char)*j->p)) j->p++;
        /* commenti // ammessi nei temi, comodi per chi li scrive a mano */
        if (j->p[0] == '/' && j->p[1] == '/') {
            while (*j->p && *j->p != '\n') j->p++;
            continue;
        }
        return;
    }
}

static jval *mk(jtype t) {
    jval *v = calloc(1, sizeof(jval));
    v->t = t;
    return v;
}

static void put_utf8(char **o, unsigned cp) {
    char *q = *o;
    if (cp < 0x80) *q++ = (char)cp;
    else if (cp < 0x800) { *q++ = (char)(0xC0 | (cp >> 6)); *q++ = (char)(0x80 | (cp & 0x3F)); }
    else { *q++ = (char)(0xE0 | (cp >> 12)); *q++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *q++ = (char)(0x80 | (cp & 0x3F)); }
    *o = q;
}

static char *pstring(jp *j) {
    if (*j->p != '"') return NULL;
    j->p++;
    const char *s = j->p;
    size_t cap = 0;
    while (*s && *s != '"') { if (*s == '\\' && s[1]) s++; s++; cap++; }
    char *out = malloc(cap * 3 + 1), *o = out;
    while (*j->p && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { *o++ = c; continue; }
        c = *j->p++;
        if (!c) { j->p--; break; }   /* barra subito prima della fine del testo */
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 0; i < 4 && isxdigit((unsigned char)*j->p); i++, j->p++)
                cp = cp * 16 + (unsigned)(isdigit((unsigned char)*j->p) ? *j->p - '0' : (tolower((unsigned char)*j->p) - 'a' + 10));
            put_utf8(&o, cp);
            break;
        }
        default: *o++ = c;
        }
    }
    *o = 0;
    if (*j->p == '"') j->p++;
    return out;
}

static jval *pvalue(jp *j);

static jval *pcontainer(jp *j, char close, jtype t) {
    jval *v = mk(t), **tail = &v->child;
    j->p++;
    for (;;) {
        ws(j);
        if (*j->p == close) { j->p++; return v; }
        char *key = NULL;
        if (t == J_OBJ) {
            key = pstring(j);
            if (!key) break;
            ws(j);
            if (*j->p != ':') { free(key); break; }
            j->p++;
        }
        jval *c = pvalue(j);
        if (!c) { free(key); break; }
        c->key = key;
        *tail = c;
        tail = &c->next;
        ws(j);
        if (*j->p == ',') j->p++;
    }
    json_free(v);
    return NULL;
}

static jval *pvalue(jp *j) {
    ws(j);
    char c = *j->p;
    if (c == '{' || c == '[') {
        if (j->depth >= MAX_DEPTH) return NULL;
        j->depth++;
        jval *v = c == '{' ? pcontainer(j, '}', J_OBJ) : pcontainer(j, ']', J_ARR);
        j->depth--;
        return v;
    }
    if (c == '"') { jval *v = mk(J_STR); v->s = pstring(j); return v; }
    if (!strncmp(j->p, "true", 4)) { j->p += 4; jval *v = mk(J_BOOL); v->n = 1; return v; }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return mk(J_BOOL); }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return mk(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end;
        jval *v = mk(J_NUM);
        v->n = strtod(j->p, &end);
        if (end == j->p) { free(v); return NULL; }   /* "-" da solo: senza avanzare il ciclo non finirebbe */
        j->p = end;
        return v;
    }
    return NULL;
}

jval *json_parse(const char *text) {
    jp j = { text };
    return text ? pvalue(&j) : NULL;
}

jval *json_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    jval *v = json_parse(buf);
    free(buf);
    return v;
}

void json_free(jval *v) {
    while (v) {
        jval *n = v->next;
        json_free(v->child);
        free(v->s);
        free(v->key);
        free(v);
        v = n;
    }
}

jval *json_get(jval *obj, const char *path) {
    char part[128];
    while (obj && path && *path) {
        const char *dot = strchr(path, '.');
        size_t len = dot ? (size_t)(dot - path) : strlen(path);
        if (len >= sizeof part) return NULL;
        memcpy(part, path, len);
        part[len] = 0;
        if (obj->t != J_OBJ) return NULL;
        jval *c = obj->child;
        while (c && strcmp(c->key, part)) c = c->next;
        obj = c;
        path = dot ? dot + 1 : NULL;
    }
    return obj;
}

double json_num(jval *obj, const char *path, double def) {
    jval *v = json_get(obj, path);
    return v && (v->t == J_NUM || v->t == J_BOOL) ? v->n : def;
}

const char *json_str(jval *obj, const char *path, const char *def) {
    jval *v = json_get(obj, path);
    return v && v->t == J_STR ? v->s : def;
}

bool json_bool(jval *obj, const char *path, bool def) {
    jval *v = json_get(obj, path);
    return v && (v->t == J_BOOL || v->t == J_NUM) ? v->n != 0 : def;
}
