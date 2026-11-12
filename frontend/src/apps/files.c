/* Esplora file: app di Mosaik, pacchetto dello Store (Arcade/Apps/files).

   Sfoglia la cartella Arcade della scheda, come la pagina del trasferimento file: si scrive solo
   in Roms, Bios, Saves, Media, Music, Linux, Apps e Bezels (le cartelle stesse non si tolgono né
   si rinominano), il resto si guarda soltanto; System/wifi.conf e i file nascosti non compaiono.
   Copia, sposta, rinomina, elimina, nuova cartella ed estrai gli zip; X segna più voci. Copie e cancellazioni
   lavorano in un thread con l'avanzamento a schermo (B interrompe); un file copiato si scrive
   come ".nome.part" e prende il suo nome solo quando è completo.

   Parte da launch.sh come un gioco Linux (gamewatch, senza il menu di Mosaik: GAMEWATCH_NOMENU
   in app.json); B nella cartella principale o Menu tornano al launcher. Le scritte sono in
   lang/<lingua>.json accanto al programma, con l'inglese come riserva.

   Sul Mac: build/files --root sdcard [--auto "wait:30,shot:/tmp/f.png,quit"] */
#include "appkit.h"
#include "../launcher/zipread.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ voci della cartella */

typedef struct {
    char name[256];
    bool dir, mark;
    long long size;
} ent_t;

#define root app_root
static char cwd[1024];       /* relativa a root, "" = principale */
static ent_t *ents;
static int nents, cap_ents, sel;
static float scroll;
static long long free_bytes;

static const char *const WRITABLE[] = { "Roms", "Bios", "Saves", "Media", "Music", "Linux", "Apps", "Bezels", "Themes", NULL };

static void join(char *out, size_t n, const char *a, const char *b) {
    if (!a[0]) snprintf(out, n, "%s", b);
    else if (!b[0]) snprintf(out, n, "%s", a);
    else snprintf(out, n, "%s/%s", a, b);
}

static void full(char *out, size_t n, const char *rel) {
    if (rel[0]) snprintf(out, n, "%s/%s", root, rel);
    else snprintf(out, n, "%s", root);
}

/* la cartella principale di rel (prima parte del percorso) è scrivibile? */
static bool in_writable(const char *rel) {
    char top[64];
    size_t k = strcspn(rel, "/");
    if (!rel[0] || k >= sizeof top) return false;
    memcpy(top, rel, k);
    top[k] = 0;
    for (int i = 0; WRITABLE[i]; i++)
        if (!strcasecmp(top, WRITABLE[i])) return true;
    return false;
}

/* si può modificare la voce rel (copiarla si può sempre): dentro una cartella scrivibile, e non
   la cartella principale stessa */
static bool can_modify(const char *rel) { return in_writable(rel) && strchr(rel, '/'); }

static bool hidden(const char *dir_rel, const char *name) {
    if (name[0] == '.') return true;
    if (!strcasecmp(dir_rel, "System") && (!strcasecmp(name, "wifi.conf") || !strcasecmp(name, "fileserver-browsers.txt"))) return true;
    return false;
}

static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void space_update(void) {
    struct statvfs v;
    free_bytes = statvfs(root, &v) == 0 ? (long long)v.f_bavail * (long long)v.f_frsize : -1;
}

/* rilegge la cartella; keep: il nome da tenere scelto (dopo un'operazione) */
static void reload(const char *keep) {
    char p[1300];
    full(p, sizeof p, cwd);
    nents = 0;
    bool cut = false;   /* nome troppo lungo o memoria finita: l'elenco non è intero e si dice */
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (hidden(cwd, e->d_name) || !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char q[1600];
        struct stat sb;
        if (snprintf(q, sizeof q, "%s/%s", p, e->d_name) >= (int)sizeof q || strlen(e->d_name) >= sizeof ents[0].name) { cut = true; continue; }
        if (stat(q, &sb) != 0) continue;
        if (nents == cap_ents) {
            int nc = cap_ents ? cap_ents * 2 : 256;
            ent_t *ne = realloc(ents, (size_t)nc * sizeof *ne);
            if (!ne) { cut = true; break; }
            ents = ne;
            cap_ents = nc;
        }
        ent_t *x = &ents[nents++];
        memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", e->d_name);
        x->dir = S_ISDIR(sb.st_mode);
        x->size = x->dir ? 0 : (long long)sb.st_size;
    }
    if (d) closedir(d);
    if (cut) { ui_log("file: elenco di %s incompleto", cwd); ui_toast(FT("files.list_incomplete")); }
    if (nents > 1) qsort(ents, (size_t)nents, sizeof *ents, ent_cmp);
    if (sel >= nents) sel = nents ? nents - 1 : 0;
    for (int i = 0; keep && i < nents; i++)
        if (!strcmp(ents[i].name, keep)) { sel = i; break; }
    space_update();
}

static void go_into(const char *name) {
    char n[1024];
    join(n, sizeof n, cwd, name);
    snprintf(cwd, sizeof cwd, "%s", n);
    sel = 0;
    scroll = 0;
    reload(NULL);
}

static void go_up(void) {
    char last[256] = "";
    char *s = strrchr(cwd, '/');
    if (s) { snprintf(last, sizeof last, "%s", s + 1); *s = 0; }
    else { snprintf(last, sizeof last, "%s", cwd); cwd[0] = 0; }
    scroll = 0;
    reload(last);
}

static int marked_count(void) {
    int n = 0;
    for (int i = 0; i < nents; i++) n += ents[i].mark;
    return n;
}

/* ------------------------------------------------------------ lavori: copia, sposta, elimina */

