/* gamewatch: avvia un gioco Linux e lo chiude se si tiene premuto il tasto Menu.

   Uso: gamewatch [--hold SECONDI] [--key CODICE]... -- comando [argomenti]

   Il gioco parte in un proprio gruppo di processi: alla chiusura riceve SIGTERM e,
   se dopo 2 secondi è ancora vivo, SIGKILL, insieme a tutti i processi che ha avviato.
   Su Linux i tasti si leggono da /dev/input/event* senza bloccarli al gioco; i codici
   predefiniti sono BTN_MODE (tasto centrale dei pad) e KEY_HOME. Sugli altri sistemi
   (il Mac del prototipo) il programma si limita ad avviare il gioco e attenderlo.

   Tasti come tastiera: molti giochi Linux per PC usano solo la tastiera. Con la variabile
   GAMEWATCH_KEYS="A=SPACE,B=ESC,START=ENTER,UP=UP" (scritta dal launcher dalla voce "keys"
   di game.json) gamewatch crea una tastiera virtuale (uinput) e preme i tasti indicati
   quando si premono i tasti della console. Croce e levetta sinistra valgono come UP, DOWN,
   LEFT, RIGHT. I codici dei tasti della console seguono lo standard Linux dei gamepad;
   se un controller ne usa altri si correggono con GAMEWATCH_PAD="A=0x131,B=0x130". */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif

/* tasti della console, con i nomi usati in game.json */
enum { P_UP, P_DOWN, P_LEFT, P_RIGHT, P_A, P_B, P_X, P_Y, P_L, P_R, P_L2, P_R2, P_SELECT, P_START, P_COUNT };
static const char *pad_names[P_COUNT] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L", "R", "L2", "R2", "SELECT", "START" };
/* codici evdev: trimui_inputd presenta i tasti come un controller Xbox 360 per posizione
   (verificato sulla Brick Pro): A a destra 305, B in basso 304, X in alto 308, Y a sinistra
   307, come il driver xpad. Croce sul cappello ABS_HAT0, L2/R2 sugli assi 2 e 5. */
static int pad_codes[P_COUNT] = { 0x220, 0x221, 0x222, 0x223, 0x131, 0x130, 0x134, 0x133, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b };
static int key_for[P_COUNT];   /* tasto della tastiera per ogni tasto della console, 0 = nessuno */

/* nome di un tasto della tastiera -> codice KEY_* */
static int key_code(const char *n) {
    static const char *rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const int first[] = { 16, 30, 44 };   /* KEY_Q, KEY_A, KEY_Z */
    static const struct { const char *n; int c; } named[] = {
        { "ESC", 1 }, { "MINUS", 12 }, { "EQUAL", 13 }, { "BACKSPACE", 14 }, { "TAB", 15 }, { "ENTER", 28 }, { "LCTRL", 29 },
        { "SEMICOLON", 39 }, { "APOSTROPHE", 40 }, { "LSHIFT", 42 }, { "COMMA", 51 }, { "DOT", 52 }, { "SLASH", 53 },
        { "RSHIFT", 54 }, { "LALT", 56 }, { "SPACE", 57 }, { "F11", 87 }, { "F12", 88 }, { "RCTRL", 97 }, { "RALT", 100 },
        { "HOME", 102 }, { "UP", 103 }, { "PAGEUP", 104 }, { "LEFT", 105 }, { "RIGHT", 106 }, { "END", 107 }, { "DOWN", 108 },
        { "PAGEDOWN", 109 }, { "INSERT", 110 }, { "DELETE", 111 }, { "PAUSE", 119 },
    };
    if (strlen(n) == 1) {
        if (n[0] >= '1' && n[0] <= '9') return 2 + n[0] - '1';
        if (n[0] == '0') return 11;
        for (int r = 0; r < 3; r++) {
            const char *p = strchr(rows[r], n[0]);
            if (p) return first[r] + (int)(p - rows[r]);
        }
    }
    if (n[0] == 'F' && atoi(n + 1) >= 1 && atoi(n + 1) <= 10) return 58 + atoi(n + 1);   /* KEY_F1 = 59 */
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcmp(named[i].n, n)) return named[i].c;
    return 0;
}

