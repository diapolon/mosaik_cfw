/* Aggiornamento del sistema via Wi-Fi (solo sistema su SD). L'indirizzo è quello di
   System/update-url.txt se c'è (per le prove: toolchain/update-server.py sul Mac), altrimenti
   quello online del canale scelto (file UPDATE_URL del progetto: bastano file statici, le
   Release di GitHub; il canale sviluppo è la Release "testing"). L'elenco dei file è firmato (ECDSA P-256), quindi chi li
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
    _Atomic int phase, error;
    _Atomic long long done, total;   /* byte elaborati / da elaborare nella fase */
    _Atomic int files;               /* file da aggiornare (in UPD_AVAILABLE: tutti quelli della versione) */
    char version[64];                 /* versione offerta */
    _Atomic bool reinstall;           /* l'area di avvio della scheda è vecchia: conviene riscrivere la SD */
    _Atomic bool cancel;
} update_status_t;

bool update_supported(void);
/* versione in uso: "0.9.0 20261108.1030" sulla console, "0.9.0" altrove */
const char *update_current(void);
void update_set_channel(int development); /* da chiamare prima di update_check */
/* Controllo silenzioso, in un thread: legge solo l'elenco firmato, al massimo una volta al giorno,
   e non tocca la finestra dell'aggiornamento. */
void update_notify_start(const char *root, int development);
/* versione più recente trovata, "" se non c'è nulla di nuovo */
const char *update_notify_version(void);
void update_check(const char *root);      /* cerca la versione e ne controlla la firma, in un thread */
/* dopo UPD_AVAILABLE, in un thread: confronta i file con la scheda (UPD_CHECK), poi li scarica e
   li controlla; UPD_CURRENT se nessun file è cambiato */
void update_install(void);
void update_apply(void);                  /* dopo UPD_READY: installa, in un thread */
void update_discard(void);                /* dopo UPD_READY: cancella i file scaricati */
void update_cancel(void);
const update_status_t *update_status(void);
void update_join(void);                   /* attende la fine del thread */
/* il launcher è arrivato alla prima schermata: il sistema nuovo funziona */
void update_confirm_boot(const char *root);