enum { JOB_NONE, JOB_COPY, JOB_MOVE, JOB_DELETE, JOB_EXTRACT };
static struct {
    int kind;
    char src_dir[1024];          /* relativa a root */
    char (*names)[256];
    int n;
    char dst_dir[1024];          /* relativa a root (copia e spostamento) */
    _Atomic long long done, total;
    _Atomic int files_done, failed;
    _Atomic bool running, cancel;
    char current[256];           /* letto dalla finestra: sotto cur_lock */
    char first[256];             /* nome della prima voce arrivata (copia e spostamento) */
    _Atomic bool nospace;        /* estrazione: lo spazio non basta, niente scritto */
} job;
static pthread_mutex_t cur_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_current(const char *name) {
    pthread_mutex_lock(&cur_lock);
    snprintf(job.current, sizeof job.current, "%s", name);
    pthread_mutex_unlock(&cur_lock);
}
static pthread_t job_thread;
static bool job_joinable;

/* clipboard: voci da copiare o spostare, con la cartella di partenza */
static struct { int kind; char dir[1024]; char (*names)[256]; int n; } clip;

static void tree_size(const char *p, long long *bytes, int *files, int depth) {
    struct stat sb;
    if (lstat(p, &sb) != 0) return;
    if (!S_ISDIR(sb.st_mode)) { *bytes += sb.st_size; (*files)++; return; }
    DIR *d = depth < 32 ? opendir(p) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char q[1600];
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        tree_size(q, bytes, files, depth + 1);
    }
    if (d) closedir(d);
}

/* nome libero nella cartella: "nome (2).ext", "nome (3).ext"...; falso se non ce n'è uno (mai
   restituire un nome occupato: copiando lo si sovrascriverebbe, audit APP-005) */
static bool unique_name(const char *dir_full, const char *name, char *out, size_t n) {
    char p[1600];
    snprintf(p, sizeof p, "%s/%s", dir_full, name);
    if (access(p, F_OK) != 0) { snprintf(out, n, "%s", name); return strlen(name) < n; }
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int k = 2; k < 1000; k++) {
        int w = snprintf(out, n, "%.*s (%d)%s", stem, name, k, dot && dot != name ? dot : "");
        if (w < 0 || (size_t)w >= n) return false;
        snprintf(p, sizeof p, "%s/%s", dir_full, out);
        if (access(p, F_OK) != 0) return true;
    }
    return false;
}

/* password Wi-Fi e browser ricordati: non escono da System nemmeno copiando la cartella (APP-007) */
static bool secret_file(const char *path) {
    const char *b = strrchr(path, '/');
    if (!b) return false;
    const char *pd = b;
    while (pd > path && pd[-1] != '/') pd--;
    return (size_t)(b - pd) == 6 && !strncasecmp(pd, "System", 6) &&
           (!strcasecmp(b + 1, "wifi.conf") || !strcasecmp(b + 1, "fileserver-browsers.txt"));
}

static bool copy_file(const char *src, const char *dst_dir, const char *name) {
    char part[1600], dst[1600];
    snprintf(part, sizeof part, "%s/.%s.%d.part", dst_dir, name, (int)getpid());   /* temporaneo solo nostro */
    snprintf(dst, sizeof dst, "%s/%s", dst_dir, name);
    int in = open(src, O_RDONLY);
    if (in < 0) return false;
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return false; }
    static char buf[256 * 1024];
    bool ok = true;
    for (;;) {
        if (job.cancel) { ok = false; break; }
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) { ok = false; break; }
        if (r == 0) break;
        for (ssize_t off = 0; off < r;) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
        job.done += r;
    }
    close(in);
    if (ok && fsync(out) != 0) ok = false;
    if (close(out) != 0) ok = false;
    if (ok && rename(part, dst) != 0) ok = false;
    if (!ok) unlink(part);
    return ok;
}

static bool copy_tree(const char *src, const char *dst_dir, const char *name, int depth) {
    struct stat sb;
    if (job.cancel || lstat(src, &sb) != 0) return false;
    set_current(name);
    if (secret_file(src)) return true;   /* saltato apposta, non è un errore */
    if (!S_ISDIR(sb.st_mode)) {
        bool ok = copy_file(src, dst_dir, name);
        if (ok) job.files_done++;
        return ok;
    }
    char nd[1600];
    snprintf(nd, sizeof nd, "%s/%s", dst_dir, name);
    if (mkdir(nd, 0755) != 0 && errno != EEXIST) return false;
    DIR *d = depth < 32 ? opendir(src) : NULL;
    struct dirent *e;
    bool ok = d != NULL;
    while (d && (e = readdir(d)) && !job.cancel) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[1600];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        ok &= copy_tree(s, nd, e->d_name, depth + 1);
    }
    if (d) closedir(d);
    return ok && !job.cancel;
}

static bool remove_tree(const char *p, int depth) {
    struct stat sb;
    if (job.cancel || lstat(p, &sb) != 0) return false;
    if (!S_ISDIR(sb.st_mode)) {
        bool ok = unlink(p) == 0;
        if (ok) { job.done += sb.st_size; job.files_done++; }
        return ok;
    }
    DIR *d = depth < 32 ? opendir(p) : NULL;
    struct dirent *e;
    bool ok = d != NULL;
    while (d && (e = readdir(d)) && !job.cancel) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char q[1600];
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        ok &= remove_tree(q, depth + 1);
    }
    if (d) closedir(d);
    return ok && !job.cancel && rmdir(p) == 0;
}

/* cartelle di un percorso relativo (dentro dst) */
static bool make_dirs(const char *dst, const char *rel) {
    char p[1600];
    snprintf(p, sizeof p, "%s/%s", dst, rel);
    for (char *c = p + strlen(dst) + 1; (c = strchr(c, '/')); c++) {
        *c = 0;
        if (mkdir(p, 0755) != 0 && errno != EEXIST) return false;
        *c = '/';
    }
    return true;
}

/* Estrai qui: tutte le voci dello zip nella cartella dst. Nomi con "..", assoluti o vuoti si
   saltano; i caratteri che il FAT non accetta diventano "_"; un file che c'è già non si tocca
   (la copia prende "nome (2)"); prima si controlla che lo spazio basti. */
