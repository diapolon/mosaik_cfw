/* Scarica immagini: lo scraper come app dello Store (Apps/scraper/scraper), aggiornabile senza
   un sistema nuovo. Non ha una sua schermata: lo avvia il launcher (launcher/scrape_client.c), che
   mostra l'avanzamento e può interromperlo. Il lavoro vero è in launcher/scraper.c, uguale a prima.

   Patto con il launcher, versione 1 (cambiarlo vuol dire alzare il numero e il "min_system" del
   pacchetto):
     scraper 1 RADICE INGRESSO STATO [EMULATORI]
   INGRESSO, righe separate da tab:
     emu  id  console(0|1)  thumbnails  nome  indice  estensioni(a|b|c)
     game core  nome  titolo  percorso  mancante(0|1)
   STATO, riscritto intero a ogni cambio: "fase fatti totale scaricate mancanti", fase come
   SCRAPE_* in scraper.h. EMULATORI: id separati da spazi, vuoto = tutti.
   SIGTERM interrompe come il tasto Interrompi. Esce con 0 a lavoro finito, 3 se il servizio non
   risponde, 2 per un errore del patto. */
#include "../launcher/scraper.h"
#include "../launcher/games.h"
#include "../common/durable.h"
#include <SDL.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PATTO 1

emulator_t EMUS[MAX_EMUS];
int NEMUS;

const emulator_t *emulator_find(const char *id) {
    for (int i = 0; i < NEMUS; i++) if (!strcmp(EMUS[i].id, id)) return &EMUS[i];
    return NULL;
}

static FILE *logfile;
void ui_log(const char *fmt, ...) {
    va_list a;
    char t[16];
    time_t now = time(NULL);
    strftime(t, sizeof t, "%H:%M:%S", localtime(&now));
    if (logfile) { fprintf(logfile, "%s ", t); va_start(a, fmt); vfprintf(logfile, fmt, a); va_end(a); fputc('\n', logfile); fflush(logfile); }
    va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr);
}

/* SIGTERM prima che lo scraper parta: scraper_start azzera lo stato, si ripete dopo */
static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; scraper_cancel(); }

/* campo successivo di una riga separata da tab ("" se finita) */
static char *field(char **p) {
    char *s = *p;
    if (!s) return "";
    char *t = strchr(s, '\t');
    if (t) { *t = 0; *p = t + 1; } else *p = NULL;
    s[strcspn(s, "\r\n")] = 0;
    return s;
}

static void write_status(const char *path, const scrape_status_t *s) {
    char tmp[1300], line[96];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int n = snprintf(line, sizeof line, "%d %d %d %d %d\n", s->phase, s->done, s->total, s->saved, s->missing);
    FILE *f = fopen(tmp, "w");
    bool ok = f && fwrite(line, 1, (size_t)n, f) == (size_t)n;
    if (f && fclose(f) != 0) ok = false;
    if (ok) rename(tmp, path); else remove(tmp);   /* lo stato si riscrive spesso: niente fsync */
}

int main(int argc, char **argv) {
    if (argc < 5 || atoi(argv[1]) != PATTO) {
        fprintf(stderr, "uso: scraper %d RADICE INGRESSO STATO [EMULATORI] (patto %d)\n", PATTO, PATTO);
        return 2;
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    sigset_t m;   /* il launcher avvia l'app con SIGTERM bloccato: arriva solo ora, col gestore */
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    const char *root = argv[2], *in = argv[3], *stato = argv[4], *only = argc > 5 ? argv[5] : NULL;
    char lp[1200];
    snprintf(lp, sizeof lp, "%s/Logs/scraper.log", root);
    logfile = fopen(lp, "a");
    FILE *f = fopen(in, "r");
    if (!f) { ui_log("scraper: ingresso %s non leggibile", in); return 2; }
    int cap = 256, n = 0;
    game_t *games = calloc((size_t)cap, sizeof(game_t));
    char *line = NULL;
    size_t ll = 0;
    while (games && getline(&line, &ll, f) > 0) {
        char *p = line, *kind = field(&p);
        if (!strcmp(kind, "emu") && NEMUS < MAX_EMUS) {
            emulator_t *e = &EMUS[NEMUS++];
            memset(e, 0, sizeof *e);
            snprintf(e->id, sizeof e->id, "%s", field(&p));
            e->console = atoi(field(&p)) != 0;
            snprintf(e->thumbs, sizeof e->thumbs, "%s", field(&p));
            snprintf(e->name, sizeof e->name, "%s", field(&p));
            snprintf(e->index, sizeof e->index, "%s", field(&p));
            char *ex = field(&p), *save = NULL;
            for (char *x = strtok_r(ex, "|", &save); x && e->nexts < 8; x = strtok_r(NULL, "|", &save))
                snprintf(e->exts[e->nexts++], sizeof e->exts[0], ".%s", x);
        } else if (!strcmp(kind, "game")) {
            if (n == cap) {
                game_t *g2 = realloc(games, sizeof(game_t) * (size_t)(cap * 2));
                if (!g2) break;
                memset(g2 + cap, 0, sizeof(game_t) * (size_t)cap);
                games = g2;
                cap *= 2;
            }
            game_t *g = &games[n++];
            snprintf(g->core, sizeof g->core, "%s", field(&p));
            snprintf(g->name, sizeof g->name, "%s", field(&p));
            snprintf(g->key, sizeof g->key, "%s", g->name);
            snprintf(g->title, sizeof g->title, "%s", field(&p));
            g->path = strdup(field(&p));
            g->missing = atoi(field(&p)) != 0;
        }
    }
    free(line);
    fclose(f);
    if (!games) { ui_log("scraper: memoria finita"); return 2; }
    ui_log("scraper (app, patto %d): %d emulatori, %d giochi, %s", PATTO, NEMUS, n, only && *only ? only : "tutti");
    SDL_Init(0);
    if (!scraper_start(root, games, n, only)) { ui_log("scraper: non partito"); return 2; }
    if (stop) scraper_cancel();
    const scrape_status_t *s = scraper_status();
    scrape_status_t last = { 0 };
    last.phase = -1;
    while (s->phase != SCRAPE_DONE && s->phase != SCRAPE_FAILED) {
        if (s->phase != last.phase || s->done != last.done || s->saved != last.saved || s->missing != last.missing || s->total != last.total) {
            last.phase = s->phase; last.done = s->done; last.total = s->total; last.saved = s->saved; last.missing = s->missing;
            write_status(stato, s);
        }
        SDL_Delay(100);
    }
    write_status(stato, s);
    int code = s->phase == SCRAPE_DONE ? 0 : 3;
    scraper_join();
    for (int i = 0; i < n; i++) free(games[i].path);
    free(games);
    if (logfile) fclose(logfile);
    return code;
}
