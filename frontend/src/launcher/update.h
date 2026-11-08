/* Aggiornamento del sistema via Wi-Fi (solo sistema su SD). La console cerca prima un Mac in
   rete locale (toolchain/update-server.py, messaggio broadcast), poi l'indirizzo di
   System/update-url.txt, poi quello online (file UPDATE_URL del progetto: bastano file statici,
   per esempio le Release di GitHub). L'elenco dei file è firmato (ECDSA P-256), quindi chi li
   consegna non conta;
   i file si scaricano con nome provvisorio, si rileggono dalla SD e si controlla l'impronta
   SHA-256: solo se tutti sono giusti prendono il posto dei vecchi. Il sistema precedente resta
   come arcade-system.squashfs.prec: se il nuovo non parte, system-init torna a quello. */
#pragma once
#include <stdbool.h>

enum { UPD_IDLE, UPD_SEARCH, UPD_CHECK, UPD_NO_SERVER, UPD_CURRENT, UPD_AVAILABLE,
       UPD_DOWNLOAD, UPD_VERIFY, UPD_READY, UPD_INSTALL, UPD_DONE, UPD_FAILED };
enum { UPE_NONE, UPE_NETWORK, UPE_SIGNATURE, UPE_MANIFEST, UPE_SPACE, UPE_VERIFY, UPE_WRITE, UPE_CANCEL };

typedef struct {
    volatile int phase, error;
    volatile long long done, total;   /* byte elaborati / da elaborare nella fase */
    volatile int files;               /* file da aggiornare */
    char version[64];                 /* versione offerta */
    volatile bool cancel;
} update_status_t;

bool update_supported(void);
/* versione in uso: "0.9.0 20261108.1030" sulla console, "0.9.0" altrove */
const char *update_current(void);
void update_check(const char *root);      /* cerca e controlla, in un thread */
void update_install(void);                /* dopo UPD_AVAILABLE: scarica e controlla, in un thread */
void update_apply(void);                  /* dopo UPD_READY: installa, in un thread */
void update_discard(void);                /* dopo UPD_READY: cancella i file scaricati */
void update_cancel(void);
const update_status_t *update_status(void);
void update_join(void);                   /* attende la fine del thread */
/* il launcher è arrivato alla prima schermata: il sistema nuovo funziona */
void update_confirm_boot(const char *root);