static bool extract_zip(const char *zpath, const char *dst) {
    FILE *z = fopen(zpath, "rb");
    zent_t *ze = NULL;
    int n = z ? zip_dir(z, &ze) : -1;
    if (n < 0) { if (z) fclose(z); return false; }
    long long total = 0;
    for (int i = 0; i < n; i++) total += ze[i].usize;
    /* spazio: se non si riesce a misurarlo non si estrae (verifica audit 0.9.26) */
    struct statvfs v;
    if (statvfs(dst, &v) != 0 || total + (64LL << 20) > (long long)v.f_bavail * (long long)v.f_frsize) {
        job.nospace = true;
        free(ze);
        fclose(z);
        return false;
    }
    zip_cancel_flag(&job.cancel);   /* B ferma anche dentro una voce grande */
    job.total = total;
    bool ok = true;
    for (int i = 0; i < n && !job.cancel; i++) {
        char rel[512];
        snprintf(rel, sizeof rel, "%s", ze[i].name);
        for (char *c = rel; *c; c++) {
            if (*c == '\\') *c = '/';
            else if (strchr(":*?\"<>|", *c) || (unsigned char)*c < 32) *c = '_';
        }
        bool dir = rel[0] && rel[strlen(rel) - 1] == '/';
        if (dir) rel[strlen(rel) - 1] = 0;
        if (!rel[0] || rel[0] == '/' || !strcmp(rel, "..") || !strncmp(rel, "../", 3) || strstr(rel, "/../") ||
            (strlen(rel) >= 3 && !strcmp(rel + strlen(rel) - 3, "/.."))) continue;   /* fuori dalla cartella: mai */
        set_current(rel);
        if (dir) {
            char d[1600];
            snprintf(d, sizeof d, "%s/%s/", dst, rel);
            ok &= make_dirs(dst, d + strlen(dst) + 1);
            continue;
        }
        if (!make_dirs(dst, rel)) { ok = false; continue; }
        char dir_full[1600], name[256], out[256];
        const char *slash = strrchr(rel, '/');
        snprintf(dir_full, sizeof dir_full, "%s/%.*s", dst, slash ? (int)(slash - rel) : 0, rel);
        if (!slash) snprintf(dir_full, sizeof dir_full, "%s", dst);
        snprintf(name, sizeof name, "%s", slash ? slash + 1 : rel);
        if (!unique_name(dir_full, name, out, sizeof out)) { ok = false; continue; }
        char part[1700], fin[1700];
        snprintf(part, sizeof part, "%s/.%s.%d.part", dir_full, out, (int)getpid());   /* temporaneo solo nostro */
        snprintf(fin, sizeof fin, "%s/%s", dir_full, out);
        FILE *o = fopen(part, "wb");
        bool fok = o && zip_entry(z, &ze[i], o, NULL, 0) && fflush(o) == 0 && fsync(fileno(o)) == 0;
        if (o && fclose(o) != 0) fok = false;
        if (fok && rename(part, fin) != 0) fok = false;
        if (!fok) { unlink(part); ok = false; continue; }
        job.done += ze[i].usize;
        job.files_done++;
    }
    zip_cancel_flag(NULL);
    free(ze);
    fclose(z);
    return ok && !job.cancel;
}

static void *job_worker(void *arg) {
    (void)arg;
    char sd[1300], dd[1300];
    full(sd, sizeof sd, job.src_dir);
    full(dd, sizeof dd, job.dst_dir);
    long long total = 0;
    int files = 0;
    for (int i = 0; i < job.n; i++) {
        char p[1600];
        snprintf(p, sizeof p, "%s/%s", sd, job.names[i]);
        if (job.kind != JOB_MOVE && job.kind != JOB_EXTRACT) tree_size(p, &total, &files, 0);
    }
    job.total = total;
    for (int i = 0; i < job.n && !job.cancel; i++) {
        char p[1600], name[256];
        snprintf(p, sizeof p, "%s/%s", sd, job.names[i]);
        set_current(job.names[i]);
        bool ok;
        if (job.kind == JOB_DELETE) ok = remove_tree(p, 0);
        else {
            if (job.kind == JOB_EXTRACT) {
                ok = extract_zip(p, dd);
                if (!ok) job.failed++;
                ui_log("file: estraggo %s/%s: %s", job.src_dir, job.names[i], ok ? "fatto" : job.nospace ? "spazio insufficiente" : "errore");
                continue;
            }
            if (!unique_name(dd, job.names[i], name, sizeof name)) { job.failed++; continue; }
            if (i == 0) snprintf(job.first, sizeof job.first, "%s", name);
            if (job.kind == JOB_MOVE) {   /* stessa scheda: basta cambiare nome */
                char q[1600];
                snprintf(q, sizeof q, "%s/%s", dd, name);
                ok = rename(p, q) == 0;
                if (ok) job.files_done++;
            } else ok = copy_tree(p, dd, name, 0);
        }
        if (!ok) job.failed++;
        ui_log("file: %s %s/%s: %s", job.kind == JOB_DELETE ? "elimino" : job.kind == JOB_MOVE ? "sposto" : "copio",
               job.src_dir, job.names[i], ok ? "fatto" : "errore");
    }
    sync();
    job.running = false;
    return NULL;
}

static void job_start(int kind, const char *src_dir, char (*names)[256], int n, const char *dst_dir) {
    job.kind = kind;
    snprintf(job.src_dir, sizeof job.src_dir, "%s", src_dir);
    snprintf(job.dst_dir, sizeof job.dst_dir, "%s", dst_dir ? dst_dir : "");
    free(job.names);
    job.names = malloc((size_t)n * sizeof *job.names);
    if (!job.names) return;
    memcpy(job.names, names, (size_t)n * sizeof *job.names);
    job.n = n;
    job.done = job.total = 0;
    job.files_done = job.failed = 0;
    job.cancel = false;
    job.current[0] = job.first[0] = 0;
    job.nospace = false;
    job.running = true;
    if (pthread_create(&job_thread, NULL, job_worker, NULL) == 0) job_joinable = true;
    else job.running = false;
}