/* "A=SPACE,B=ESC": per ogni coppia chiama fn(indice tasto console, valore) */
static int parse_pairs(const char *spec, void (*fn)(int, const char *)) {
    char buf[1024], *save = NULL;
    int n = 0;
    snprintf(buf, sizeof buf, "%s", spec ? spec : "");
    for (char *it = strtok_r(buf, ", ", &save); it; it = strtok_r(NULL, ", ", &save)) {
        char *eq = strchr(it, '=');
        if (!eq) continue;
        *eq = 0;
        for (char *c = it; *c; c++) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
        for (char *c = eq + 1; *c; c++) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
        int p = -1;
        for (int i = 0; i < P_COUNT; i++)
            if (!strcmp(pad_names[i], it)) p = i;
        if (p < 0) { fprintf(stderr, "gamewatch: tasto della console sconosciuto: %s\n", it); continue; }
        fn(p, eq + 1);
        n++;
    }
    return n;
}

static void set_key(int p, const char *v) {
    key_for[p] = key_code(v);
    if (!key_for[p]) fprintf(stderr, "gamewatch: tasto della tastiera sconosciuto: %s\n", v);
}
static void set_pad(int p, const char *v) { pad_codes[p] = (int)strtol(v, NULL, 0); }

#ifdef __linux__
/* intero da un file di /sys, -1 se non si legge */
static int sys_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}

/* batteria scarica: al 3% senza caricatore in due letture di fila (15 s): una lettura sbagliata per
   un attimo, magari proprio all'avvio, chiudeva il gioco e spegneva (audit 0.9.88, GIO-17) */
static bool batt_empty(int pct, bool usb) {
    static int low;
    low = pct >= 0 && pct <= 3 && !usb ? low + 1 : 0;
    return low >= 2;
}

/* standby automatico dei giochi Linux (23 novembre: fino alla 0.9.112 valeva solo nel launcher e
   nelle app di Mosaik): secondi senza tasti, minuti di Sospensione e Spegnimento automatici (0 =
   spenti) e /tmp/stay_awake, che lo impedisce come sul firmware TrimUI. 0 niente, 1 sospendi,
   2 spegni; prima lo spegnimento, come nelle app (rest_check in ui.c) */
static int idle_action(double idle_s, int suspend_min, int sleep_min, bool stay_awake) {
    if (stay_awake) return 0;
    if (sleep_min > 0 && idle_s > sleep_min * 60.0) return 2;
    if (suspend_min > 0 && idle_s > suspend_min * 60.0) return 1;
    return 0;
}
#endif

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* chiude il gioco e tutti i suoi processi, anche quando il programma principale è già finito:
   al ritorno al menu non deve restare niente in esecuzione */
/* attesa dopo SIGTERM, in passi da 100 ms: 5 s (prima 2), 10 s per gli emulatori con un menu loro (Flycast,
   PPSSPP, mupen64plus chiudono in ordine e scrivono VMU e salvataggi: audit 0.9.72, GIO-06) */
static int stop_steps = 50;   /* 5 s: Flycast e i giochi Linux escono appena possono, l'attesa è solo un limite */
static void stop_group(pid_t pgid) {
    kill(-pgid, SIGTERM);
    kill(-pgid, SIGCONT);   /* un processo fermo dal menu non riceverebbe la richiesta */
    for (int i = 0; i < stop_steps; i++) {
        waitpid(pgid, NULL, WNOHANG);
        if (kill(-pgid, 0) != 0) return;   /* nessuno è rimasto */
        usleep(100 * 1000);
    }
    kill(-pgid, SIGKILL);
}

#ifdef __linux__
/* menu sopra il gioco fermo: programma gamemenu (grafica del launcher in un livello del
   display). Uscita: 0 riprendi, 10 esci dal gioco, 20 spegni, altro = riprendi */
