/* Prova M1 sulla console: disegna sul framebuffer e registra i tasti.
     m1test info                stampa le caratteristiche del framebuffer
     m1test show COLORE SEC RIGA1 [RIGA2]   schermata con barre di colore e messaggio
     m1test keys SEC            registra per SEC secondi i tasti premuti (tutti i /dev/input/event*)
   COLORE: blu (in corso), verde (riuscito), rosso (fallito). Compilato statico: gira con
   il solo kernel, senza librerie. */
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "font5x7.h"

static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static uint8_t *fbmem;
static int fbfd = -1;

static int fb_open(void) {
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("/dev/fb0"); return 0; }
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fix)) { perror("ioctl fb"); return 0; }
    fbmem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) { perror("mmap fb"); fbmem = NULL; return 0; }
    ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
    /* retroilluminazione a metà, come fa MinUI (ioctl del driver display Allwinner) */
    int disp = open("/dev/disp", O_RDWR);
    if (disp >= 0) { unsigned long p[4] = { 0, 128, 0, 0 }; ioctl(disp, 0x102, p); close(disp); }
    return 1;
}

static void px(int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= (int)var.xres || y >= (int)var.yres) return;
    uint8_t *p = fbmem + (size_t)(y + var.yoffset) * fix.line_length + (size_t)(x + var.xoffset) * (var.bits_per_pixel / 8);
    uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    uint32_t v = (r >> (8 - var.red.length)) << var.red.offset | (g >> (8 - var.green.length)) << var.green.offset |
                 (b >> (8 - var.blue.length)) << var.blue.offset;
    if (var.transp.length) v |= ((1u << var.transp.length) - 1) << var.transp.offset;
    if (var.bits_per_pixel == 32) *(uint32_t *)p = v;
    else if (var.bits_per_pixel == 16) *(uint16_t *)p = (uint16_t)v;
    else if (var.bits_per_pixel == 24) { p[0] = v & 255; p[1] = v >> 8 & 255; p[2] = v >> 16 & 255; }
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(i, j, c);
}

