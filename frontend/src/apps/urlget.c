/* Scarica file: app di Mosaik, pacchetto dello Store (Arcade/Apps/urlget).

   Uno strumento generico per portare sulla scheda file da un indirizzo web: un file diretto o un
   "item" di archive.org (se ne legge l'elenco, si scelgono i file, se ne controllano dimensione
   e hash). Non ha fonti né elenchi suoi: gli indirizzi li scrive l'utente in Apps/urlget/links.txt
   (uno per riga, # per i commenti), dal computer con il trasferimento file o a mano. Si sceglie
   l'indirizzo, i file e la cartella di arrivo (una cartella di Roms, oppure Bios); il resto
   (HTTPS soltanto, niente indirizzi privati, ".part" e controlli) è in urlget_core.c.
   Chi scarica risponde di ciò che scarica: servono file che si ha il diritto di avere (homebrew,
   pubblico dominio, copie proprie, traduzioni, demo).

   Sul Mac: build/urlget --root sdcard --app package/apps/urlget */
#include "appkit.h"
#include "urlget_core.h"
#include <curl/curl.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

enum { ST_LINKS, ST_LOADING, ST_PICK, ST_DEST, ST_RUN, ST_DONE };
static int st = ST_LINKS;
static char cacert[1200];

/* ------------------------------------------------------------ indirizzi */

#define MAXLINKS 200
static char *links[MAXLINKS];
static int nlinks, link_sel, link_skipped;

static char *links_path(char *out, size_t n) {
    snprintf(out, n, "%s/links.txt", app_dir);
    return out;
}

static void links_load(void) {
    for (int i = 0; i < nlinks; i++) free(links[i]);
    nlinks = link_skipped = 0;
    char p[1300], line[2300];
    FILE *f = fopen(links_path(p, sizeof p), "r");
    if (!f) {   /* primo avvio: un file di esempio, con le istruzioni e nessun indirizzo */
        f = fopen(p, "w");
        if (f) {
            fputs("# One address per line. Lines starting with # are ignored.\n"
                  "# Un indirizzo per riga. Le righe che cominciano con # si ignorano.\n"
                  "#\n# https://example.org/file.zip\n# https://archive.org/download/<item>/\n", f);
            fclose(f);
        }
        return;
    }
    while (fgets(line, sizeof line, f) && nlinks < MAXLINKS) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        s[strcspn(s, "\r\n")] = 0;
        for (size_t l = strlen(s); l && (s[l - 1] == ' ' || s[l - 1] == '\t'); ) s[--l] = 0;
        if (!*s || *s == '#') continue;
        ug_link_t t;
        if (ug_classify(s, &t) == UG_BAD) { link_skipped++; continue; }
        links[nlinks++] = strdup(s);
    }
    fclose(f);
    if (link_sel >= nlinks) link_sel = nlinks ? nlinks - 1 : 0;
}

/* ------------------------------------------------------------ elenco dei file di un item */

static ug_link_t lnk;
static char direct_url[2100];
static ug_file_t *files;
static int nfiles;
static bool *chk;
static int *vis, nvis;
static char exts[17][16];
static int next_ext = 1, filt;   /* exts[0] = tutti */
static int pick_sel, pick_scroll;

static atomic_bool ld_cancel, ld_done;
static atomic_int ld_err;
static pthread_t ld_thr;
static bool ld_thr_on;

static bool ld_progress(void *u, long long d, long long t) { (void)u; (void)d; (void)t; return atomic_load(&ld_cancel); }

static void *ld_worker(void *arg) {
    (void)arg;
    char *json = NULL;
    int r = ug_fetch(lnk.meta, cacert, UG_META_MAX, &json, ld_progress, NULL);
    if (r == UG_OK) {
        nfiles = ug_parse_meta(json, &files);
        if (!nfiles) r = UG_E_SIZE;   /* niente file utili: "nessun file" */
    }
    free(json);
    atomic_store(&ld_err, r);
    atomic_store(&ld_done, true);
    return NULL;
}

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d && strlen(d) <= 12 ? d : "";
}

