/* Wi-Fi del sistema su SD: il servizio è m2/wifi-ctl (driver, wpa_supplicant, udhcpc, ora da
   internet); qui si parla con wpa_supplicant tramite wpa_cli. Sul Mac le reti sono simulate,
   per provare l'interfaccia. Nell'app sul firmware TrimUI il Wi-Fi lo gestisce il firmware e
   wifi_supported() è falso. */
#pragma once
#include <stdbool.h>

enum { WIFI_OFF, WIFI_DISCONNECTED, WIFI_CONNECTING, WIFI_CONNECTED };

typedef struct {
    char ssid[33];       /* nome della rete (UTF-8) */
    int bars;            /* 0..3 dal segnale */
    bool secured;        /* serve una password */
    bool saved;          /* già in System/wifi.conf */
    bool current;        /* rete a cui si è collegati */
    bool failed;         /* ultimo tentativo fallito (password sbagliata?) */
} wifi_net_t;

typedef struct {
    int state;
    char ssid[33];
    char ip[16];
    int bars;
} wifi_status_t;

bool wifi_supported(void);
bool wifi_starting(void);                        /* sta ancora partendo: l'icona lampeggia */
void wifi_enable(bool on);                       /* avvia o ferma il servizio, senza aspettare */
const wifi_status_t *wifi_status(bool refresh);  /* refresh: rilegge subito; altrimenti ogni 2 s */
void wifi_scan(void);                            /* chiede una ricerca; i risultati arrivano dopo */
int wifi_networks(wifi_net_t *out, int max);     /* ordinate: collegata, salvate, segnale */
bool wifi_psk_valid(const char *psk);   /* 8-63 caratteri stampabili */
bool wifi_connect(const char *ssid, const char *psk);   /* psk NULL: rete aperta o salvata */
bool wifi_forget(const char *ssid);
