/* opaco: livello dell'interfaccia (canale 1, livello 0) del motore video Allwinner opaco o no.
 *   opaco on    alfa globale 255: si vede tutto quello che il programma disegna
 *   opaco off   alfa per pixel (quella di partenza del firmware)
 * Con il PowerVR GE8300 l'EGL di PPSSPP sceglie una superficie con il canale alfa e il motore
 * video, con l'alfa per pixel, mostra il gioco trasparente: schermo nero con l'audio (prova sulla
 * Brick Pro del 20 novembre). Stessa soluzione di spruceOS ("setalpha 0"), scritta per Mosaik con
 * le strutture di frontend/src/tools/gamemenu.c (include/video/sunxi_display2.h del kernel 4.9).
 * Dalla 0.9.112 è nel sistema (Makefile.tg5040, /usr/arcade/bin/opaco): system-init lo accende per i giochi
 * Linux e le app con "opaque": true e lo spegne dopo ogni gioco Linux. toolchain/build-ppsspp.sh ne mette
 * una copia anche nel pacchetto PPSSPP (Cores/ppsspp/opaco), per i sistemi più vecchi. */
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct disp_rect { int x, y; unsigned int width, height; };
struct disp_rectsz { unsigned int width, height; };
struct disp_rect64 { long long x, y, width, height; };
struct disp_fb_info {
    unsigned long long addr[3];
    struct disp_rectsz size[3];
    unsigned int align[3];
    int format;
    int color_space;
    unsigned int trd_right_addr[3];
    bool pre_multiply;
    struct disp_rect64 crop;
    int flags;
    int scan;
};
struct disp_layer_info {
    int mode;
    unsigned char zorder, alpha_mode, alpha_value;
    struct disp_rect screen_win;
    bool b_trd_out;
    int out_trd_mode;
    union { unsigned int color; struct disp_fb_info fb; };
    unsigned int id;
};
struct disp_layer_config { struct disp_layer_info info; bool enable; unsigned int channel, layer_id; };
#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

int main(int argc, char **argv) {
    bool on = argc == 2 && !strcmp(argv[1], "on");
    if (argc != 2 || (!on && strcmp(argv[1], "off"))) {
        fprintf(stderr, "uso: opaco on|off\n");
        return 2;
    }
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("opaco: /dev/disp"); return 1; }
    struct disp_layer_config c;
    memset(&c, 0, sizeof c);
    c.channel = 1;
    c.layer_id = 0;
    unsigned long arg[4] = { 0, (unsigned long)&c, 1, 0 };
    if (ioctl(fd, DISP_LAYER_GET_CONFIG, arg) < 0) { perror("opaco: lettura del livello"); close(fd); return 1; }
    unsigned char before = c.info.alpha_mode;
    c.info.alpha_mode = on ? 1 : 0;   /* 1 = globale, 0 = per pixel */
    if (on) c.info.alpha_value = 255;
    if (ioctl(fd, DISP_LAYER_SET_CONFIG, arg) < 0) { perror("opaco: scrittura del livello"); close(fd); return 1; }
    close(fd);
    printf("opaco: livello 1.0 alfa %s (prima %d)\n", on ? "globale 255" : "per pixel", before);
    return 0;
}
