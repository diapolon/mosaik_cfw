/* Scarica immagini dal launcher: il lavoro lo fa l'app dello Store Apps/scraper/scraper
   (src/apps/scraper.c, che contiene launcher/scraper.c), aggiornabile senza un sistema nuovo.
   Qui c'è la stessa interfaccia di scraper.h: si scrive l'elenco dei giochi in un file, si avvia
   l'app e si legge l'avanzamento dal file di stato che riscrive. Patto: vedi src/apps/scraper.c.
   ARCADE_SCRAPER_APP indica un altro programma (Mac e prove). */
#include "scraper.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <SDL.h>

#define PATTO "1"

void ui_log(const char *fmt, ...);

static scrape_status_t st;
static pid_t child = -1;
static Uint32 last_read;
static char in_path[1100], st_path[1100];

void scraper_app_path(const char *root, char *out, size_t n) {
    const char *env = getenv("ARCADE_SCRAPER_APP");
    if (env && *env) snprintf(out, n, "%s", env);
    else snprintf(out, n, "%s/Apps/scraper/scraper", root);
}

bool scraper_installed(const char *root) {
    char p[1100];
    scraper_app_path(root, p, sizeof p);
    struct stat s;
    return stat(p, &s) == 0 && S_ISREG(s.st_mode);
}

/* niente tab né a capo nei campi: romperebbero le righe */
static void put(FILE *f, const char *s) {
    for (; s && *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
}

static bool write_input(const game_t *games, int n) {
    FILE *f = fopen(in_path, "w");
    if (!f) return false;
    for (int e = 0; e < NEMUS; e++) {
        const emulator_t *m = &EMUS[e];
        fprintf(f, "emu\t");
        put(f, m->id);
        fprintf(f, "\t%d\t", m->console ? 1 : 0);
        put(f, m->thumbs); fputc('\t', f);
        put(f, m->name); fputc('\t', f);
        put(f, m->index); fputc('\t', f);
        for (int x = 0; x < m->nexts; x++) { if (x) fputc('|', f); put(f, m->exts[x][0] == '.' ? m->exts[x] + 1 : m->exts[x]); }
        fputc('\n', f);
    }
    for (int i = 0; i < n; i++) {
        const game_t *g = &games[i];
        if (!strcmp(g->core, "linux") || g->missing) continue;
        fprintf(f, "game\t");
        put(f, g->core); fputc('\t', f);
        put(f, g->name); fputc('\t', f);
        put(f, g->title); fputc('\t', f);
        put(f, g->path ? g->path : ""); fprintf(f, "\t0\n");
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool scraper_start(const char *root, const game_t *games, int n, const char *only) {
    if (child > 0) return false;
    memset(&st, 0, sizeof st);
    char app[1100];
    scraper_app_path(root, app, sizeof app);
    snprintf(in_path, sizeof in_path, "%s/System/.scraper-in.txt", root);
    snprintf(st_path, sizeof st_path, "%s/System/.scraper-stato.txt", root);
    remove(st_path);
    if (!write_input(games, n)) { ui_log("immagini: %s non scrivibile", in_path); return false; }
    /* Interrompi subito dopo l'avvio: il segnale aspetta che l'app sia pronta a riceverlo. La
       maschera, messa prima del fork, passa all'app, che la toglie dopo aver messo il gestore */
    sigset_t m, old;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    sigprocmask(SIG_BLOCK, &m, &old);
    pid_t p = fork();
    if (p != 0) sigprocmask(SIG_SETMASK, &old, NULL);
    if (p < 0) { ui_log("immagini: fork non riuscito (%s)", strerror(errno)); return false; }
    if (p == 0) {
        for (int fd = 3; fd < 1024; fd++) close(fd);   /* niente socket, audio o schermo del launcher */
        execl(app, app, PATTO, root, in_path, st_path, only ? only : "", (char *)NULL);
        _exit(127);
    }
    child = p;
    st.phase = SCRAPE_INFO;
    last_read = 0;
    ui_log("immagini: avviata l'app %s (pid %d)", app, (int)p);
    return true;
}

void scraper_cancel(void) {
    st.cancel = true;
    if (child > 0) kill(child, SIGTERM);
}

/* last: l'app è uscita. Prima la fase finale non si mostra: la finestra direbbe "finito" con
   l'app ancora al lavoro */
static void read_status(bool last) {
    FILE *f = fopen(st_path, "r");
    if (!f) return;
    int ph, d, t, s, m;
    if (fscanf(f, "%d %d %d %d %d", &ph, &d, &t, &s, &m) == 5 && ph >= SCRAPE_IDLE && ph <= SCRAPE_FAILED) {
        if (last || (ph != SCRAPE_DONE && ph != SCRAPE_FAILED)) st.phase = ph;
        st.done = d; st.total = t; st.saved = s; st.missing = m;
    }
    fclose(f);
}

static void finish(int code) {
    child = -1;
    remove(in_path);
    read_status(true);
    /* lo stato finale l'ha scritto l'app; se manca o l'app non è uscita bene, è andata male */
    if (code != 0 || st.phase != SCRAPE_DONE) st.phase = SCRAPE_FAILED;
    if (code != 0) ui_log("immagini: l'app è finita con %d", code);
}

const scrape_status_t *scraper_status(void) {
    if (child > 0 && SDL_GetTicks() - last_read >= 200) {
        last_read = SDL_GetTicks();
        int ws;
        pid_t r = waitpid(child, &ws, WNOHANG);
        if (r == child) finish(WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + (WIFSIGNALED(ws) ? WTERMSIG(ws) : 0));
        else if (r < 0) finish(126);
        else read_status(false);
    }
    return &st;
}

void scraper_join(void) {
    if (child <= 0) return;
    kill(child, SIGTERM);
    int ws;
    for (int i = 0; i < 50; i++) {   /* 5 secondi per chiudere, poi si ferma a forza */
        if (waitpid(child, &ws, WNOHANG) == child) { finish(WIFEXITED(ws) ? WEXITSTATUS(ws) : 128); return; }
        SDL_Delay(100);
    }
    kill(child, SIGKILL);
    waitpid(child, &ws, 0);
    finish(137);
}
