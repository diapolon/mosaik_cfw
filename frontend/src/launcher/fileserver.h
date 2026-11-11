/* Trasferimento di file via Wi-Fi: un piccolo server HTTP dentro il launcher, acceso solo
   mentre la finestra "Trasferisci file" è aperta. Da un computer o da un telefono sulla stessa
   rete si apre l'indirizzo mostrato sulla console, si scrive il PIN e si gestiscono i file della
   cartella Arcade/: caricare ROM, BIOS, immagini, musica e giochi Linux, scaricare salvataggi e
   registri (anche cartelle intere come zip), creare, rinominare ed eliminare.

   Regole:
   - niente fuori da Arcade/, niente nomi che cominciano con un punto, niente System/wifi.conf
     (contiene le password delle reti);
   - si scrive solo dentro Roms, Bios, Saves, Media, Music, Linux e Bezels, mai le cartelle
     principali stesse: il resto (sistema, core, temi, lingue, impostazioni, registri) si può
     solo scaricare;
   - un file caricato si scrive come ".nome.part" e prende il suo nome solo quando è completo e
     già sulla scheda: uno spegnimento a metà non lascia file tronchi;
   - il PIN cambia a ogni apertura; dopo 5 PIN sbagliati si aspetta 30 secondi;
   - un browser può essere ricordato (codice in System/fileserver-browsers.txt, invisibile dalla
     pagina): le volte dopo entra senza PIN, finché dalla console non si dimenticano i browser;
   - la console risponde al nome mosaik.local (mDNS) mentre la finestra è aperta. */
#pragma once
#include <stdbool.h>

enum { FS_IDLE, FS_RECEIVING, FS_SENDING };
enum { FS_EV_UPLOAD, FS_EV_DOWNLOAD, FS_EV_ZIP, FS_EV_DELETE, FS_EV_RENAME, FS_EV_MKDIR, FS_EV_LOGIN, FS_EV_FAIL };

typedef struct {
    bool running;
    int state;               /* FS_IDLE, FS_RECEIVING, FS_SENDING */
    char current[96];        /* file in corso */
    long long done, total;   /* byte del file in corso (total 0: sconosciuto) */
    int nevents;             /* ultime operazioni, la più recente per prima */
    int ev_kind[3];
    char ev_name[3][96];
    int uploads;             /* file caricati da quando è aperto */
} fs_status_t;

/* avvia il server sulla cartella root; restituisce falso se la porta non si apre */
bool fs_start(const char *root);
void fs_stop(void);
void fs_get_status(fs_status_t *out);
const char *fs_pin(void);
int fs_port(void);
/* indirizzo IPv4 della console sulla rete (wlan0 se c'è), "" se non collegata */
void fs_local_ip(char *out, int n);
/* qualcosa è cambiato nella scheda da fs_start: la libreria va riletta */
bool fs_changed(void);
/* browser che non chiedono più il PIN ("Ricorda questo browser"): quanti sono, dimenticarli tutti */
int fs_browsers(void);
void fs_forget_browsers(void);
/* nome fisso sulla rete, per l'indirizzo da mostrare: "mosaik.local" */
#define FS_HOSTNAME "mosaik.local"