static void text(const char *s, int x, int y, int scale, uint32_t c) {
    for (; *s; s++, x += 6 * scale) {
        char ch = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        for (size_t k = 0; k < sizeof FONT / sizeof FONT[0]; k++) {
            if (FONT[k].c != ch) continue;
            for (int r = 0; r < 7; r++)
                for (int col = 0; col < 5; col++)
                    if (FONT[k].rows[r] >> (4 - col) & 1) rect(x + col * scale, y + r * scale, scale, scale, c);
        }
    }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

static void show(const char *color, int secs, const char *l1, const char *l2) {
    int W = (int)var.xres, H = (int)var.yres;
    uint32_t bg = !strcmp(color, "verde") ? 0x1b8a3a : !strcmp(color, "rosso") ? 0xb3261e : 0x1d3f8f;
    rect(0, 0, W, H, bg);
    /* barre: rosso, verde, blu, bianco, nero: confermano l'ordine dei colori */
    uint32_t bars[5] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x000000 };
    for (int i = 0; i < 5; i++) rect(i * W / 5, 0, W / 5 + 1, H / 8, bars[i]);
    int sc = W >= 1000 ? 8 : 6;
    /* titolo: FB_TITLE nell'ambiente (M2 usa "ARCADE"), altrimenti quello della prova M1 */
    const char *title = getenv("FB_TITLE") ? getenv("FB_TITLE") : "ARCADE M1";
    text(title, (W - text_w(title, sc)) / 2, H / 4, sc, 0xffffff);
    if (l1) text(l1, (W - text_w(l1, sc / 2)) / 2, H / 2, sc / 2, 0xffffff);
    if (l2) text(l2, (W - text_w(l2, sc / 2)) / 2, H / 2 + 12 * sc / 2, sc / 2, 0xffffff);
    if (secs <= 0) return;
    /* barra del tempo che si svuota */
    for (int t = secs * 10; t >= 0; t--) {
        rect(W / 10, H * 7 / 8, W * 8 / 10, H / 30, 0x000000);
        rect(W / 10, H * 7 / 8, (int)((long)W * 8 / 10 * t / (secs > 0 ? secs * 10 : 1)), H / 30, 0xffffff);
        if (t) usleep(100 * 1000);
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void keys(int secs) {
    struct pollfd fds[32];
    char names[32][128];
    int n = 0;
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 32) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        names[n][0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof names[n]), names[n]);
        struct input_id id;
        ioctl(fd, EVIOCGID, &id);
        printf("dispositivo %s: \"%s\" bus %04x vendor %04x product %04x\n", p, names[n], id.bustype, id.vendor, id.product);
        /* tasti dichiarati: SDL numera i pulsanti di un joystick in quest'ordine
           (prima da BTN_JOYSTICK in su, poi da BTN_MISC a BTN_JOYSTICK) */
        unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
        memset(bits, 0, sizeof bits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits);
        printf("  tasti:");
        for (int k = 0; k < KEY_MAX; k++)
            if (bits[k / (8 * sizeof(long))] >> (k % (8 * sizeof(long))) & 1) printf(" %d", k);
        unsigned long abits[ABS_MAX / (8 * sizeof(long)) + 1];
        memset(abits, 0, sizeof abits);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abits), abits);
        printf("\n  assi:");
        for (int a = 0; a < ABS_MAX; a++)
            if (abits[a / (8 * sizeof(long))] >> (a % (8 * sizeof(long))) & 1) {
                struct input_absinfo ai;
                ioctl(fd, EVIOCGABS(a), &ai);
                printf(" %d[%d..%d]", a, ai.minimum, ai.maximum);
            }
        printf("\n");
        fds[n++] = (struct pollfd){ fd, POLLIN, 0 };
    }
    if (d) closedir(d);
    printf("--- tasti per %d secondi (tipo codice valore) ---\n", secs);
    fflush(stdout);
    double end = now() + secs;
    while (now() < end) {
        if (poll(fds, (nfds_t)n, 200) <= 0) continue;
        for (int i = 0; i < n; i++) {
            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_SYN || ev.type == EV_MSC) continue;
                printf("%.2f %s: %s %d %d (tipo %d)\n", secs - (end - now()), names[i],
                       ev.type == EV_KEY ? "KEY" : ev.type == EV_ABS ? "ABS" : ev.type == EV_SW ? "SW" : "ALTRO", ev.code, ev.value, ev.type);
                /* a schermo: tasti premuti, interruttori e assi oltre metà corsa (grilletti, levette) */
                bool big = ev.type == EV_ABS && (ev.code == ABS_Z || ev.code == ABS_RZ ? ev.value > 128 : ev.code <= ABS_RY && (ev.value > 16000 || ev.value < -16000));
                if (fbmem && ((ev.type == EV_KEY && ev.value == 1) || ev.type == EV_SW || big)) {
                    char m[64];
                    if (ev.type == EV_KEY) snprintf(m, sizeof m, "TASTO %d", ev.code);
                    else if (ev.type == EV_SW) snprintf(m, sizeof m, "INTERRUTTORE %d %d", ev.code, ev.value);
                    else snprintf(m, sizeof m, "ASSE %d %s", ev.code, ev.value > 0 ? "+" : "-");
                    int W = (int)var.xres, H = (int)var.yres;
                    rect(0, H * 3 / 4 - 40, W, 80, 0x1d3f8f);
                    text(m, (W - text_w(m, 6)) / 2, H * 3 / 4 - 21, 6, 0xffff00);
                }
            }
        }
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: m1test info | show COLORE SEC RIGA1 [RIGA2] | keys SEC\n"); return 2; }
    int ok = fb_open();
    if (!strcmp(argv[1], "info")) {
        if (!ok) return 1;
        printf("fb0: %s, %ux%u visibili, %ux%u virtuali, %u bpp, line_length %u, smem %u\n", fix.id, var.xres, var.yres,
               var.xres_virtual, var.yres_virtual, var.bits_per_pixel, fix.line_length, fix.smem_len);
        printf("colori: R %u/%u G %u/%u B %u/%u A %u/%u, rotazione %u\n", var.red.offset, var.red.length, var.green.offset,
               var.green.length, var.blue.offset, var.blue.length, var.transp.offset, var.transp.length, var.rotate);
        return 0;
    }
    if (!strcmp(argv[1], "show") && argc >= 5) {
        if (ok) show(argv[2], atoi(argv[3]), argv[4], argc > 5 ? argv[5] : NULL);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[1], "keys") && argc >= 3) {
        if (ok) {
            int W = (int)var.xres, H = (int)var.yres;
            rect(0, H / 2 - 60, W, H / 2 + 60, 0x1d3f8f);
            text("PREMI TUTTI I TASTI", (W - text_w("PREMI TUTTI I TASTI", 5)) / 2, H / 2, 5, 0xffffff);
        }
        keys(atoi(argv[2]));
        return 0;
    }
    return 2;
}