/* voci su cui agire: le segnate, altrimenti quella scelta */
static int selection(char (**out)[256]) {
    int n = marked_count();
    if (!n && nents) n = 1;
    *out = n ? malloc((size_t)n * sizeof **out) : NULL;
    if (!*out) return 0;
    int k = 0;
    if (marked_count()) { for (int i = 0; i < nents; i++) if (ents[i].mark) snprintf((*out)[k++], 256, "%s", ents[i].name); }
    else snprintf((*out)[k++], 256, "%s", ents[sel].name);
    return k;
}

/* ------------------------------------------------------------ finestre */

enum { W_NONE, W_MENU, W_DELETE, W_KB, W_JOB };
static int win, menu_sel, ask_sel;
enum { A_COPY, A_CUT, A_PASTE, A_RENAME, A_DELETE, A_NEWDIR, A_MARKALL, A_EXTRACT, A_COUNT };
static int menu_items[A_COUNT], nmenu;
static long long del_bytes;
static int del_files;
static bool kb_new;        /* tastiera: true nuova cartella, false rinomina */
static char kb_text[256];
static int kb_row, kb_col, kb_layout;

static void toast(const char *key) { ui_toast(FT(key)); }

static void menu_open(void) {
    nmenu = 0;
    /* le voci di cwd si modificano se cwd sta dentro una cartella scrivibile (can_modify) */
    bool any = nents > 0, mod = false;
    if (any) { char rel[1300]; join(rel, sizeof rel, cwd, ents[sel].name); mod = can_modify(rel); }
    if (any) menu_items[nmenu++] = A_COPY;
    if (mod) menu_items[nmenu++] = A_CUT;
    if (clip.n && in_writable(cwd)) menu_items[nmenu++] = A_PASTE;
    if (mod && marked_count() <= 1) menu_items[nmenu++] = A_RENAME;
    if (any && !marked_count() && !ents[sel].dir && in_writable(cwd)) {   /* Estrai qui: solo sugli zip */
        size_t l = strlen(ents[sel].name);
        if (l > 4 && !strcasecmp(ents[sel].name + l - 4, ".zip")) menu_items[nmenu++] = A_EXTRACT;
    }
    if (mod) menu_items[nmenu++] = A_DELETE;
    if (in_writable(cwd)) menu_items[nmenu++] = A_NEWDIR;
    if (any) menu_items[nmenu++] = A_MARKALL;
    if (!nmenu) { toast("files.read_only"); ui_sound(SND_ERROR); return; }
    menu_sel = 0;
    win = W_MENU;
    ui_sound(SND_OK);
}

/* la voce da rinominare: quella segnata, se ce n'è una sola, altrimenti quella scelta */
static const char *rename_target(void) {
    for (int i = 0; i < nents; i++) if (ents[i].mark) return ents[i].name;
    return nents ? ents[sel].name : "";
}

static void kb_open(bool newdir) {
    kb_new = newdir;
    snprintf(kb_text, sizeof kb_text, "%s", newdir ? "" : rename_target());
    kb_row = 1;
    kb_col = 0;
    kb_layout = 1;
    win = W_KB;
}

static void action(int a) {
    char (*names)[256] = NULL;
    int n;
    win = W_NONE;
    switch (a) {
    case A_COPY:
    case A_CUT:
        n = selection(&names);
        if (!n) return;
        free(clip.names);
        clip.names = names;
        clip.n = n;
        clip.kind = a == A_COPY ? JOB_COPY : JOB_MOVE;
        snprintf(clip.dir, sizeof clip.dir, "%s", cwd);
        for (int i = 0; i < nents; i++) ents[i].mark = false;
        toast(a == A_COPY ? "files.copied" : "files.cut");
        break;
    case A_PASTE: {
        /* una cartella non va dentro se stessa */
        for (int i = 0; i < clip.n; i++) {
            char srcp[1300];
            join(srcp, sizeof srcp, clip.dir, clip.names[i]);
            size_t l = strlen(srcp);
            if (!strncmp(cwd, srcp, l) && (cwd[l] == 0 || cwd[l] == '/')) { toast("files.into_itself"); ui_sound(SND_ERROR); return; }
        }
        if (clip.kind == JOB_MOVE && !strcmp(clip.dir, cwd)) { toast("files.same_folder"); return; }
        job_start(clip.kind, clip.dir, clip.names, clip.n, cwd);
        win = W_JOB;
        if (clip.kind == JOB_MOVE) { free(clip.names); clip.names = NULL; clip.n = 0; }
        break;
    }
    case A_RENAME: kb_open(false); break;
    case A_NEWDIR: kb_open(true); break;
    case A_DELETE: {
        n = selection(&names);
        del_bytes = 0;
        del_files = 0;
        char base[1300];
        full(base, sizeof base, cwd);
        for (int i = 0; i < n; i++) {
            char p[1600];
            snprintf(p, sizeof p, "%s/%s", base, names[i]);
            tree_size(p, &del_bytes, &del_files, 0);
        }
        free(names);
        ask_sel = 0;
        win = W_DELETE;
        break;
    }
    case A_EXTRACT: {
        char one[1][256];
        snprintf(one[0], 256, "%s", ents[sel].name);
        job_start(JOB_EXTRACT, cwd, one, 1, cwd);
        win = W_JOB;
        break;
    }
    case A_MARKALL: {
        bool all = marked_count() == nents;
        for (int i = 0; i < nents; i++) ents[i].mark = !all;
        break;
    }
    }
}

