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
#include <linux/fb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "font5x7.h"
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

static void stop_group(pid_t pgid) {
    kill(-pgid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (waitpid(pgid, NULL, WNOHANG) == pgid) return;
        usleep(100 * 1000);
    }
    kill(-pgid, SIGKILL);
}

#ifdef __linux__
/* ------------------------------------------------------------------ menu sul framebuffer
   Il gioco è fermo (SIGSTOP): la sua ultima immagine resta nella memoria video (/dev/fb0, la
   parte mostrata è quella a yoffset). Si salva, si scurisce, si disegna il menu con il font
   di fbmsg; all'uscita si rimette l'immagine com'era e il gioco riparte. */
static struct fb_var_screeninfo fbv;
static struct fb_fix_screeninfo fbf;
static uint8_t *fbm;

static void fb_px(int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= (int)fbv.xres || y >= (int)fbv.yres) return;
    uint8_t *p = fbm + (size_t)(y + fbv.yoffset) * fbf.line_length + (size_t)(x + fbv.xoffset) * (fbv.bits_per_pixel / 8);
    uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    uint32_t v = (r >> (8 - fbv.red.length)) << fbv.red.offset | (g >> (8 - fbv.green.length)) << fbv.green.offset |
                 (b >> (8 - fbv.blue.length)) << fbv.blue.offset;
    if (fbv.transp.length) v |= ((1u << fbv.transp.length) - 1) << fbv.transp.offset;
    if (fbv.bits_per_pixel == 32) *(uint32_t *)p = v;
    else if (fbv.bits_per_pixel == 16) *(uint16_t *)p = (uint16_t)v;
}

static void fb_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) fb_px(i, j, c);
}

static void fb_text(const char *s, int x, int y, int sc, uint32_t c) {
    for (; *s; s++, x += 6 * sc) {
        char ch = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        for (size_t k = 0; k < sizeof FONT / sizeof FONT[0]; k++) {
            if (FONT[k].c != ch) continue;
            for (int r = 0; r < 7; r++)
                for (int col = 0; col < 5; col++)
                    if (FONT[k].rows[r] >> (4 - col) & 1) fb_rect(x + col * sc, y + r * sc, sc, sc, c);
        }
    }
}

/* impostazioni del launcher: si leggono e si aggiornano solo volume e luminosità */
static char settings_path[1100];
static int setting_get(const char *key, int def) {
    FILE *f = fopen(settings_path, "r");
    if (!f) return def;
    char line[256], k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    int v = def;
    while (fgets(line, sizeof line, f)) { char *p = strstr(line, k); if (p) { v = atoi(p + strlen(k)); break; } }
    fclose(f);
    return v;
}
static bool setting_lang_it(void) {
    FILE *f = fopen(settings_path, "r");
    if (!f) return true;
    char line[256];
    bool it = true;
    while (fgets(line, sizeof line, f)) if (strstr(line, "\"language\":")) { it = strstr(line, "\"it\"") != NULL; break; }
    fclose(f);
    return it;
}
static void setting_put(const char *key, int v) {
    FILE *f = fopen(settings_path, "r");
    if (!f) return;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char k[64], out[8400];
    snprintf(k, sizeof k, "\"%s\":", key);
    char *p = strstr(buf, k);
    if (!p) return;
    char *e = p + strlen(k);
    while (*e == ' ' || (*e >= '0' && *e <= '9') || *e == '-') e++;
    snprintf(out, sizeof out, "%.*s%s %d%s", (int)(p - buf), buf, k, v, e);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", settings_path);
    f = fopen(tmp, "w");
    if (!f) return;
    fputs(out, f);
    fclose(f);
    rename(tmp, settings_path);
}

static void apply_volume(int level) {   /* come plat_apply del launcher */
    char cmd[160];
    snprintf(cmd, sizeof cmd, "amixer sset 'digital volume' -M %d%% >/dev/null 2>&1", 100 - level * 5);
    if (system(cmd)) {}
}
static void apply_brightness(int level) {
    static const int raw[11] = { 4, 6, 10, 16, 32, 48, 64, 96, 128, 192, 255 };
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return;
    unsigned long param[4] = { 0, (unsigned long)raw[level < 0 ? 0 : level > 10 ? 10 : level], 0, 0 };
    ioctl(fd, 0x102, &param);
    close(fd);
}