static void rebuild_vis(void) {
    free(vis);
    vis = malloc(sizeof *vis * (size_t)(nfiles ? nfiles : 1));
    nvis = 0;
    for (int i = 0; vis && i < nfiles; i++)
        if (!filt || !strcasecmp(ext_of(files[i].name), exts[filt])) vis[nvis++] = i;
    if (pick_sel >= nvis) pick_sel = nvis ? nvis - 1 : 0;
}

static void pick_init(void) {
    free(chk);
    chk = calloc((size_t)(nfiles ? nfiles : 1), sizeof *chk);
    snprintf(exts[0], sizeof exts[0], "*");
    next_ext = 1;
    filt = pick_sel = pick_scroll = 0;
    for (int i = 0; i < nfiles && next_ext < 17; i++) {
        const char *e = ext_of(files[i].name);
        int k = 1;
        while (k < next_ext && strcasecmp(exts[k], e)) k++;
        if (k == next_ext && *e) snprintf(exts[next_ext++], sizeof exts[0], "%s", e);
    }
    rebuild_vis();
}

/* ------------------------------------------------------------ cartella di arrivo */

#define MAXDEST 80
static char dests[MAXDEST][96];
static int ndests, dest_sel;
static char dest_dir[1300];

static int cmp_str(const void *a, const void *b) { return strcasecmp(a, b); }

static void dests_load(void) {
    ndests = 0;
    char p[1300];
    snprintf(p, sizeof p, "%s/Roms", app_root);
    DIR *d = opendir(p);
    for (struct dirent *e; d && (e = readdir(d)) && ndests < MAXDEST - 1;) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 90) continue;
        char q[1500];
        struct stat sb;
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        if (stat(q, &sb) == 0 && S_ISDIR(sb.st_mode)) snprintf(dests[ndests++], sizeof dests[0], "Roms/%s", e->d_name);
    }
    if (d) closedir(d);
    qsort(dests, (size_t)ndests, sizeof dests[0], cmp_str);
    snprintf(dests[ndests++], sizeof dests[0], "Bios");
    if (dest_sel >= ndests) dest_sel = 0;
}

/* ------------------------------------------------------------ scaricamento */

static struct {
    atomic_bool cancel, finished;
    atomic_llong done, total;
    atomic_int idx, n, ok, skipped, failed, last_err;
} job;
static int *queue;
static pthread_t job_thr;
static bool job_thr_on;

static bool job_progress(void *u, long long d, long long t) {
    (void)u;
    atomic_store(&job.done, d);
    if (t > 0) atomic_store(&job.total, t);
    return atomic_load(&job.cancel);
}

static void *job_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < job.n && !atomic_load(&job.cancel); i++) {
        const ug_file_t *f = &files[queue[i]];
        char url[2200];
        if (lnk.kind == UG_ITEM) ug_item_url(&lnk, f->path, url, sizeof url);
        else snprintf(url, sizeof url, "%s", direct_url);
        atomic_store(&job.idx, i);
        atomic_store(&job.done, 0);
        atomic_store(&job.total, f->size);
        int r = ug_download(url, dest_dir, f->name, f->size, f->sha1, f->md5, cacert, job_progress, NULL);
        ui_log("urlget: %s -> %s: %d", f->name, dest_dir, r);
        if (r == UG_OK) job.ok++;
        else if (r == UG_E_EXISTS) job.skipped++;
        else if (r == UG_E_CANCEL) break;
        else { job.failed++; atomic_store(&job.last_err, r); }
    }
    atomic_store(&job.finished, true);
    return NULL;
}

static void run_start(void) {
    snprintf(dest_dir, sizeof dest_dir, "%s/%s", app_root, dests[dest_sel]);
    free(queue);
    queue = malloc(sizeof *queue * (size_t)(nfiles ? nfiles : 1));
    int n = 0;
    for (int i = 0; queue && i < nfiles; i++) if (chk[i]) queue[n++] = i;
    if (!queue || !n) { ui_toast(FT("urlget.none_selected")); ui_sound(SND_ERROR); return; }
    mkdir(dest_dir, 0755);
    job.cancel = job.finished = false;
    job.done = job.total = job.idx = job.ok = job.skipped = job.failed = job.last_err = 0;
    job.n = n;
    if (pthread_create(&job_thr, NULL, job_worker, NULL) != 0) { ui_toast(FT("urlget.e_io")); ui_sound(SND_ERROR); return; }
    job_thr_on = true;
    st = ST_RUN;
    ui_sound(SND_OK);
}