/* nomi validi sulla scheda FAT: niente / \ : * ? " < > | e niente punto o spazio in fondo */
static bool name_ok(const char *s) {
    if (!s[0] || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    if (strpbrk(s, "/\\:*?\"<>|")) return false;
    size_t n = strlen(s);
    return s[n - 1] != '.' && s[n - 1] != ' ' && s[0] != ' ';
}

/* Rinomina di sole maiuscole: il passaggio intermedio si annota in Apps/files/rinomina.txt
   (temporaneo, nome di prima) prima di farlo. Se la corrente manca a metà, alla prossima apertura
   il file torna al nome di prima (verifica audit APP-006). */
/* true solo se la nota è sulla scheda (audit A38-005): senza, la rinomina non parte. tmp NULL
   toglie la nota, a rinomina conclusa. */
static bool rename_note(const char *tmp, const char *orig) {
    char j[1200], jt[1300];
    snprintf(j, sizeof j, "%s/rinomina.txt", app_dir);
    snprintf(jt, sizeof jt, "%s.tmp", j);
    if (!tmp) {
        bool gone = unlink(j) == 0 || errno == ENOENT;
        if (!gone) ui_log("file: la nota della rinomina resta: %s", strerror(errno));
        sync();
        return gone;
    }
    FILE *f = fopen(jt, "w");
    bool ok = f && fprintf(f, "%s\n%s\n", tmp, orig) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = false;
    if (ok && rename(jt, j) != 0) ok = false;
    if (!ok) { ui_log("file: nota della rinomina non scritta (%s): non rinomino", strerror(errno)); unlink(jt); return false; }
    sync();   /* la nota deve esserci prima del primo spostamento */
    return true;
}

static void rename_recover(void) {
    char j[1200], tmp[1600] = "", orig[1600] = "";
    snprintf(j, sizeof j, "%s/rinomina.txt", app_dir);
    FILE *f = fopen(j, "r");
    if (!f) return;
    bool got = fgets(tmp, sizeof tmp, f) && fgets(orig, sizeof orig, f);
    fclose(f);
    tmp[strcspn(tmp, "\n")] = 0;
    orig[strcspn(orig, "\n")] = 0;
    /* solo un temporaneo nostro, dentro la scheda */
    bool ours = got && strstr(tmp, "/.rinomina-") && !strncmp(tmp, app_root, strlen(app_root)) && !strncmp(orig, app_root, strlen(app_root));
    if (ours && access(tmp, F_OK) == 0 && access(orig, F_OK) != 0) {
        int r = rename(tmp, orig);
        ui_log("file: rinomina interrotta, rimesso %s: %s", orig, r == 0 ? "fatto" : strerror(errno));
        if (r != 0) return;   /* si riprova alla prossima apertura */
        sync();
    }
    unlink(j);
}

static void kb_done(void) {
    if (!name_ok(kb_text)) { toast("files.bad_name"); ui_sound(SND_ERROR); return; }
    char base[1300], p[1600], q[1600];
    full(base, sizeof base, cwd);
    snprintf(q, sizeof q, "%s/%s", base, kb_text);
    bool ok;
    if (kb_new) ok = mkdir(q, 0755) == 0;
    else {
        const char *old = rename_target();
        snprintf(p, sizeof p, "%s/%s", base, old);
        if (!strcmp(old, kb_text)) { win = W_NONE; return; }
        /* stesso nome con maiuscole diverse: sul FAT serve un passaggio intermedio */
        if (!strcasecmp(old, kb_text)) {
            /* temporaneo che non esiste già; se il secondo passo fallisce si rimette il nome di
               prima (audit APP-006) */
            char t[1600];
            ok = false;
            for (int k = 0; k < 100; k++) {
                snprintf(t, sizeof t, "%s/.rinomina-%d-%d.tmp", base, (int)getpid(), k);
                if (access(t, F_OK) != 0) { ok = true; break; }
            }
            if (ok) ok = rename_note(t, p);
            if (!ok) {
                /* nome occupato o nota non scritta: niente è stato toccato */
            } else if (rename(p, t) == 0) {
                if (rename(t, q) != 0) { ok = false; if (rename(t, p) != 0) ui_log("file: %s resta come %s, si rimette alla prossima apertura", p, t); else rename_note(NULL, NULL); }
                else rename_note(NULL, NULL);
            } else { ok = false; rename_note(NULL, NULL); }
        } else ok = access(q, F_OK) != 0 && rename(p, q) == 0;
    }
    ui_log("file: %s %s/%s: %s", kb_new ? "nuova cartella" : "rinomino in", cwd, kb_text, ok ? "fatto" : "errore");
    if (!ok) { toast(kb_new ? "files.exists" : "files.rename_failed"); ui_sound(SND_ERROR); return; }
    sync();
    win = W_NONE;
    for (int i = 0; i < nents; i++) ents[i].mark = false;
    reload(kb_text);
    ui_sound(SND_OK);
}

/* tastiera: lettere, cifre e i simboli permessi nei nomi dei file; X cambia disposizione */
static const char *kb_layouts[3][4] = {
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-._" },
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-._" },
    { "1234567890", "!#$%&'()+,", "-.;=@[]^_`", "{}~!#$%&()" },
};

static void kb_type(char c) {
    size_t n = strlen(kb_text);
    if (n + 1 < sizeof kb_text && n < 120) { kb_text[n] = c; kb_text[n + 1] = 0; }
}

static void kb_input(void) {
    input_t *in = &UI.in;
    if (in->pressed[BTN_UP]) { kb_row = (kb_row + 4) % 5; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_DOWN]) { kb_row = (kb_row + 1) % 5; ui_sound(SND_MOVE); }
    int cols = kb_row < 4 ? 10 : 3;
    if (kb_col >= cols) kb_col = cols - 1;
    if (in->pressed[BTN_LEFT]) { kb_col = (kb_col + cols - 1) % cols; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_RIGHT]) { kb_col = (kb_col + 1) % cols; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_Y]) { kb_type(' '); ui_sound(SND_MOVE); }
    if (in->pressed[BTN_X]) { kb_layout = (kb_layout + 1) % 3; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_B]) {
        size_t n = strlen(kb_text);
        if (n) { kb_text[n - 1] = 0; ui_sound(SND_BACK); }
        else { win = W_NONE; ui_sound(SND_BACK); }
    }
    if (in->pressed[BTN_START]) kb_done();
    if (in->pressed[BTN_A]) {
        if (kb_row < 4) kb_type(kb_layouts[kb_layout][kb_row][kb_col]);
        else if (kb_col == 0) kb_type(' ');
        else if (kb_col == 1) { size_t n = strlen(kb_text); if (n) kb_text[n - 1] = 0; }
        else { kb_done(); return; }
        ui_sound(SND_MOVE);
    }
}

