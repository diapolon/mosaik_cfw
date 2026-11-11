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

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* chiude il gioco e tutti i suoi processi, anche quando il programma principale è già finito:
   al ritorno al menu non deve restare niente in esecuzione */
static void stop_group(pid_t pgid) {
    kill(-pgid, SIGTERM);
    kill(-pgid, SIGCONT);   /* un processo fermo dal menu non riceverebbe la richiesta */
    for (int i = 0; i < 20; i++) {
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
            fds[nfds++] = (struct pollfd){ fd, POLLIN, 0 };
        }
    }
    if (d) closedir(d);
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
    /* GAMEWATCH_NOMENU=1: il programma ha un suo menu e la sua uscita, e chiuderlo da fuori fa
       perdere dati (Deadcast: salvataggi della VMU ancora nel buffer). Menu tenuto e HOME non fanno
       nulla, e il tasto di accensione sospende senza la domanda (il nostro livello lì non si vede). */
    bool nomenu = getenv("GAMEWATCH_NOMENU") && getenv("GAMEWATCH_NOMENU")[0] == '1';
    if (nomenu) fprintf(stderr, "gamewatch: senza il menu di Mosaik (si esce dal menu del programma)\n");
    double power_at = 0;
    double power_block_until = 0;   /* dopo un risveglio il tasto non vale per un momento */
    double pressed_at = 0;
    bool open_menu = false;
    pid_t osd_pid = 0;   /* barra del volume in corso */
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            stop_group(pid);   /* il programma principale è finito: restano i processi che ha lanciato */
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (poll(fds, (nfds_t)nfds, 100) > 0) {
            for (int i = 0; i < nfds; i++) {
                struct input_event ev;
                while (read(fds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
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
                            int ask = nomenu ? 30 : run_menu("--ask-suspend");
                            fprintf(stderr, "gamewatch: conferma della sospensione: %d\n", ask);
                            if (ask == 20) {
                                kill(-pid, SIGCONT);
                                stop_group(pid);
                                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                                return 120;
                            }
                            if (ask == 30) {
                                fprintf(stderr, "gamewatch: sospensione\n");
                                int rc = system("arcade-suspend");
                                fprintf(stderr, "gamewatch: risveglio (%s)\n", rc == 0 ? "riuscita" : "non riuscita");
                                power_block_until = now() + 1.5;
                            }
                            kill(-pid, SIGCONT);
                            /* eventi arrivati durante il sonno: non devono valere dopo */
                            struct input_event skip;
                            for (int j = 0; j < nfds; j++) while (read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
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
            int m = run_menu(NULL);
            fprintf(stderr, "gamewatch: menu chiuso (%d)\n", m);
            kill(-pid, SIGCONT);
            if (m == 10 || m == 20) {
                stop_group(pid);
                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                return m == 20 ? 120 : 0;
            }
            /* eventi arrivati durante il menu: il gioco non deve vederli come nuovi */
            struct input_event skip;
            for (int j = 0; j < nfds; j++) while (read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
            continue;
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