/* ------------------------------------------------------------ azioni */

static void free_files(void) {
    free(files);
    free(chk);
    free(vis);
    free(queue);
    files = NULL;
    chk = NULL;
    vis = NULL;
    queue = NULL;
    nfiles = nvis = 0;
}

static const char *err_key(int r) {
    switch (r) {
    case UG_E_URL: return "urlget.e_url";
    case UG_E_NET: return "urlget.e_net";
    case UG_E_SPACE: return "urlget.e_space";
    case UG_E_SIZE: return "urlget.e_size";
    case UG_E_HASH: return "urlget.e_hash";
    case UG_E_BIG: return "urlget.e_big";
    default: return "urlget.e_io";
    }
}

static void open_link(void) {
    if (!nlinks) return;
    if (!UI.set.wifi) { ui_toast(FT("urlget.wifi_off")); ui_sound(SND_ERROR); return; }
    free_files();
    const char *u = links[link_sel];
    if (ug_classify(u, &lnk) == UG_BAD) { ui_toast(FT("urlget.e_url")); ui_sound(SND_ERROR); return; }
    if (lnk.kind == UG_ITEM) {
        ld_cancel = ld_done = false;
        if (ld_thr_on) { pthread_join(ld_thr, NULL); ld_thr_on = false; }
        if (pthread_create(&ld_thr, NULL, ld_worker, NULL) != 0) return;
        ld_thr_on = true;
        st = ST_LOADING;
        ui_sound(SND_OK);
        return;
    }
    /* file diretto: un solo file, scelto già */
    snprintf(direct_url, sizeof direct_url, "%s", u);
    files = calloc(1, sizeof *files);
    if (!files || !ug_url_name(u, files[0].name, sizeof files[0].name)) { ui_toast(FT("urlget.e_url")); ui_sound(SND_ERROR); free_files(); return; }
    nfiles = 1;
    chk = calloc(1, sizeof *chk);
    if (!chk) return;
    chk[0] = true;
    dests_load();
    st = ST_DEST;
    ui_sound(SND_OK);
}

static void loading_poll(void) {
    if (!atomic_load(&ld_done)) return;
    pthread_join(ld_thr, NULL);
    ld_thr_on = false;
    int r = atomic_load(&ld_err);
    if (r == UG_OK && nfiles > 0) { pick_init(); st = ST_PICK; return; }
    if (r != UG_E_CANCEL) { ui_toast(FT(r == UG_E_SIZE ? "urlget.no_files" : err_key(r))); ui_sound(SND_ERROR); }
    st = ST_LINKS;
}

static long long selected_bytes(int *count) {
    long long b = 0;
    int n = 0;
    for (int i = 0; i < nfiles; i++) if (chk[i]) { n++; b += files[i].size; }
    *count = n;
    return b;
}

/* ------------------------------------------------------------ schermate */

typedef struct { int mx, W, top, bottom, rh, vis; } lay_t;

static lay_t layout(void) {
    theme_t *t = &UI.th;
    lay_t l;
    l.mx = M(t->margin);
    l.W = UI.w - 2 * l.mx;
    l.rh = TTF_FontHeight(UI.f_normal) + M(0.016f);
    l.top = M(t->topbar_h) + M(0.02f);
    l.bottom = UI.h - ui_keybar_h() - M(0.01f);
    l.vis = (l.bottom - l.top) / l.rh;
    if (l.vis < 1) l.vis = 1;
    return l;
}

static void row_bg(const lay_t *l, int y, bool on) {
    if (on) ui_rrect(l->mx - M(0.01f), y, l->W + M(0.02f), l->rh - 2, M(UI.th.panel_radius), UI.th.accent);
}