static void kb_draw(void) {
    theme_t *t = &UI.th;
    int pad = M(0.035f), key = M(0.085f), gap = M(0.012f);
    int w = pad * 2 + 10 * key + 9 * gap;
    if (w > UI.w - M(0.04f)) { key = (UI.w - M(0.04f) - pad * 2 - 9 * gap) / 10; w = pad * 2 + 10 * key + 9 * gap; }
    int field_h = M(0.075f), h = pad * 2 + field_h + M(0.02f) + 5 * key + 4 * gap, x, y;
    app_panel(w, h, &x, &y);
    int fy = y + pad;
    ui_rrect(x + pad, fy, w - 2 * pad, field_h, M(t->panel_radius), with_alpha(t->text_dim, 40));
    TTF_Font *f = UI.f_normal;
    int tx = x + pad + M(0.02f), ty = fy + (field_h - TTF_FontHeight(f)) / 2;
    char fit[300];
    ui_text_fit(f, kb_text, w - 2 * pad - M(0.05f), fit, sizeof fit);
    int tw = kb_text[0] ? ui_text(f, fit, tx, ty, t->text, ALIGN_LEFT) : 0;
    if (!kb_text[0]) ui_text(f, FT(kb_new ? "files.new_folder" : "files.rename"), tx, ty, t->text_dim, ALIGN_LEFT);
    if (SDL_fmod(UI.t, 1.0) < 0.55) ui_rect(tx + tw + 2, ty + M(0.004f), M(0.004f) + 1, TTF_FontHeight(f) - M(0.008f), t->accent);
    int ky = fy + field_h + M(0.02f);
    for (int r = 0; r < 5; r++, ky += key + gap) {
        int cols = r < 4 ? 10 : 3, kw = r < 4 ? key : (w - 2 * pad - 2 * gap) / 3;
        for (int c = 0; c < cols; c++) {
            int kx = x + pad + c * (kw + gap);
            bool on = r == kb_row && c == kb_col;
            ui_rrect(kx, ky, kw, key, M(t->panel_radius), on ? t->accent : with_alpha(t->text_dim, 45));
            char label[48];
            if (r < 4) snprintf(label, sizeof label, "%c", kb_layouts[kb_layout][r][c]);
            else snprintf(label, sizeof label, "%s", T(c == 0 ? "kb.space" : c == 1 ? "kb.delete" : "kb.done"));
            ui_text(f, label, kx + kw / 2, ky + (key - TTF_FontHeight(f)) / 2, on ? t->accent_text : t->text, ALIGN_CENTER);
        }
    }
    static const char *layout_keys[3] = { "kb.lower", "kb.symbols", "kb.upper" };
    const char *hint[] = { "X", T(layout_keys[kb_layout]), "Y", T("kb.space"), "B", T("kb.delete"), "START", T("kb.done"), "A", T("kb.type") };
    ui_hints(hint, 10);
}

static const char *action_key(int a) {
    static const char *k[A_COUNT] = { "files.copy", "files.move", "files.paste", "files.rename", "files.delete", "files.new_folder", "files.mark_all", "files.extract" };
    return k[a];
}

static void menu_draw(void) {
    theme_t *t = &UI.th;
    TTF_Font *f = UI.f_normal;
    int rh = TTF_FontHeight(f) + M(0.03f), pad = M(0.03f), w = M(0.6f), h = pad * 2 + TTF_FontHeight(UI.f_title) + M(0.02f) + nmenu * rh, x, y;
    app_panel(w, h, &x, &y);
    int n = marked_count();
    char title[300];
    if (n > 1) snprintf(title, sizeof title, FT("files.n_selected"), n);
    else snprintf(title, sizeof title, "%s", nents ? ents[sel].name : FT("files.title"));
    char fit[300];
    ui_text_fit(UI.f_title, title, w - 2 * pad, fit, sizeof fit);
    ui_text(UI.f_title, fit, x + pad, y + pad, t->text, ALIGN_LEFT);
    int ry = y + pad + TTF_FontHeight(UI.f_title) + M(0.02f);
    for (int i = 0; i < nmenu; i++, ry += rh) {
        bool on = i == menu_sel;
        if (on) ui_rrect(x + pad / 2, ry, w - pad, rh, M(t->panel_radius), t->accent);
        char label[160];
        if (menu_items[i] == A_PASTE) snprintf(label, sizeof label, FT(clip.kind == JOB_MOVE ? "files.move_here" : "files.paste_n"), clip.n);
        else if (menu_items[i] == A_MARKALL) snprintf(label, sizeof label, "%s", FT(marked_count() == nents ? "files.unmark_all" : "files.mark_all"));
        else snprintf(label, sizeof label, "%s", FT(action_key(menu_items[i])));
        color_t c = on ? t->accent_text : menu_items[i] == A_DELETE ? rgba(220, 70, 70, 255) : t->text;
        ui_text(f, label, x + pad, ry + (rh - TTF_FontHeight(f)) / 2, c, ALIGN_LEFT);
    }
}