static int run_menu(const char *arg) {
    pid_t m = fork();
    if (m < 0) return 0;
    if (m == 0) {
        execlp("gamemenu", "gamemenu", arg, (char *)NULL);
        _exit(3);
    }
    int st = 0;
    waitpid(m, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}
#endif

#ifdef __linux__
/* sospende la console con il programma già fermo (SIGSTOP); al risveglio il tasto di accensione
   non vale per un momento. Con la SD guasta al risveglio il programma si chiude senza salvare (le
   sue scritture non tornerebbero) e system-init riavvia. */
static void group_sleep(pid_t pid, int ui, double *block_until) {
    fprintf(stderr, "gamewatch: sospensione\n");
    int rc = system("arcade-suspend");
    if (access("/run/sd-guasta", F_OK) == 0) {
        kill(-pid, SIGKILL);
        if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
        _exit(0);
    }
    fprintf(stderr, "gamewatch: risveglio (%s)\n", rc == 0 ? "riuscita" : "non riuscita");
    *block_until = now() + 1.5;
}

/* Scheda tolta a console accesa (m2/guardia-sd.c scrive /run/scheda-tolta, «SCADENZA AZIONE»;
   ARCADE_SD_RUN al posto di /run nelle prove). Il programma gira dalla scheda e scrive lì: si ferma
   (SIGSTOP) e resta fermo finché la scheda non torna. La finestra del launcher qui non c'è (gamemenu
   legge dalla scheda caratteri e lingua): il messaggio lo scrive fbmsg, che è in memoria, e i LED
   rossi li accende la guardia. Accensione breve = sospendi, tenuta = spegni (lo fa la guardia); alla
   scadenza con «sospendi» si sospende. Rimessa la scheda il programma si chiude: riprenderlo
   leggerebbe pagine e file della scheda tolta (cadrebbe, o salverebbe dove non resta niente). */
static void sd_path(char *out, size_t n, const char *name) {
    const char *d = getenv("ARCADE_SD_RUN");
    snprintf(out, n, "%s/%s", d && *d ? d : "/run", name);
}
static bool sd_out_now(void) {
    char p[256];
    sd_path(p, sizeof p, "scheda-tolta");
    return access(p, F_OK) == 0;
}
static void sd_msg(void) {
    pid_t m = fork();
    if (m == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("fbmsg", "fbmsg", "show", "rosso", "0", "SCHEDA TOLTA", "RIMETTILA: IL GIOCO SI CHIUDE E TORNI AL MENU", (char *)NULL);
        _exit(127);
    }
    if (m > 0) waitpid(m, NULL, 0);
}
/* gioco già fermo; torna quando la scheda è di nuovo al suo posto */
static void sd_wait(pid_t pid, int ui, struct pollfd *fds, int nfds, double *block_until) {
    fprintf(stderr, "gamewatch: scheda tolta, gioco fermo\n");
    sd_msg();
    double power_at = 0, acted = -1;
    while (sd_out_now()) {
        if (poll(fds, (nfds_t)nfds, 200) > 0) {
            for (int i = 0; i < nfds; i++) {
                struct input_event ev;
                while (fds[i].fd >= 0 && read(fds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                    if (ev.type != EV_KEY || ev.code != KEY_POWER || now() < *block_until) continue;
                    if (ev.value == 1) power_at = now();
                    else if (ev.value == 0 && power_at) {
                        power_at = 0;
                        group_sleep(pid, ui, block_until);
                        sd_msg();
                    }
                }
            }
        }
        if (power_at && now() - power_at >= 1.2) {
            char p[256];
            sd_path(p, sizeof p, "scheda-spegni-ora");
            FILE *f = fopen(p, "w");
            if (f) fclose(f);
            power_at = 0;
            fprintf(stderr, "gamewatch: scheda tolta, spegnimento chiesto alla guardia\n");
        }
        char p[256], act[16] = "";
        double dl = 0;
        sd_path(p, sizeof p, "scheda-tolta");
        FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%lf %15s", &dl, act) != 2) act[0] = 0; fclose(f); }
        if (!strcmp(act, "sospendi") && now() >= dl && dl != acted) {
            acted = dl;
            fprintf(stderr, "gamewatch: scheda tolta, sospensione alla scadenza\n");
            group_sleep(pid, ui, block_until);
            sd_msg();
        }
    }
}
#endif

int main(int argc, char **argv) {
    double hold = 1.5;
    int keys[8], nkeys = 0, cmd = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { cmd = i + 1; break; }
        if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atof(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc && nkeys < 8) keys[nkeys++] = (int)strtol(argv[++i], NULL, 0);
    }
    if (cmd < 0 || cmd >= argc) {
        fprintf(stderr, "uso: gamewatch [--hold s] [--key codice]... -- comando [argomenti]\n");
        return 2;
    }

    parse_pairs(getenv("GAMEWATCH_PAD"), set_pad);
    int nmapped = parse_pairs(getenv("GAMEWATCH_KEYS"), set_key);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        setsid();   /* nuovo gruppo: si può chiudere il gioco con tutti i suoi figli */
        execvp(argv[cmd], argv + cmd);
        perror("exec");
        _exit(127);
    }

#ifdef __linux__
    if (!nkeys) { keys[nkeys++] = BTN_MODE; keys[nkeys++] = KEY_HOME; }
    /* HOME laterale della Brick Pro (KEY_HOMEPAGE): esce subito, senza tenerlo premuto */
    const int quick_key = KEY_HOMEPAGE;
    struct pollfd fds[32];
    static char fpath[32][300];   /* percorso di ogni dispositivo aperto, per ritrovarlo se sparisce */
    int nfds = 0;
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d)) && nfds < 32) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char name[128] = "";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            fprintf(stderr, "gamewatch: %s \"%s\"\n", path, name);
            snprintf(fpath[nfds], sizeof fpath[0], "%s", path);
            fds[nfds++] = (struct pollfd){ fd, POLLIN, 0 };
        }
    }
    if (d) closedir(d);
    int lost = 0;          /* dispositivi spariti (trimui_inputd riavviato): si riaprono (audit R17) */
    unsigned rescan = 0;
    fprintf(stderr, "gamewatch: %d dispositivi; Menu tenuto %.1f s o HOME per uscire\n", nfds, hold);

    /* tastiera virtuale, creata dopo aver aperto i dispositivi: così non legge sé stessa */
    int ui = -1;
    bool held[P_COUNT] = { 0 };
    struct { int hat_x, hat_y, ax, ay; struct input_absinfo ix, iy; } axes[32];
    memset(axes, 0, sizeof axes);
    for (int i = 0; i < nfds; i++) {
        ioctl(fds[i].fd, EVIOCGABS(ABS_X), &axes[i].ix);
        ioctl(fds[i].fd, EVIOCGABS(ABS_Y), &axes[i].iy);
    }
    if (nmapped) {
        ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (ui < 0) perror("gamewatch: /dev/uinput");
        else {
            ioctl(ui, UI_SET_EVBIT, EV_KEY);
            ioctl(ui, UI_SET_EVBIT, EV_SYN);
            for (int p = 0; p < P_COUNT; p++)
                if (key_for[p]) ioctl(ui, UI_SET_KEYBIT, key_for[p]);
            struct uinput_user_dev dev;
            memset(&dev, 0, sizeof dev);
            snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "gamewatch keyboard");
            dev.id.bustype = BUS_VIRTUAL;
            dev.id.vendor = 0x1209;
            dev.id.product = 0x7457;
            if (write(ui, &dev, sizeof dev) != (ssize_t)sizeof dev || ioctl(ui, UI_DEV_CREATE) < 0) {
                perror("gamewatch: tastiera virtuale");
                close(ui);
                ui = -1;
            } else {
                fprintf(stderr, "gamewatch: tastiera virtuale con %d tasti\n", nmapped);
            }
        }
    }

    /* tasto di accensione (solo nel sistema su SD): breve = sospensione, con il gioco fermo
       (SIGSTOP) finché la console non si risveglia; tenuto premuto 1,2 s = chiude il gioco e
       chiede lo spegnimento (codice di uscita 120 per system-init) */
    bool standalone = getenv("ARCADE_STANDALONE") && getenv("ARCADE_STANDALONE")[0] == '1';
    unlink("/run/gamewatch-sospendi");   /* avanzi di un programma di prima */
    unlink("/run/gamewatch-spegni");
    /* GAMEWATCH_NOMENU=1: il programma ha un suo menu e la sua uscita, e chiuderlo da fuori fa
       perdere dati (Deadcast: salvataggi della VMU ancora nel buffer). Menu tenuto e HOME non fanno
       nulla, e il tasto di accensione sospende senza la domanda (il nostro livello lì non si vede). */
    bool nomenu = getenv("GAMEWATCH_NOMENU") && getenv("GAMEWATCH_NOMENU")[0] == '1';
    /* GAMEWATCH_NOOVERLAY=1 (emulatori che disegnano da soli sullo schermo): il livello della finestra
       del tasto di accensione non si vedrebbe, quindi la pressione breve sospende senza chiedere.
       Le app hanno solo GAMEWATCH_NOMENU: Menu è loro, ma la finestra Sospendi/Riavvia/Spegni sì. */
    bool nooverlay = getenv("GAMEWATCH_NOOVERLAY") && getenv("GAMEWATCH_NOOVERLAY")[0] == '1';
    /* GAMEWATCH_STATES=1 (emulatori con "states": true, il Nintendo 64): Salva e Carica stato nel
       menu; a menu chiuso il gruppo del gioco riceve SIGUSR1 (salva) o SIGUSR2 (carica), che il
       launch.sh ignora e l'emulatore esegue */
    bool states = getenv("GAMEWATCH_STATES") && getenv("GAMEWATCH_STATES")[0] == '1';
    if (nomenu) fprintf(stderr, "gamewatch: senza il menu di Mosaik (si esce dal menu del programma)\n");
    if (nomenu || nooverlay) stop_steps = 100;
    double power_at = 0;
    double power_block_until = 0;   /* dopo un risveglio il tasto non vale per un momento */
    double pressed_at = 0;
    bool open_menu = false;
    /* standby automatico: i minuti li passa il launcher in next.env, solo per i giochi Linux (le app
       di Mosaik lo chiedono da sé, e un'app come un lettore video non si tocca mentre riproduce) */
    int idle_suspend_min = getenv("ARCADE_AUTOSUSPEND_MIN") ? atoi(getenv("ARCADE_AUTOSUSPEND_MIN")) : 0;
    int idle_sleep_min = getenv("ARCADE_AUTOSLEEP_MIN") ? atoi(getenv("ARCADE_AUTOSLEEP_MIN")) : 0;
    double last_input = now();
    pid_t osd_pid = 0;   /* barra del volume in corso */
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (standalone && r != pid && sd_out_now()) {
            if (osd_pid > 0) { kill(osd_pid, SIGTERM); waitpid(osd_pid, NULL, 0); osd_pid = 0; }
            kill(-pid, SIGSTOP);
            sd_wait(pid, ui, fds, nfds, &power_block_until);
            /* scheda rimessa (o la guardia sta riavviando): il gioco si chiude, il launcher lo dice */
            char mark[256];
            sd_path(mark, sizeof mark, "gioco-chiuso-scheda");
            FILE *mf = fopen(mark, "w");
            if (mf) fclose(mf);
            fprintf(stderr, "gamewatch: scheda rimessa, chiudo il gioco\n");
            stop_group(pid);
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return 0;
        }
        if (r == pid) {
            stop_group(pid);   /* il programma principale è finito: restano i processi che ha lanciato */
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (lost && ++rescan % 20 == 0) {   /* ogni 2 s circa, finché manca qualcuno */
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd >= 0) continue;
                int fd = open(fpath[i], O_RDONLY | O_NONBLOCK);
                char name[128] = "";
                if (fd >= 0) ioctl(fd, EVIOCGNAME(sizeof name), name);
                if (fd < 0 || strstr(name, "gamewatch keyboard")) { if (fd >= 0) close(fd); continue; }   /* mai la propria tastiera */
                fds[i].fd = fd;
                ioctl(fd, EVIOCGABS(ABS_X), &axes[i].ix);
                ioctl(fd, EVIOCGABS(ABS_Y), &axes[i].iy);
                lost--;
                fprintf(stderr, "gamewatch: %s \"%s\" riaperto\n", fpath[i], name);
            }
        }
        /* app dello Store ferma da troppo (ui.c, rest_check): sospensione o spegnimento automatici */
        if (standalone && unlink("/run/gamewatch-spegni") == 0) {
            fprintf(stderr, "gamewatch: spegnimento automatico chiesto dal programma\n");
            stop_group(pid);
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return 120;
        }
        if (standalone && unlink("/run/gamewatch-sospendi") == 0) {
            fprintf(stderr, "gamewatch: sospensione automatica chiesta dal programma\n");
            kill(-pid, SIGSTOP);
            group_sleep(pid, ui, &power_block_until);
            kill(-pid, SIGCONT);
            struct input_event skip;
            for (int j = 0; j < nfds; j++) while (fds[j].fd >= 0 && read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
            pressed_at = power_at = 0;
        }
        if (poll(fds, (nfds_t)nfds, 100) > 0) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd >= 0 && (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    /* dispositivo sparito: senza chiuderlo poll tornerebbe subito, all'infinito */
                    fprintf(stderr, "gamewatch: %s sparito, lo cerco di nuovo\n", fpath[i]);
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    lost++;
                    continue;
                }
                if (fds[i].fd < 0) continue;
                struct input_event ev;
                while (read(fds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                    /* qualcuno sta giocando: tasti, croce, grilletti, levette oltre metà corsa (il
                       tremolio delle levette a riposo non conta) */
                    if (ev.type == EV_KEY) last_input = now();
                    else if (ev.type == EV_ABS) {
                        if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) { if (ev.value) last_input = now(); }
                        else if (ev.code == ABS_Z || ev.code == ABS_RZ) { if (ev.value > 64) last_input = now(); }
                        else if (ev.code == ABS_X || ev.code == ABS_Y || ev.code == ABS_RX || ev.code == ABS_RY) {
                            struct input_absinfo ai;
                            if (ioctl(fds[i].fd, EVIOCGABS(ev.code), &ai) == 0) {
                                int mid = (ai.minimum + ai.maximum) / 2, range = (ai.maximum - ai.minimum) / 2;
                                if (range > 0 && (ev.value > mid + range / 2 || ev.value < mid - range / 2)) last_input = now();
                            }
                        }
                    }
                    /* tasti del volume: barra sopra il gioco, che non si ferma */
                    if (standalone && ev.type == EV_KEY && ev.value == 1 && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)) {
                        if (osd_pid > 0 && waitpid(osd_pid, NULL, WNOHANG) == osd_pid) osd_pid = 0;
                        if (osd_pid <= 0) {
                            osd_pid = fork();
                            if (osd_pid == 0) {
                                execlp("gamemenu", "gamemenu", "--volume", ev.code == KEY_VOLUMEUP ? "1" : "-1", (char *)NULL);
                                _exit(3);
                            }
                        }
                    }
                    if (standalone && ev.type == EV_KEY && ev.code == KEY_POWER) {
                        if (now() < power_block_until) power_at = 0;   /* è ancora la pressione che ha svegliato la console */
                        else if (ev.value == 1) power_at = now();
                        else if (ev.value == 0 && power_at) {
                            power_at = 0;
                            if (osd_pid > 0) { kill(osd_pid, SIGTERM); waitpid(osd_pid, NULL, 0); osd_pid = 0; }   /* un livello alla volta */
                            kill(-pid, SIGSTOP);
                            /* chiede conferma: senza un sì non si sospende */
                            int ask = nooverlay ? 30 : run_menu("--ask-suspend");
                            last_input = now();
                            fprintf(stderr, "gamewatch: conferma della sospensione: %d\n", ask);
                            if (ask == 20 || ask == 21) {   /* Spegni o Riavvia dalla finestra */
                                kill(-pid, SIGCONT);
                                stop_group(pid);
                                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                                return ask == 21 ? 121 : 120;
                            }
                            if (ask == 30) group_sleep(pid, ui, &power_block_until);
                            kill(-pid, SIGCONT);
                            /* eventi arrivati durante il sonno: non devono valere dopo */
                            struct input_event skip;
                            for (int j = 0; j < nfds; j++) while (fds[j].fd >= 0 && read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
                            pressed_at = 0;
                            break;
                        }
                    }
                    if (!nomenu && ev.type == EV_KEY && ev.code == quick_key && ev.value == 1) {
                        fprintf(stderr, "gamewatch: HOME, chiudo il gioco\n");
                        stop_group(pid);
                        if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                        return 0;
                    }
                    if (ev.type == EV_KEY && !nomenu)
                        for (int k = 0; k < nkeys; k++)
                            if (ev.code == keys[k]) {
                                if (ev.value != 2) fprintf(stderr, "gamewatch: Menu %s\n", ev.value ? "premuto" : "rilasciato");
                                pressed_at = ev.value ? (pressed_at ? pressed_at : now()) : 0;
                            }
                    if (ui < 0) continue;
                    /* stato dei tasti della console: pulsanti, croce digitale (hat) e levetta */
                    bool want[P_COUNT];
                    memcpy(want, held, sizeof want);
                    if (ev.type == EV_KEY)
                        for (int p = 0; p < P_COUNT; p++)
                            if (ev.code == pad_codes[p]) want[p] = ev.value != 0;
                    if (ev.type == EV_ABS) {
                        if (ev.code == ABS_Z) want[P_L2] = ev.value > 64;
                        if (ev.code == ABS_RZ) want[P_R2] = ev.value > 64;
                        if (ev.code == ABS_HAT0X) axes[i].hat_x = ev.value;
                        if (ev.code == ABS_HAT0Y) axes[i].hat_y = ev.value;
                        if (ev.code == ABS_X || ev.code == ABS_Y) {
                            struct input_absinfo *ai = ev.code == ABS_X ? &axes[i].ix : &axes[i].iy;
                            int mid = (ai->minimum + ai->maximum) / 2, range = (ai->maximum - ai->minimum) / 2;
                            int dir = range > 0 && ev.value > mid + range / 2 ? 1 : range > 0 && ev.value < mid - range / 2 ? -1 : 0;
                            if (ev.code == ABS_X) axes[i].ax = dir;
                            else axes[i].ay = dir;
                        }
                        if (ev.code == ABS_HAT0X || ev.code == ABS_X) {
                            want[P_LEFT] = axes[i].hat_x < 0 || axes[i].ax < 0;
                            want[P_RIGHT] = axes[i].hat_x > 0 || axes[i].ax > 0;
                        }
                        if (ev.code == ABS_HAT0Y || ev.code == ABS_Y) {
                            want[P_UP] = axes[i].hat_y < 0 || axes[i].ay < 0;
                            want[P_DOWN] = axes[i].hat_y > 0 || axes[i].ay > 0;
                        }
                    }
                    for (int p = 0; p < P_COUNT; p++) {
                        if (want[p] == held[p]) continue;
                        held[p] = want[p];
                        if (!key_for[p]) continue;
                        struct input_event out[2];
                        memset(out, 0, sizeof out);
                        out[0].type = EV_KEY;
                        out[0].code = (unsigned short)key_for[p];
                        out[0].value = want[p];
                        out[1].type = EV_SYN;
                        out[1].code = SYN_REPORT;
                        if (write(ui, out, sizeof out) < 0) perror("gamewatch: uinput");
                    }
                }
            }
        }
        if (open_menu) {
            open_menu = false;
            if (osd_pid > 0) { kill(osd_pid, SIGTERM); waitpid(osd_pid, NULL, 0); osd_pid = 0; }   /* un livello alla volta */
            kill(-pid, SIGSTOP);
            int m = run_menu(states ? "--states" : NULL);
            last_input = now();   /* il tempo passato nel menu non conta per lo standby automatico */
            fprintf(stderr, "gamewatch: menu chiuso (%d)\n", m);
            kill(-pid, SIGCONT);
            if (states && (m == 40 || m == 41)) {
                fprintf(stderr, "gamewatch: %s lo stato\n", m == 40 ? "salvo" : "carico");
                kill(-pid, m == 40 ? SIGUSR1 : SIGUSR2);
            }
            if (m == 10 || m == 20 || m == 21) {
                stop_group(pid);
                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                return m == 20 ? 120 : m == 21 ? 121 : 0;
            }
            /* eventi arrivati durante il menu: il gioco non deve vederli come nuovi */
            struct input_event skip;
            for (int j = 0; j < nfds; j++) while (fds[j].fd >= 0 && read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
            continue;
        }
        /* batteria scarica (audit 0.9.72, GIO-08): il runner salva la partita al 4%; qui non si
           può, quindi al 3% senza caricatore si chiude il gioco in ordine (gli emulatori a sé
           scrivono i salvataggi) e si spegne, prima che la console si spenga da sola */
        static double batt_next;
        if (standalone && now() >= batt_next) {
            batt_next = now() + 15;
            int pct = sys_int("/sys/class/power_supply/axp2202-battery/capacity");
            if (batt_empty(pct, sys_int("/sys/class/power_supply/axp2202-usb/online") == 1)) {
                fprintf(stderr, "gamewatch: batteria al %d%%, chiudo il gioco e spengo\n", pct);
                stop_group(pid);
                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                return 120;
            }
        }
        if (standalone && (idle_suspend_min > 0 || idle_sleep_min > 0)) {
            int act = idle_action(now() - last_input, idle_suspend_min, idle_sleep_min, access("/tmp/stay_awake", F_OK) == 0);
            if (act == 2) {
                fprintf(stderr, "gamewatch: %d minuti senza tasti, chiudo il gioco e spengo\n", idle_sleep_min);
                stop_group(pid);
                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                return 120;
            }
            if (act == 1) {
                fprintf(stderr, "gamewatch: %d minuti senza tasti, sospendo\n", idle_suspend_min);
                if (osd_pid > 0) { kill(osd_pid, SIGTERM); waitpid(osd_pid, NULL, 0); osd_pid = 0; }
                kill(-pid, SIGSTOP);
                group_sleep(pid, ui, &power_block_until);
                kill(-pid, SIGCONT);
                struct input_event skip;
                for (int j = 0; j < nfds; j++) while (fds[j].fd >= 0 && read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
                pressed_at = power_at = 0;
                last_input = now();
            }
        }
        if (power_at && now() - power_at >= 1.2) {
            fprintf(stderr, "gamewatch: tasto di accensione tenuto, chiudo il gioco e spengo\n");
            stop_group(pid);
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return 120;
        }
        /* sistema su SD: Menu tenuto 1 s apre il menu sopra il gioco fermo (la pressione breve
           resta al gioco, che può usare quel tasto); altrove tenuto premuto chiude il gioco */
        if (standalone && pressed_at && now() - pressed_at >= 1.0) {
            fprintf(stderr, "gamewatch: tasto Menu tenuto, apro il menu\n");
            pressed_at = 0;
            open_menu = true;
            continue;
        }
        if (pressed_at && now() - pressed_at >= hold) {
            fprintf(stderr, "gamewatch: tasto Menu tenuto, chiudo il gioco\n");
            stop_group(pid);
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return 0;
        }
    }
#else
    (void)hold;
    (void)keys;
    (void)nkeys;
    if (nmapped) fprintf(stderr, "gamewatch: %d tasti come tastiera (solo su Linux)\n", nmapped);
    (void)now;
    (void)stop_group;
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
