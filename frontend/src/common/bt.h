/* Bluetooth del sistema su SD: il servizio è m2/bt-ctl (chip, D-Bus, bluetoothd, bluealsa,
   abbinamenti sulla SD); qui lo si chiama e se ne legge l'uscita. Sul Mac i dispositivi sono
   simulati, per provare l'interfaccia. Nell'app sul firmware TrimUI e sulla R36S bt_supported()
   è falso. Le funzioni che aspettano il servizio (bt_devices, bt_pair, ...) vanno chiamate da un
   thread: un abbinamento dura qualche secondo. */
#pragma once
#include <stdbool.h>

enum { BT_OFF, BT_STARTING, BT_ON, BT_CONNECTED };
enum { BT_AUDIO, BT_SPEAKER, BT_INPUT, BT_OTHER };   /* cuffie, casse, controller, il resto */

typedef struct {
    char addr[18];
    char name[64];
    int kind;            /* BT_AUDIO cuffie, BT_SPEAKER casse, BT_INPUT controller, BT_OTHER il resto */
    bool paired, connected;
} bt_dev_t;

typedef struct {
    int state;
    int kind;                  /* del dispositivo collegato */
    char addr[18], name[64];   /* dispositivo collegato (BT_CONNECTED) */
} bt_status_t;

bool bt_supported(void);
void bt_enable(bool on);                 /* avvia o ferma il servizio, senza aspettare */
const bt_status_t *bt_status(void);      /* non blocca: l'ultimo stato, riletto ogni 3 s in un thread */
void bt_refresh(void);                   /* rilegge lo stato alla prossima chiamata */
void bt_scan_start(int secs);            /* ricerca in sottofondo; i dispositivi arrivano con bt_devices */
bool bt_scanning(void);
int bt_devices(bt_dev_t *out, int max);  /* abbinati e collegati prima; blocca */
bool bt_pair(const char *addr);          /* abbina, si fida, collega; blocca */
bool bt_connect(const char *addr);
bool bt_disconnect(const char *addr);
bool bt_forget(const char *addr);