static void draw_links(void) {
    theme_t *t = &UI.th;
    ui_topbar(FT("urlget.title"));
    lay_t l = layout();
    if (!nlinks) {
        char lines[6][160], p[1300];
        int n = ui_wrap(UI.f_normal, FT("urlget.no_links"), l.W, lines, 4);
        for (int i = 0; i < n; i++) ui_text(UI.f_normal, lines[i], l.mx, l.top + i * l.rh, t->text, ALIGN_LEFT);
        snprintf(p, sizeof p, "Apps/urlget/links.txt");
        ui_text(UI.f_small, p, l.mx, l.top + (n + 1) * l.rh, t->accent, ALIGN_LEFT);
    }
    int first = link_sel >= l.vis ? link_sel - l.vis + 1 : 0;
    char fit[300];
    for (int i = first; i < nlinks && i < first + l.vis; i++) {
        int y = l.top + (i - first) * l.rh;
        bool on = i == link_sel;
        row_bg(&l, y, on);
        const char *u = links[i];
        const char *s = strstr(u, "://");
        ui_text_fit(UI.f_normal, s ? s + 3 : u, l.W, fit, sizeof fit);
        ui_text(UI.f_normal, fit, l.mx, y + M(0.006f), on ? t->accent_text : t->text, ALIGN_LEFT);
    }
    if (link_skipped) {
        char m[160];
        snprintf(m, sizeof m, FT("urlget.skipped_lines"), link_skipped);
        ui_text(UI.f_small, m, l.mx, l.bottom - TTF_FontHeight(UI.f_small), t->text_dim, ALIGN_LEFT);
    }
    const char *h[] = { "B", FT("urlget.quit"), NULL, "A", FT("urlget.open"), NULL };
    ui_keybar(h, 6);
}

static void draw_loading(void) {
    ui_topbar(FT("urlget.title"));
    lay_t l = layout();
    ui_text(UI.f_title, FT("urlget.loading"), l.mx, l.top + M(0.04f), UI.th.text, ALIGN_LEFT);
    const char *h[] = { "B", FT("urlget.cancel"), NULL };
    ui_keybar(h, 3);
}

static void draw_pick(void) {
    theme_t *t = &UI.th;
    char head[200], a[32];
    int cnt;
    long long sb = selected_bytes(&cnt);
    app_size(sb, a, sizeof a);
    snprintf(head, sizeof head, "%s  ·  %s  ·  %d/%d  ·  %s", FT("urlget.pick"), filt ? exts[filt] : FT("urlget.all_types"), cnt, nfiles, a);
    ui_topbar(head);
    lay_t l = layout();
    if (pick_sel < pick_scroll) pick_scroll = pick_sel;
    if (pick_sel >= pick_scroll + l.vis) pick_scroll = pick_sel - l.vis + 1;
    char fit[300], sz[32];
    int box = TTF_FontHeight(UI.f_small);
    for (int k = pick_scroll; k < nvis && k < pick_scroll + l.vis; k++) {
        int i = vis[k], y = l.top + (k - pick_scroll) * l.rh;
        bool on = k == pick_sel;
        row_bg(&l, y, on);
        color_t fg = on ? t->accent_text : t->text;
        ui_rborder(l.mx, y + (l.rh - box) / 2, box, box, 3, 2, fg);
        if (chk[i]) ui_rrect(l.mx + 4, y + (l.rh - box) / 2 + 4, box - 8, box - 8, 2, fg);
        if (files[i].size > 0) app_size(files[i].size, sz, sizeof sz); else sz[0] = 0;
        int sw = ui_text_w(UI.f_small, sz) + M(0.02f);
        ui_text_fit(UI.f_normal, files[i].name, l.W - box - M(0.02f) - sw, fit, sizeof fit);
        ui_text(UI.f_normal, fit, l.mx + box + M(0.015f), y + M(0.006f), fg, ALIGN_LEFT);
        ui_text(UI.f_small, sz, l.mx + l.W, y + M(0.01f), on ? t->accent_text : t->text_dim, ALIGN_RIGHT);
    }
    if (!nvis) ui_text(UI.f_normal, FT("urlget.no_files"), l.mx, l.top, t->text_dim, ALIGN_LEFT);
    const char *h[] = { "B", FT("urlget.back"), NULL, "A", FT("urlget.toggle"), NULL, "X", FT("urlget.all"), NULL, "Y", FT("urlget.filter"), NULL, "START", FT("urlget.next"), NULL };
    ui_keybar(h, 15);
}