static void job_draw(void) {
    theme_t *t = &UI.th;
    int w = M(0.8f), h = M(0.3f), x, y;
    app_panel(w, h, &x, &y);
    char line[300], a[32], b[32];
    snprintf(line, sizeof line, "%s", FT(job.kind == JOB_DELETE ? "files.deleting" : job.kind == JOB_MOVE ? "files.moving" : job.kind == JOB_EXTRACT ? "files.extracting" : "files.copying"));
    ui_text(UI.f_title, line, x + w / 2, y + M(0.04f), t->text, ALIGN_CENTER);
    char cur[256];
    pthread_mutex_lock(&cur_lock);
    snprintf(cur, sizeof cur, "%s", job.current);
    pthread_mutex_unlock(&cur_lock);
    ui_text_fit(UI.f_small, cur, w - M(0.1f), line, sizeof line);
    int ty = y + M(0.04f) + TTF_FontHeight(UI.f_title) + M(0.01f);
    ui_text(UI.f_small, line, x + w / 2, ty, t->text_dim, ALIGN_CENTER);
    int bw = w - M(0.12f), bh = M(0.012f), bx = x + M(0.06f), by = ty + TTF_FontHeight(UI.f_small) + M(0.03f);
    long long tot = job.total, done = job.done;
    ui_rrect(bx, by, bw, bh, bh / 2, with_alpha(t->text_dim, 60));
    if (tot > 0) ui_rrect(bx, by, (int)((long long)bw * (done < tot ? done : tot) / tot), bh, bh / 2, t->accent);
    app_size(done, a, sizeof a);
    app_size(tot, b, sizeof b);
    snprintf(line, sizeof line, "%s / %s", a, b);
    if (tot > 0) ui_text(UI.f_small, line, x + w / 2, by + bh + M(0.02f), t->text_dim, ALIGN_CENTER);
    const char *h2[] = { "B", T("common.cancel") };
    ui_hints(h2, 2);
}

/* ------------------------------------------------------------ elenco */

static int rows_visible(int *top_y, int *row_h) {
    theme_t *t = &UI.th;
    int y = M(t->topbar_h) + M(0.02f) + TTF_FontHeight(UI.f_normal) + M(0.025f), bottom = UI.h - ui_keybar_h() - M(0.01f);
    int rh = TTF_FontHeight(UI.f_normal) + M(0.028f);
    *top_y = y;
    *row_h = rh;
    return (bottom - y) / rh;
}

/* cartella: due rettangoli arrotondati (linguetta e corpo); file: foglio con l'angolo piegato */
static void icon(int x, int y, int s, bool dir, color_t c) {
    if (dir) {
        ui_rrect(x, y + s / 6, s * 5 / 12, s / 4, s / 10, c);
        ui_rrect(x, y + s / 4, s, s * 2 / 3, s / 8, c);
    } else {
        int w = s * 3 / 4, xx = x + (s - w) / 2;
        ui_rrect(xx, y + s / 12, w, s * 5 / 6, s / 10, with_alpha(c, 170));
        ui_triangle(xx + w - s / 4, y + s / 12, xx + w, y + s / 12, xx + w, y + s / 12 + s / 4, UI.th.surface);
    }
}

static void list_draw(void) {
    theme_t *t = &UI.th;
    int mx = M(t->margin), W = UI.w - 2 * mx;
    ui_topbar(FT("files.title"));
    TTF_Font *fn = UI.f_normal, *fs = UI.f_small;
    int fnh = TTF_FontHeight(fn), fsh = TTF_FontHeight(fs);
    /* percorso e spazio libero */
    int py = M(t->topbar_h) + M(0.02f);
    char path[1600] = "Arcade", fit[1600], sp[48], free_s[64];
    for (const char *c = cwd, *s; *c; c = *s ? s + 1 : s) {   /* "Arcade / Roms / FBNEO" */
        s = c + strcspn(c, "/");
        size_t l = strlen(path);
        snprintf(path + l, sizeof path - l, " / %.*s", (int)(s - c), c);
    }
    if (free_bytes >= 0) { app_size(free_bytes, sp, sizeof sp); snprintf(free_s, sizeof free_s, FT("files.free"), sp); }
    else free_s[0] = 0;
    int fw = ui_text_w(fs, free_s);
    ui_text(fs, free_s, mx + W, py + (fnh - fsh) / 2, t->text_dim, ALIGN_RIGHT);
    ui_text_fit(fn, path, W - fw - M(0.03f), fit, sizeof fit);
    ui_text(fn, fit, mx, py, t->accent, ALIGN_LEFT);
    if (!in_writable(cwd)) {   /* sola lettura: lo si dice accanto al percorso */
        int pw = ui_text_w(fn, fit);
        ui_text(fs, FT("files.read_only_tag"), mx + pw + M(0.02f), py + (fnh - fsh) / 2, t->text_dim, ALIGN_LEFT);
    }
    int top_y, rh, vis = rows_visible(&top_y, &rh);
    if (!nents) {
        ui_text(fn, FT("files.empty"), UI.w / 2, top_y + rh, t->text_dim, ALIGN_CENTER);
    } else {
        int first = (int)scroll;
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        scroll = (float)first;
        for (int i = first; i < nents && i < first + vis; i++) {
            ent_t *e = &ents[i];
            int ry = top_y + (i - first) * rh;
            bool on = i == sel;
            if (on) ui_rrect(mx - M(0.012f), ry, W + M(0.024f), rh, M(t->panel_radius), with_alpha(t->accent, 230));
            else if (e->mark) ui_rrect(mx - M(0.012f), ry, W + M(0.024f), rh, M(t->panel_radius), with_alpha(t->accent, 50));
            color_t tc = on ? t->accent_text : t->text, dc = on ? t->accent_text : t->text_dim;
            int is = fnh, ix = mx;
            if (e->mark) {   /* segnata: cerchio pieno con un punto */
                ui_circle(ix + is / 2, ry + rh / 2, is / 2, on ? t->accent_text : t->accent);
                ui_circle(ix + is / 2, ry + rh / 2, is / 5, on ? t->accent : t->accent_text);
            } else icon(ix, ry + (rh - is) / 2, is, e->dir, on ? t->accent_text : e->dir ? t->accent : t->text_dim);
            char right[48];
            if (e->dir) snprintf(right, sizeof right, "%s", "");
            else app_size(e->size, right, sizeof right);
            int rw = ui_text_w(fs, right);
            ui_text(fs, right, mx + W, ry + (rh - fsh) / 2, dc, ALIGN_RIGHT);
            ui_text_fit(fn, e->name, W - is - M(0.03f) - rw - M(0.03f), fit, sizeof fit);
            ui_text(fn, fit, ix + is + M(0.02f), ry + (rh - fnh) / 2, tc, ALIGN_LEFT);
        }
        if (nents > vis) {   /* barra di scorrimento */
            int bh = vis * rh, th = bh * vis / nents, ty = top_y + (bh - th) * first / (nents - vis);
            ui_rrect(UI.w - mx / 2 - M(0.004f), ty, M(0.006f), th, M(0.003f), with_alpha(t->text_dim, 120));
        }
    }
    if (win == W_KB || win == W_JOB) return;   /* quelle finestre hanno la loro barra dei tasti */
    char marks[64] = "";
    int n = marked_count();
    if (n) snprintf(marks, sizeof marks, FT("files.n_selected"), n);
    const char *h[] = { "B", cwd[0] ? T("common.back") : FT("files.quit"), NULL, "X", FT(nents && ents[sel].mark ? "files.unmark" : "files.mark"), "",
                        "Y", FT("files.actions"), NULL, "A", FT(nents && ents[sel].dir ? "files.open" : "files.info"), NULL };
    int end = ui_keybar(h, 12);
    if (n && mx + ui_text_w(fs, marks) + M(0.03f) < end) ui_text(fs, marks, mx, UI.h - ui_keybar_h() / 2 - fsh / 2, t->accent, ALIGN_LEFT);
}