enum { MENU_RESUME, MENU_QUIT, MENU_POWEROFF };

/* menu con il gioco fermo; restituisce MENU_* */
static int fb_menu(struct pollfd *fds, int nfds) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("gamewatch: /dev/fb0"); return MENU_RESUME; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &fbv) || ioctl(fd, FBIOGET_FSCREENINFO, &fbf)) { close(fd); return MENU_RESUME; }
    fbm = mmap(NULL, fbf.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbm == MAP_FAILED) { close(fd); return MENU_RESUME; }
    fprintf(stderr, "gamewatch: menu su fb0 %ux%u (virtuale %ux%u, yoffset %u, %u bpp)\n",
            fbv.xres, fbv.yres, fbv.xres_virtual, fbv.yres_virtual, fbv.yoffset, fbv.bits_per_pixel);
    int W = (int)fbv.xres, H = (int)fbv.yres, bpp = (int)fbv.bits_per_pixel / 8;
    size_t rowb = (size_t)W * bpp;
    uint8_t *saved = malloc(rowb * H);
    uint8_t *base = fbm + (size_t)fbv.yoffset * fbf.line_length + (size_t)fbv.xoffset * bpp;
    for (int y = 0; y < H; y++) memcpy(saved + y * rowb, base + (size_t)y * fbf.line_length, rowb);
    /* immagine del gioco scurita, come sfondo del menu */
    if (bpp == 4)
        for (int y = 0; y < H; y++) {
            uint32_t *row = (uint32_t *)(base + (size_t)y * fbf.line_length);
            for (int x = 0; x < W; x++) { uint32_t v = row[x]; row[x] = (v & 0xFF000000u) | ((v >> 2) & 0x3F3F3Fu); }
        }

    bool it = setting_lang_it();
    int vol = setting_get("volume", 12), bri = setting_get("brightness", 8);
    const char *labels_it[5] = { "RIPRENDI", "VOLUME", "LUMINOSITA", "ESCI DAL GIOCO", "SPEGNI" };
    const char *labels_en[5] = { "RESUME", "VOLUME", "BRIGHTNESS", "QUIT GAME", "POWER OFF" };
    const char **labels = it ? labels_it : labels_en;
    int sel = 0, result = -1, sc = W >= 1000 ? 5 : 4;
    int pw = W * 6 / 10, rh = 7 * sc + 4 * sc, ph = 5 * rh + 10 * sc + 7 * sc + 6 * sc;
    int px0 = (W - pw) / 2, py0 = (H - ph) / 2;
    int hx = 0, hy = 0;   /* croce (cappello) */
    bool redraw = true;
    while (result < 0) {
        if (redraw) {
            fb_rect(px0, py0, pw, ph, 0x202530);
            fb_rect(px0, py0, pw, 3 * sc, 0x0FB1D4);
            fb_text("MENU", px0 + 4 * sc, py0 + 5 * sc, sc, 0xFFFFFF);
            for (int i = 0; i < 5; i++) {
                int y = py0 + 10 * sc + 7 * sc + i * rh;
                fb_rect(px0 + 2 * sc, y - 2 * sc, pw - 4 * sc, rh - sc, i == sel ? 0x0FB1D4 : 0x202530);
                uint32_t c = i == sel ? 0x000000 : 0xFFFFFF;
                fb_text(labels[i], px0 + 4 * sc, y, sc, c);
                if (i == 1 || i == 2) {
                    char v[24];
                    snprintf(v, sizeof v, "< %d >", i == 1 ? vol : bri);
                    fb_text(v, px0 + pw - 4 * sc - (int)strlen(v) * 6 * sc, y, sc, c);
                }
            }
            redraw = false;
        }
        if (poll(fds, (nfds_t)nfds, 200) <= 0) continue;
        for (int i = 0; i < nfds && result < 0; i++) {
            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                int up = 0, down = 0, left = 0, right = 0;
                if (ev.type == EV_ABS && ev.code == ABS_HAT0Y) { if (ev.value < 0 && hy >= 0) up = 1; if (ev.value > 0 && hy <= 0) down = 1; hy = ev.value; }
                if (ev.type == EV_ABS && ev.code == ABS_HAT0X) { if (ev.value < 0 && hx >= 0) left = 1; if (ev.value > 0 && hx <= 0) right = 1; hx = ev.value; }
                if (ev.type == EV_KEY && ev.value == 1) {
                    if (ev.code == BTN_DPAD_UP) up = 1;
                    if (ev.code == BTN_DPAD_DOWN) down = 1;
                    if (ev.code == BTN_DPAD_LEFT) left = 1;
                    if (ev.code == BTN_DPAD_RIGHT) right = 1;
                    if (ev.code == pad_codes[P_B] || ev.code == BTN_MODE) result = MENU_RESUME;
                    if (ev.code == pad_codes[P_A]) {
                        if (sel == 0) result = MENU_RESUME;
                        if (sel == 3) result = MENU_QUIT;
                        if (sel == 4) result = MENU_POWEROFF;
                    }
                }
                if (up) { sel = (sel + 4) % 5; redraw = true; }
                if (down) { sel = (sel + 1) % 5; redraw = true; }
                if ((left || right) && (sel == 1 || sel == 2)) {
                    int d = right ? 1 : -1;
                    if (sel == 1) { vol = vol + d < 0 ? 0 : vol + d > 20 ? 20 : vol + d; apply_volume(vol); setting_put("volume", vol); }
                    else { bri = bri + d < 1 ? 1 : bri + d > 10 ? 10 : bri + d; apply_brightness(bri); setting_put("brightness", bri); }
                    redraw = true;
                }
            }
        }
    }
    for (int y = 0; y < H; y++) memcpy(base + (size_t)y * fbf.line_length, saved + y * rowb, rowb);
    free(saved);
    munmap(fbm, fbf.smem_len);
    close(fd);
    fprintf(stderr, "gamewatch: menu chiuso (%s)\n", result == MENU_RESUME ? "riprendi" : result == MENU_QUIT ? "esci" : "spegni");
    return result;
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
    snprintf(settings_path, sizeof settings_path, "%s/System/settings.json", getenv("ROOT") ? getenv("ROOT") : "/mnt/SDCARD/Arcade");
    double power_at = 0;
    double pressed_at = 0;
    bool open_menu = false;
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (poll(fds, (nfds_t)nfds, 100) > 0) {
            for (int i = 0; i < nfds; i++) {
                struct input_event ev;
                while (read(fds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                    if (standalone && ev.type == EV_KEY && ev.code == KEY_POWER) {
                        if (ev.value == 1) power_at = now();
                        else if (ev.value == 0 && power_at) {
                            power_at = 0;
                            fprintf(stderr, "gamewatch: sospensione\n");
                            kill(-pid, SIGSTOP);
                            int rc = system("arcade-suspend");
                            kill(-pid, SIGCONT);
                            fprintf(stderr, "gamewatch: risveglio (%s)\n", rc == 0 ? "riuscita" : "non riuscita");
                            /* eventi arrivati durante il sonno: non devono valere dopo */
                            struct input_event skip;
                            for (int j = 0; j < nfds; j++) while (read(fds[j].fd, &skip, sizeof skip) == (ssize_t)sizeof skip) {}
                            pressed_at = 0;
                            break;
                        }
                    }
                    if (ev.type == EV_KEY && ev.code == quick_key && ev.value == 1) {
                        fprintf(stderr, "gamewatch: HOME, chiudo il gioco\n");
                        stop_group(pid);
                        if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                        return 0;
                    }
                    if (ev.type == EV_KEY)
                        for (int k = 0; k < nkeys; k++)
                            if (ev.code == keys[k]) {
                                if (ev.value != 2) fprintf(stderr, "gamewatch: Menu %s\n", ev.value ? "premuto" : "rilasciato");
                                /* pressione breve (sistema su SD): menu sopra il gioco fermo */
                                if (standalone && ev.value == 0 && pressed_at && now() - pressed_at < hold) open_menu = true;
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
            kill(-pid, SIGSTOP);
            usleep(50 * 1000);   /* l'ultimo fotogramma del gioco è a schermo */
            int m = fb_menu(fds, nfds);
            if (m == MENU_RESUME) kill(-pid, SIGCONT);
            else {
                kill(-pid, SIGCONT);
                stop_group(pid);
                if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
                return m == MENU_POWEROFF ? 120 : 0;
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