static void draw_dest(void) {
    theme_t *t = &UI.th;
    ui_topbar(FT("urlget.dest"));
    lay_t l = layout();
    int first = dest_sel >= l.vis ? dest_sel - l.vis + 1 : 0;
    for (int i = first; i < ndests && i < first + l.vis; i++) {
        int y = l.top + (i - first) * l.rh;
        bool on = i == dest_sel;
        row_bg(&l, y, on);
        ui_text(UI.f_normal, dests[i], l.mx, y + M(0.006f), on ? t->accent_text : t->text, ALIGN_LEFT);
    }
    struct statvfs vs;
    char fr[32], m[200];
    if (statvfs(app_root, &vs) == 0) {
        app_size((long long)vs.f_bavail * (long long)vs.f_frsize, fr, sizeof fr);
        snprintf(m, sizeof m, FT("urlget.free"), fr);
        ui_text(UI.f_small, m, l.mx + l.W, l.bottom - TTF_FontHeight(UI.f_small), t->text_dim, ALIGN_RIGHT);
    }
    const char *h[] = { "B", FT("urlget.back"), NULL, "A", FT("urlget.download"), NULL };
    ui_keybar(h, 6);
}

static void draw_run(void) {
    theme_t *t = &UI.th;
    ui_topbar(FT("urlget.title"));
    lay_t l = layout();
    int i = atomic_load(&job.idx);
    long long done = atomic_load(&job.done), tot = atomic_load(&job.total);
    char fit[300], a[32], b[32], line[300];
    int y = l.top + M(0.03f);
    snprintf(line, sizeof line, "%d / %d", i + 1 > job.n ? job.n : i + 1, job.n);
    ui_text(UI.f_small, line, l.mx, y, t->text_dim, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_small) + M(0.01f);
    ui_text_fit(UI.f_title, files[queue[i < job.n ? i : 0]].name, l.W, fit, sizeof fit);
    ui_text(UI.f_title, fit, l.mx, y, t->text, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_title) + M(0.03f);
    int bh = M(0.02f);
    ui_rrect(l.mx, y, l.W, bh, bh / 2, with_alpha(t->text_dim, 60));
    if (tot > 0) ui_rrect(l.mx, y, (int)((long long)l.W * (done < tot ? done : tot) / tot), bh, bh / 2, t->accent);
    y += bh + M(0.025f);
    app_size(done, a, sizeof a);
    app_size(tot, b, sizeof b);
    snprintf(line, sizeof line, tot > 0 ? "%s / %s" : "%s", a, b);
    ui_text(UI.f_normal, line, l.mx, y, t->text, ALIGN_LEFT);
    const char *h[] = { "B", FT("urlget.stop"), NULL };
    ui_keybar(h, 3);
}

static void draw_done(void) {
    theme_t *t = &UI.th;
    ui_topbar(FT("urlget.title"));
    lay_t l = layout();
    int y = l.top + M(0.03f);
    char line[300];
    snprintf(line, sizeof line, FT(job.ok == 1 ? "urlget.done_ok1" : "urlget.done_ok"), (int)job.ok);
    ui_text(UI.f_title, line, l.mx, y, t->text, ALIGN_LEFT);
    y += TTF_FontHeight(UI.f_title) + M(0.02f);
    if (job.skipped) {
        snprintf(line, sizeof line, FT(job.skipped == 1 ? "urlget.done_skipped1" : "urlget.done_skipped"), (int)job.skipped);
        ui_text(UI.f_normal, line, l.mx, y, t->text_dim, ALIGN_LEFT);
        y += l.rh;
    }
    if (job.failed) {
        snprintf(line, sizeof line, FT(job.failed == 1 ? "urlget.done_failed1" : "urlget.done_failed"), (int)job.failed, FT(err_key(job.last_err)));
        ui_text(UI.f_normal, line, l.mx, y, rgba(220, 70, 70, 255), ALIGN_LEFT);
        y += l.rh;
    }
    if (job.cancel) ui_text(UI.f_normal, FT("urlget.cancelled"), l.mx, y, t->text_dim, ALIGN_LEFT);
    const char *h[] = { "A", FT("urlget.ok"), NULL };
    ui_keybar(h, 3);
}