/* ------------------------------------------------------------ ciclo */

static bool quit;

static void list_input(void) {
    input_t *in = &UI.in;
    int top_y, rh, vis = rows_visible(&top_y, &rh), old = sel;
    if (in->pressed[BTN_UP] && nents) sel = (sel + nents - 1) % nents;
    if (in->pressed[BTN_DOWN] && nents) sel = (sel + 1) % nents;
    if (in->pressed[BTN_L] || in->pressed[BTN_LEFT]) sel = sel - vis < 0 ? 0 : sel - vis;
    if (in->pressed[BTN_R] || in->pressed[BTN_RIGHT]) sel = sel + vis >= nents ? (nents ? nents - 1 : 0) : sel + vis;
    if (sel != old) ui_sound(SND_MOVE);
    if (in->pressed[BTN_MENU]) { quit = true; return; }
    if (in->pressed[BTN_B]) {
        ui_sound(SND_BACK);
        if (cwd[0]) go_up(); else quit = true;
        return;
    }
    if (in->pressed[BTN_X] && nents) { ents[sel].mark = !ents[sel].mark; if (sel + 1 < nents) sel++; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_Y]) menu_open();
    if (in->pressed[BTN_A] && nents) {
        if (ents[sel].dir) { ui_sound(SND_OK); go_into(ents[sel].name); }
        else {   /* file: dimensione e data in un avviso */
            char p[1300], q[1600], sz[32], when[48], msg[200];
            full(p, sizeof p, cwd);
            snprintf(q, sizeof q, "%s/%s", p, ents[sel].name);
            struct stat sb;
            if (stat(q, &sb) == 0) {
                struct tm tm;
                localtime_r(&sb.st_mtime, &tm);
                strftime(when, sizeof when, "%d/%m/%Y %H:%M", &tm);
                app_size((long long)sb.st_size, sz, sizeof sz);
                snprintf(msg, sizeof msg, "%s · %s", sz, when);
                ui_toast(msg);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "files")) return 1;
    rename_recover();
    reload(NULL);
    while (!quit) {
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch (sospensione) */
        if (win == W_JOB) {
            if (UI.in.pressed[BTN_B] && job.running) { job.cancel = true; ui_sound(SND_BACK); }
            if (!job.running) {
                if (job_joinable) { pthread_join(job_thread, NULL); job_joinable = false; }
                win = W_NONE;
                if (job.cancel) toast("files.cancelled");
                else if (job.nospace) toast("files.no_space");
                else if (job.failed) toast("files.failed");
                else toast("files.done");
                ui_sound(job.failed || job.cancel ? SND_ERROR : SND_OK);
                for (int i = 0; i < nents; i++) ents[i].mark = false;
                reload(job.kind != JOB_DELETE && job.first[0] ? job.first : NULL);
            }
        } else if (win == W_MENU) {
            input_t *in = &UI.in;
            if (in->pressed[BTN_UP]) { menu_sel = (menu_sel + nmenu - 1) % nmenu; ui_sound(SND_MOVE); }
            if (in->pressed[BTN_DOWN]) { menu_sel = (menu_sel + 1) % nmenu; ui_sound(SND_MOVE); }
            if (in->pressed[BTN_B] || in->pressed[BTN_Y]) { win = W_NONE; ui_sound(SND_BACK); }
            else if (in->pressed[BTN_A]) { ui_sound(SND_OK); action(menu_items[menu_sel]); }
        } else if (win == W_DELETE) {
            int res = ui_confirm_input(&ask_sel);
            if (res >= 0) {
                win = W_NONE;
                if (res == 1) {
                    char (*names)[256];
                    int n = selection(&names);
                    if (n) { job_start(JOB_DELETE, cwd, names, n, NULL); win = W_JOB; }
                    free(names);
                }
            }
        } else if (win == W_KB) kb_input();
        else list_input();

        ui_begin();
        list_draw();
        if (win == W_MENU) menu_draw();
        else if (win == W_KB) kb_draw();
        else if (win == W_JOB) job_draw();
        else if (win == W_DELETE) {
            char title[300], body[300], sz[32];
            int n = marked_count();
            if (n > 1) snprintf(title, sizeof title, FT("files.delete_n"), n);
            else snprintf(title, sizeof title, FT("files.delete_one"), nents ? ents[sel].name : "");
            app_size(del_bytes, sz, sizeof sz);
            snprintf(body, sizeof body, FT("files.delete_body"), del_files, sz);
            ui_confirm_draw(title, body, T("common.cancel"), FT("files.delete"), ask_sel);
        }
        ui_end();
    }
    app_exit_begin();
    if (job_joinable) { job.cancel = true; pthread_join(job_thread, NULL); }
    app_quit();   /* senza exit.txt: quel file è del launcher e del runner */
    return 0;
}
