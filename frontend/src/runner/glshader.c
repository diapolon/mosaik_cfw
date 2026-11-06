#include "glshader.h"
#include "../common/ui.h"
#include <SDL_image.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Le funzioni OpenGL si caricano con SDL_GL_GetProcAddress: lo stesso codice va con
   OpenGL 2.1 (Mac) e OpenGL ES 2 (console) senza dipendere dalle intestazioni. */
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_ARRAY_BUFFER 0x8892
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000

#define GL_FUNCS \
    GLF(GLuint, glCreateShader, (GLenum)) \
    GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    GLF(void, glCompileShader, (GLuint)) \
    GLF(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    GLF(void, glDeleteShader, (GLuint)) \
    GLF(GLuint, glCreateProgram, (void)) \
    GLF(void, glAttachShader, (GLuint, GLuint)) \
    GLF(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    GLF(void, glLinkProgram, (GLuint)) \
    GLF(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    GLF(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    GLF(void, glDeleteProgram, (GLuint)) \
    GLF(void, glUseProgram, (GLuint)) \
    GLF(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    GLF(void, glUniform1i, (GLint, GLint)) \
    GLF(void, glUniform1f, (GLint, GLfloat)) \
    GLF(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    GLF(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    GLF(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    GLF(void, glEnableVertexAttribArray, (GLuint)) \
    GLF(void, glDisableVertexAttribArray, (GLuint)) \
    GLF(void, glGetVertexAttribiv, (GLuint, GLenum, GLint *)) \
    GLF(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    GLF(void, glGetIntegerv, (GLenum, GLint *)) \
    GLF(void, glActiveTexture, (GLenum)) \
    GLF(void, glBindTexture, (GLenum, GLuint)) \
    GLF(void, glGenTextures, (GLsizei, GLuint *)) \
    GLF(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    GLF(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    GLF(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    GLF(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    GLF(void, glBindFramebuffer, (GLenum, GLuint)) \
    GLF(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    GLF(GLenum, glCheckFramebufferStatus, (GLenum)) \
    GLF(void, glBindBuffer, (GLenum, GLuint)) \
    GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    GLF(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    GLF(void, glClear, (GLbitfield)) \
    GLF(GLboolean, glIsEnabled, (GLenum)) \
    GLF(void, glEnable, (GLenum)) \
    GLF(void, glDisable, (GLenum))

#define GLF(ret, name, args) static ret (*name) args;
GL_FUNCS
#undef GLF

enum { MAX_PASSES = 8, MAX_PARAMS = 64, MAX_LUTS = 6 };
enum { SCALE_DEFAULT, SCALE_SOURCE, SCALE_VIEWPORT, SCALE_ABSOLUTE };

typedef struct {
    GLuint prog;
    GLint u_mvp, u_dir, u_count, u_out, u_tex_size, u_in, u_tex;
    GLint u_orig, u_orig_size, u_orig_in;
    GLint u_pass[MAX_PASSES], u_pass_size[MAX_PASSES], u_pass_in[MAX_PASSES];   /* uscite dei passaggi precedenti */
    GLint u_lut[MAX_LUTS];
    GLint u_param[MAX_PARAMS];
    int stype_x, stype_y, fc_mod, linear;   /* linear: -1 = non indicato */
    float sx, sy;
    char alias[32];
    GLuint fbo, tex;                        /* uscita (non per l'ultimo passaggio) */
    int w, h;
} pass_t;

typedef struct { char name[48]; GLuint tex; } lut_t;

static int available = -1;
static bool gles;
static pass_t passes[MAX_PASSES];
static int npasses;
static glsh_param_t params[MAX_PARAMS];
static int nparams;
static lut_t luts[MAX_LUTS];
static int nluts;

bool glsh_available(void) {
    if (available >= 0) return available;
    SDL_RendererInfo info;
    available = 0;
    if (SDL_GetRendererInfo(UI.ren, &info) != 0) return false;
    gles = !strcmp(info.name, "opengles2");
    if (strcmp(info.name, "opengl") && !gles) {
        ui_log("shader non disponibili con il renderer %s", info.name);
        return false;
    }
#define GLF(ret, name, args) if (!(*(void **)&name = SDL_GL_GetProcAddress(#name))) { ui_log("OpenGL: manca %s", #name); return false; }
    GL_FUNCS
#undef GLF
    available = 1;
    ui_log("shader disponibili (renderer %s)", info.name);
    return true;
}

/* ---------------------------------------------------------- parametri */

int glsh_param_count(void) { return nparams; }
glsh_param_t *glsh_param(int i) { return i >= 0 && i < nparams ? &params[i] : NULL; }

glsh_param_t *glsh_param_find(const char *name) {
    for (int i = 0; i < nparams; i++)
        if (!strcmp(params[i].name, name)) return &params[i];
    return NULL;
}

/* #pragma parameter NOME "Descrizione" predefinito minimo massimo [passo] */
static void collect_params(const char *src) {
    for (const char *p = strstr(src, "#pragma parameter"); p; p = strstr(p + 1, "#pragma parameter")) {
        glsh_param_t q = { 0 };
        q.step = 0;
        int n = sscanf(p, "#pragma parameter %47s \"%95[^\"]\" %f %f %f %f", q.name, q.desc, &q.def, &q.min, &q.max, &q.step);
        if (n < 5 || glsh_param_find(q.name) || nparams == MAX_PARAMS) continue;
        if (n < 6 || q.step <= 0) q.step = (q.max - q.min) / 20;
        q.value = q.def;
        params[nparams++] = q;
    }
}

/* ------------------------------------------------------- compilazione */

static GLuint compile(GLenum type, const char *src, const char *path) {
    /* intestazione come la prepara RetroArch: versione, sezione, parametri come uniform */
    const char *head = gles ? "#version 100\n" : "#version 120\n";
    const char *stage = type == GL_VERTEX_SHADER ? "#define VERTEX\n#define PARAMETER_UNIFORM\n" : "#define FRAGMENT\n#define PARAMETER_UNIFORM\n";
    const char *parts[3] = { head, stage, src };
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 3, parts, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        ui_log("shader %s (%s): %s", path, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    n = (long)fread(s, 1, (size_t)n, f);
    s[n] = 0;
    fclose(f);
    return s;
}

static GLint uniform_any(GLuint prog, const char *a, const char *b) {
    GLint u = glGetUniformLocation(prog, a);
    return u >= 0 || !b ? u : glGetUniformLocation(prog, b);
}

static bool build_pass(int i, const char *path) {
    pass_t *ps = &passes[i];
    char *src = read_file(path);
    if (!src) { ui_log("shader non trovato: %s", path); return false; }
    collect_params(src);
    GLuint vs = compile(GL_VERTEX_SHADER, src, path), fs = compile(GL_FRAGMENT_SHADER, src, path);
    free(src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    ps->prog = glCreateProgram();
    glAttachShader(ps->prog, vs);
    glAttachShader(ps->prog, fs);
    glBindAttribLocation(ps->prog, 0, "VertexCoord");
    glBindAttribLocation(ps->prog, 1, "TexCoord");
    glBindAttribLocation(ps->prog, 2, "COLOR");
    glLinkProgram(ps->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(ps->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(ps->prog, sizeof log, NULL, log);
        ui_log("shader %s: collegamento fallito: %s", path, log);
        glDeleteProgram(ps->prog);
        ps->prog = 0;
        return false;
    }
    GLuint p = ps->prog;
    ps->u_mvp = glGetUniformLocation(p, "MVPMatrix");
    ps->u_dir = glGetUniformLocation(p, "FrameDirection");
    ps->u_count = glGetUniformLocation(p, "FrameCount");
    ps->u_out = glGetUniformLocation(p, "OutputSize");
    ps->u_tex_size = glGetUniformLocation(p, "TextureSize");
    ps->u_in = glGetUniformLocation(p, "InputSize");
    ps->u_tex = glGetUniformLocation(p, "Texture");
    ps->u_orig = glGetUniformLocation(p, "OrigTexture");
    ps->u_orig_size = glGetUniformLocation(p, "OrigTextureSize");
    ps->u_orig_in = glGetUniformLocation(p, "OrigInputSize");
    /* uscite dei passaggi precedenti: per alias, come PassPrevN o come PassN */
    for (int j = 0; j < MAX_PASSES; j++) ps->u_pass[j] = ps->u_pass_size[j] = ps->u_pass_in[j] = -1;
    for (int j = 0; j + 1 < i; j++) {
        char a[64], b[64];
        if (passes[j].alias[0]) snprintf(a, sizeof a, "%sTexture", passes[j].alias);
        else snprintf(a, sizeof a, "PassPrev%dTexture", i - j);
        snprintf(b, sizeof b, "Pass%dTexture", j + 1);
        ps->u_pass[j] = uniform_any(p, a, b);
        if (passes[j].alias[0]) snprintf(a, sizeof a, "%sTextureSize", passes[j].alias);
        else snprintf(a, sizeof a, "PassPrev%dTextureSize", i - j);
        ps->u_pass_size[j] = glGetUniformLocation(p, a);
        if (passes[j].alias[0]) snprintf(a, sizeof a, "%sInputSize", passes[j].alias);
        else snprintf(a, sizeof a, "PassPrev%dInputSize", i - j);
        ps->u_pass_in[j] = glGetUniformLocation(p, a);
    }
    for (int k = 0; k < nluts; k++) ps->u_lut[k] = glGetUniformLocation(p, luts[k].name);
    for (int k = 0; k < MAX_PARAMS; k++) ps->u_param[k] = -1;
    return true;
}

/* ------------------------------------------------------------- preset */

typedef struct { char key[64], value[512]; } kv_t;

static int parse_preset(const char *text, kv_t *kv, int max) {
    int n = 0;
    const char *line = text;
    while (line && *line && n < max) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[640];
        snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
        line = nl ? nl + 1 : NULL;
        bool quoted = false;
        for (char *c = buf; *c; c++) {
            if (*c == '"') quoted = !quoted;
            else if (*c == '#' && !quoted) { *c = 0; break; }
        }
        char *eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = buf, *v = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); ) *--e = 0;
        while (isspace((unsigned char)*v) || *v == '"') v++;
        for (char *e = v + strlen(v); e > v && (isspace((unsigned char)e[-1]) || e[-1] == '"'); ) *--e = 0;
        snprintf(kv[n].key, sizeof kv[n].key, "%s", k);
        snprintf(kv[n].value, sizeof kv[n].value, "%s", v);
        n++;
    }
    return n;
}

static const char *kv_get(kv_t *kv, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (!strcmp(kv[i].key, key)) return kv[i].value;
    return NULL;
}

static void path_join(char *out, size_t n, const char *base_file, const char *rel) {
    if (rel[0] == '/') { snprintf(out, n, "%s", rel); return; }
    const char *slash = strrchr(base_file, '/');
    snprintf(out, n, "%.*s/%s", slash ? (int)(slash - base_file) : 1, slash ? base_file : ".", rel);
}

static int scale_type(const char *s) {
    if (!s) return SCALE_DEFAULT;
    if (!strcmp(s, "viewport")) return SCALE_VIEWPORT;
    if (!strcmp(s, "absolute")) return SCALE_ABSOLUTE;
    return SCALE_SOURCE;
}

static bool load_lut(lut_t *l, const char *path, bool linear) {
    SDL_Surface *s0 = IMG_Load(path);
    if (!s0) { ui_log("texture dello shader non trovata: %s", path); return false; }
    SDL_Surface *s = SDL_ConvertSurfaceFormat(s0, SDL_PIXELFORMAT_ABGR8888, 0);   /* RGBA in memoria */
    SDL_FreeSurface(s0);
    if (!s) return false;
    glGenTextures(1, &l->tex);
    glBindTexture(GL_TEXTURE_2D, l->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(s);
    return true;
}

bool glsh_load(const char *path) {
    glsh_unload();
    if (!glsh_available()) return false;
    /* SDL tiene una copia dello stato: programma e texture vanno rimessi come prima */
    GLint saved_prog, saved_tex, saved_active;
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex);
    bool ok = true;
    size_t len = strlen(path);
    if (len > 6 && !strcasecmp(path + len - 6, ".glslp")) {
        char *text = read_file(path);
        if (!text) { ui_log("preset non trovato: %s", path); ok = false; }
        static kv_t kv[256];
        int n = text ? parse_preset(text, kv, 256) : 0;
        free(text);
        const char *ns = kv_get(kv, n, "shaders");
        npasses = ns ? atoi(ns) : 0;
        if (ok && (npasses < 1 || npasses > MAX_PASSES)) { ui_log("preset %s: numero di passaggi non valido", path); ok = false; }
        /* texture aggiuntive (LUT): textures = "NOME1;NOME2", NOME1 = file, NOME1_linear = true */
        const char *tx = kv_get(kv, n, "textures");
        char list[512];
        snprintf(list, sizeof list, "%s", tx ? tx : "");
        char *save = NULL;
        for (char *name = strtok_r(list, ";", &save); ok && name && nluts < MAX_LUTS; name = strtok_r(NULL, ";", &save)) {
            const char *file = kv_get(kv, n, name);
            char key[96], full[1200];
            snprintf(key, sizeof key, "%s_linear", name);
            const char *lin = kv_get(kv, n, key);
            if (!file) continue;
            path_join(full, sizeof full, path, file);
            lut_t *l = &luts[nluts];
            snprintf(l->name, sizeof l->name, "%s", name);
            if (load_lut(l, full, lin && !strcmp(lin, "true"))) nluts++;
        }
        for (int i = 0; ok && i < npasses; i++) {
            char key[48], full[1200];
            pass_t *ps = &passes[i];
            memset(ps, 0, sizeof *ps);
            snprintf(key, sizeof key, "shader%d", i);
            const char *file = kv_get(kv, n, key);
            if (!file) { ui_log("preset %s: manca %s", path, key); ok = false; break; }
            snprintf(key, sizeof key, "alias%d", i);
            snprintf(ps->alias, sizeof ps->alias, "%s", kv_get(kv, n, key) ? kv_get(kv, n, key) : "");
            snprintf(key, sizeof key, "scale_type%d", i);
            const char *st = kv_get(kv, n, key);
            snprintf(key, sizeof key, "scale_type_x%d", i);
            ps->stype_x = scale_type(kv_get(kv, n, key) ? kv_get(kv, n, key) : st);
            snprintf(key, sizeof key, "scale_type_y%d", i);
            ps->stype_y = scale_type(kv_get(kv, n, key) ? kv_get(kv, n, key) : st);
            snprintf(key, sizeof key, "scale%d", i);
            const char *sc = kv_get(kv, n, key);
            snprintf(key, sizeof key, "scale_x%d", i);
            ps->sx = kv_get(kv, n, key) ? (float)atof(kv_get(kv, n, key)) : sc ? (float)atof(sc) : 1;
            snprintf(key, sizeof key, "scale_y%d", i);
            ps->sy = kv_get(kv, n, key) ? (float)atof(kv_get(kv, n, key)) : sc ? (float)atof(sc) : 1;
            snprintf(key, sizeof key, "filter_linear%d", i);
            const char *fl = kv_get(kv, n, key);
            ps->linear = fl ? !strcmp(fl, "true") : -1;
            snprintf(key, sizeof key, "frame_count_mod%d", i);
            ps->fc_mod = kv_get(kv, n, key) ? atoi(kv_get(kv, n, key)) : 0;
            path_join(full, sizeof full, path, file);
            ok = build_pass(i, full);
        }
        /* valori dei parametri indicati dal preset */
        for (int i = 0; ok && i < nparams; i++) {
            const char *v = kv_get(kv, n, params[i].name);
            if (v) params[i].value = params[i].def = (float)atof(v);
        }
    } else {
        npasses = 1;
        memset(&passes[0], 0, sizeof passes[0]);
        passes[0].linear = -1;
        ok = build_pass(0, path);
    }
    for (int i = 0; ok && i < npasses; i++)
        for (int k = 0; k < nparams; k++) passes[i].u_param[k] = glGetUniformLocation(passes[i].prog, params[k].name);
    glUseProgram((GLuint)saved_prog);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved_tex);
    glActiveTexture((GLenum)saved_active);
    if (!ok) { glsh_unload(); return false; }
    ui_log("shader caricato: %s (%d passaggi, %d parametri)", path, npasses, nparams);
    return true;
}

void glsh_unload(void) {
    for (int i = 0; i < npasses && available == 1; i++) {
        if (passes[i].prog) glDeleteProgram(passes[i].prog);
        if (passes[i].fbo) glDeleteFramebuffers(1, &passes[i].fbo);
        if (passes[i].tex) glDeleteTextures(1, &passes[i].tex);
    }
    for (int k = 0; k < nluts && available == 1; k++) glDeleteTextures(1, &luts[k].tex);
    memset(passes, 0, sizeof passes);
    npasses = nluts = nparams = 0;
}

bool glsh_loaded(void) { return npasses > 0 && passes[npasses - 1].prog != 0; }

/* ------------------------------------------------------------- disegno */

static void ensure_target(pass_t *ps, int w, int h) {
    if (ps->fbo && ps->w == w && ps->h == h) return;
    if (!ps->tex) glGenTextures(1, &ps->tex);
    if (!ps->fbo) glGenFramebuffers(1, &ps->fbo);
    glBindTexture(GL_TEXTURE_2D, ps->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, ps->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ps->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) ui_log("shader: framebuffer %dx%d incompleto", w, h);
    ps->w = w;
    ps->h = h;
}

static int scaled(int type, float s, int src, int vp) {
    int v = type == SCALE_VIEWPORT ? (int)(vp * s) : type == SCALE_ABSOLUTE ? (int)s : (int)(src * s);
    return v < 1 ? 1 : v > 4096 ? 4096 : v;
}

void glsh_draw(SDL_Texture *tex, int tex_w, int tex_h, int frame_w, int frame_h,
               SDL_Rect dst, unsigned rot, bool smooth, unsigned frame_count) {
    if (!glsh_loaded()) return;
    /* i comandi SDL in coda vanno eseguiti prima del nostro disegno */
    SDL_RenderFlush(UI.ren);

    /* stato OpenGL di SDL: va rimesso com'era, perché SDL ne tiene una copia */
    GLint old_prog, old_active, old_tex, old_buf, old_fbo, old_vp[4], old_attr[3];
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, old_vp);
    for (int i = 0; i < 3; i++) glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &old_attr[i]);
    GLboolean old_blend = glIsEnabled(GL_BLEND), old_scissor = glIsEnabled(GL_SCISSOR_TEST);

    float tw = 1, th = 1;
    SDL_GL_BindTexture(tex, &tw, &th);
    GLint orig_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &orig_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttrib4f(2, 1, 1, 1, 1);

    bool odd = rot & 1;
    int vp_w = odd ? dst.h : dst.w, vp_h = odd ? dst.w : dst.h;   /* uscita nell'orientamento del gioco */
    float orig_tex_w = tex_w / tw, orig_tex_h = tex_h / th;
    /* ingresso del passaggio: all'inizio l'immagine del gioco, poi l'uscita precedente */
    GLuint in_tex = (GLuint)orig_tex;
    float in_w = (float)frame_w, in_h = (float)frame_h, in_tw = orig_tex_w, in_th = orig_tex_h;
    static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    for (int i = 0; i < npasses; i++) {
        pass_t *ps = &passes[i];
        bool last = i == npasses - 1;
        int out_w = vp_w, out_h = vp_h;
        if (!last) {
            out_w = scaled(ps->stype_x == SCALE_DEFAULT ? SCALE_SOURCE : ps->stype_x, ps->sx, (int)in_w, vp_w);
            out_h = scaled(ps->stype_y == SCALE_DEFAULT ? SCALE_SOURCE : ps->stype_y, ps->sy, (int)in_h, vp_h);
            ensure_target(ps, out_w, out_h);
            glBindFramebuffer(GL_FRAMEBUFFER, ps->fbo);
            glViewport(0, 0, out_w, out_h);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
            glViewport(0, 0, UI.w, UI.h);
        }
        glUseProgram(ps->prog);

        /* ingresso sull'unità 0, con il filtro richiesto da questo passaggio */
        bool linear = ps->linear >= 0 ? ps->linear : (i == 0 ? smooth : false);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        int unit = 1;
        if (ps->u_orig >= 0) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, (GLuint)orig_tex);
            glUniform1i(ps->u_orig, unit++);
        }
        for (int j = 0; j + 1 < i; j++)
            if (ps->u_pass[j] >= 0 && unit < 8) {
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, passes[j].tex);
                glUniform1i(ps->u_pass[j], unit++);
            }
        for (int k = 0; k < nluts; k++)
            if (ps->u_lut[k] >= 0 && unit < 12) {
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, luts[k].tex);
                glUniform1i(ps->u_lut[k], unit++);
            }
        glActiveTexture(GL_TEXTURE0);

        if (ps->u_mvp >= 0) glUniformMatrix4fv(ps->u_mvp, 1, 0, identity);
        if (ps->u_dir >= 0) glUniform1i(ps->u_dir, 1);
        if (ps->u_count >= 0) glUniform1i(ps->u_count, (GLint)(ps->fc_mod ? frame_count % (unsigned)ps->fc_mod : frame_count));
        if (ps->u_out >= 0) glUniform2f(ps->u_out, (float)out_w, (float)out_h);
        if (ps->u_tex_size >= 0) glUniform2f(ps->u_tex_size, in_tw, in_th);
        if (ps->u_in >= 0) glUniform2f(ps->u_in, in_w, in_h);
        if (ps->u_tex >= 0) glUniform1i(ps->u_tex, 0);
        if (ps->u_orig_size >= 0) glUniform2f(ps->u_orig_size, orig_tex_w, orig_tex_h);
        if (ps->u_orig_in >= 0) glUniform2f(ps->u_orig_in, (float)frame_w, (float)frame_h);
        for (int j = 0; j + 1 < i; j++) {
            if (ps->u_pass_size[j] >= 0) glUniform2f(ps->u_pass_size[j], (float)passes[j].w, (float)passes[j].h);
            if (ps->u_pass_in[j] >= 0) glUniform2f(ps->u_pass_in[j], (float)passes[j].w, (float)passes[j].h);
        }
        for (int k = 0; k < nparams; k++)
            if (ps->u_param[k] >= 0) glUniform1f(ps->u_param[k], params[k].value);

        /* angoli: alto-sx, alto-dx, basso-dx, basso-sx dell'immagine. Nelle texture di
           destinazione (le nostre e il canvas di SDL) la riga 0 sta a y = -1. */
        float umax = in_w / in_tw, vmax = in_h / in_th;
        float iu[4] = { 0, umax, umax, 0 }, iv[4] = { 0, 0, vmax, vmax };
        float cx[4], cy[4];
        if (last) {
            float x0 = (float)dst.x, x1 = (float)(dst.x + dst.w), y0 = (float)dst.y, y1 = (float)(dst.y + dst.h);
            float ax[4] = { x0, x1, x1, x0 }, ay[4] = { y0, y0, y1, y1 };
            for (int c = 0; c < 4; c++) { cx[c] = ax[c] / UI.w * 2 - 1; cy[c] = ay[c] / UI.h * 2 - 1; }
        } else {
            float ax[4] = { -1, 1, 1, -1 }, ay[4] = { -1, -1, 1, 1 };
            memcpy(cx, ax, sizeof cx);
            memcpy(cy, ay, sizeof cy);
        }
        float pos[4][4], uv[4][4];
        static const int strip[4] = { 0, 1, 3, 2 };   /* ordine per GL_TRIANGLE_STRIP */
        for (int k = 0; k < 4; k++) {
            /* angolo i dell'immagine -> angolo (i - rot) dello schermo; ruota solo l'ultimo passaggio */
            int ic = strip[k], d = last ? (int)((ic - (int)rot + 8) % 4) : ic;
            pos[k][0] = cx[d];
            pos[k][1] = cy[d];
            pos[k][2] = 0;
            pos[k][3] = 1;
            uv[k][0] = iu[ic];
            uv[k][1] = iv[ic];
            uv[k][2] = uv[k][3] = 0;
        }
        glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, pos);
        glVertexAttribPointer(1, 4, GL_FLOAT, 0, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (!last) {
            in_tex = ps->tex;
            in_w = in_tw = (float)out_w;
            in_h = in_th = (float)out_h;
        }
    }

    /* ripristino: unità di texture usate, programma, buffer, framebuffer, stato */
    for (int u = 11; u >= 1; u--) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)orig_tex);
    SDL_GL_UnbindTexture(tex);
    glUseProgram((GLuint)old_prog);
    for (int i = 0; i < 3; i++) {
        if (old_attr[i]) glEnableVertexAttribArray((GLuint)i);
        else glDisableVertexAttribArray((GLuint)i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buf);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
    glActiveTexture((GLenum)old_active);
    glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
    if (old_blend) glEnable(GL_BLEND);
    if (old_scissor) glEnable(GL_SCISSOR_TEST);
}