/* ------------------------------------------------------------ ingresso */

static void move(int *sel, int n, input_t *in, int page) {
    if (n <= 0) { *sel = 0; return; }
    if (in->pressed[BTN_DOWN]) { *sel = *sel + 1 < n ? *sel + 1 : 0; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_UP]) { *sel = *sel > 0 ? *sel - 1 : n - 1; ui_sound(SND_MOVE); }
    if (in->pressed[BTN_R]) *sel = *sel + page < n ? *sel + page : n - 1;
    if (in->pressed[BTN_L]) *sel = *sel > page ? *sel - page : 0;
}

int main(int argc, char **argv) {
    if (!app_start(argc, argv, "urlget")) return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    snprintf(cacert, sizeof cacert, "%s/System/cacert.pem", app_root);
    links_load();
    bool quit = false;
    while (!quit) {
        ui_poll();
        if (UI.in.quit) break;
        UI.in.power = false;   /* il tasto di accensione lo gestisce gamewatch */
        UI.busy = st == ST_LOADING || st == ST_RUN;
        input_t *in = &UI.in;
        lay_t l = layout();
        bool back = in->pressed[BTN_B] || in->pressed[BTN_MENU], ok = in->pressed[BTN_A];
        switch (st) {
        case ST_LINKS:
            move(&link_sel, nlinks, in, l.vis);
            if (back) quit = true;
            else if (ok) open_link();
            break;
        case ST_LOADING:
            if (back) atomic_store(&ld_cancel, true);
            loading_poll();
            break;
        case ST_PICK:
            move(&pick_sel, nvis, in, l.vis);
            if (back) { free_files(); st = ST_LINKS; ui_sound(SND_BACK); }
            else if (ok && nvis) { chk[vis[pick_sel]] = !chk[vis[pick_sel]]; ui_sound(SND_MOVE); }
            else if (in->pressed[BTN_X] && nvis) {   /* tutti i visibili: se lo sono già tutti, nessuno */
                bool all = true;
                for (int k = 0; k < nvis; k++) if (!chk[vis[k]]) all = false;
                for (int k = 0; k < nvis; k++) chk[vis[k]] = !all;
                ui_sound(SND_OK);
            } else if (in->pressed[BTN_Y] && next_ext > 1) { filt = (filt + 1) % next_ext; pick_sel = pick_scroll = 0; rebuild_vis(); ui_sound(SND_MOVE); }
            else if (in->pressed[BTN_START]) {
                int cnt;
                selected_bytes(&cnt);
                if (!cnt) { ui_toast(FT("urlget.none_selected")); ui_sound(SND_ERROR); }
                else { dests_load(); st = ST_DEST; ui_sound(SND_OK); }
            }
            break;
        case ST_DEST:
            move(&dest_sel, ndests, in, l.vis);
            if (back) { if (lnk.kind == UG_ITEM) st = ST_PICK; else { free_files(); st = ST_LINKS; } ui_sound(SND_BACK); }
            else if (ok) run_start();
            break;
        case ST_RUN:
            if (back) atomic_store(&job.cancel, true);
            if (atomic_load(&job.finished)) { pthread_join(job_thr, NULL); job_thr_on = false; st = ST_DONE; ui_sound(job.failed ? SND_ERROR : SND_OK); }
            break;
        case ST_DONE:
            if (ok || back) { free_files(); st = ST_LINKS; ui_sound(SND_BACK); }
            break;
        }
        ui_begin();
        switch (st) {
        case ST_LINKS: draw_links(); break;
        case ST_LOADING: draw_loading(); break;
        case ST_PICK: draw_pick(); break;
        case ST_DEST: draw_dest(); break;
        case ST_RUN: draw_run(); break;
        default: draw_done(); break;
        }
        ui_end();
    }
    atomic_store(&ld_cancel, true);
    atomic_store(&job.cancel, true);
    app_exit_begin();
    if (ld_thr_on) pthread_join(ld_thr, NULL);
    if (job_thr_on) pthread_join(job_thr, NULL);
    free_files();
    for (int i = 0; i < nlinks; i++) free(links[i]);
    curl_global_cleanup();
    app_quit();
    return 0;
}
