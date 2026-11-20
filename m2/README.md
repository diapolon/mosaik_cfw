# M2: sistema Arcade su SD (Brick Pro TG4040, Smart Pro TG5040)

La console si accende direttamente nel launcher, senza il sistema TrimUI e senza toccare la
memoria interna. Si toglie la scheda e riparte il sistema originale. Il sistema si chiama
Mosaik; la cartella `Arcade`, `arcade-system.squashfs` e gli altri nomi interni restano
(cambiarli romperebbe le schede già installate e gli aggiornamenti). L'etichetta della SD è
MOSAIK dalla 0.9.6; l'initramfs accetta anche ARCADE, quella delle schede precedenti.

**Due modelli, un solo sistema.** `arcade-system.squashfs` è unico: una parte comune
(BusyBox, launcher, runner, glibc) e uno strato per modello in `/fw/tg4040` e `/fw/tg5040`
con ciò che nei firmware è compilato a parte (moduli e librerie della GPU, SDL2, ALSA, udev,
`trimui_inputd`, driver Wi-Fi) più `hw-init.sh` (GPIO come `runtrimui.sh`: sulla Smart Pro
anche 110 e 114, l'alimentazione delle schede delle levette). L'initramfs del modello collega
`/run/fw` allo strato giusto. Le immagini sono due perché l'area di avvio (boot0, U-Boot con
la configurazione dell'hardware, kernel) è propria di ogni console e il chip la legge da una
posizione fissa; il contenuto (cartella `Arcade`, ROM, salvataggi) è lo stesso e si sposta da
una scheda all'altra.

- `dist/m2-smartpro.img` per la Smart Pro, `dist/m2-brickpro.img` per la Brick Pro: entrambe provate sulla console.

## Com'è fatta la scheda (2,3 GB)

| Parte | Contenuto |
|---|---|
| boot0, boot_package | originali del firmware v1.1.1, modalità normale |
| partizione `boot` | kernel originale 4.9.191 + nostro initramfs (`m2/initramfs-init`, con `sdgrow`, `fsck.fat`, `axpctl`) |
| `env`, `env-redund` | ambiente di U-Boot originale, con `loglevel=4` |
| FAT "MOSAIK" (2,2 GB, poi tutta la scheda) | cartella `Arcade` (temi, lingue, filtri, core, indici, ROM, salvataggi, log), `LEGGIMI.txt`, `bootlogo.bmp` |
| `Arcade/System/arcade-system.squashfs` (circa 30 MB) | il sistema: BusyBox, librerie di base, GPU, audio, Wi-Fi, `trimui_inputd`, `amixer`/`tinymix` del firmware, i nostri programmi, `m2/system-init` |

Avvio:
1. **initramfs**: alimenta il watchdog, trova la FAT (controllo con `fsck.fat` se serve,
   allargamento con `sdgrow` alla prima accensione), scrive la generazione dell'area di avvio
   in `Arcade/System/area-avvio.txt`, sceglie il sistema (nuovo, di prova o `.prec`), monta lo
   squashfs e gli passa il controllo (`switch_root`). Se qualcosa manca lo dice a schermo
   (rosso) e lo scrive in `Arcade/Logs/initramfs.txt`.
2. **system-init**: carica i moduli della GPU (`pvrsrvkm`, `dc_sunxi`), avvia udev e
   `trimui_inputd`, imposta l'audio come il firmware, controlla i nomi corti (`fatnomi`), copia
   i crash del kernel (pstore) in `Logs/crash`, rimette a posto il chip di alimentazione
   (`axpctl restore`), separa le interruzioni sulla Brick Pro, accende il Wi-Fi se attivo,
   scrive `Arcade/Logs/sistema.txt` e avvia il ciclo launcher/runner.
3. **launcher/runner**: "Spegni" spegne davvero (`poweroff -f`, oppure `axpctl poweroff` col
   caricatore). Tasti extra della Brick Pro: HOME torna al launcher, F1/F2 salvano e
   caricano, FN = prestazioni massime.

Per aggiornare il sistema basta copiare dal Mac un nuovo `arcade-system.squashfs` (e i file
della cartella `Arcade`) sulla scheda, o usare l'aggiornamento Wi-Fi: non serve riscriverla.
L'area di avvio invece arriva solo riscrivendo l'immagine: a ogni suo cambio si alza
`GENERAZIONE` in `m2/area-avvio.conf` (oggi 4, dalla 0.9.74; schede senza file = 0), e con
`RISCRIVERE_SOTTO` sopra 0 la finestra dell'aggiornamento invita le schede più vecchie a
riscrivere la SD.

## Provare

1. Scrivi l'immagine del modello su una microSD con balenaEtcher.
2. Dal Mac copia qualche ROM in `Arcade/Roms/FBNEO` (e `neogeo.zip` in `Arcade/Bios`).
3. Accendi la console con la scheda.
4. Leggi i file di `Arcade/Logs`: `passi.txt`, `avvio.log`, `sistema.txt`, `Arcade.log`, `wifi.log`.

Prova del Wi-Fi a ripetizione: `Arcade/System/prova-wifi.txt` con `DRIVER GIRI [led-spenti|led-fissi]
[irq-separati] [senza-pausa]` (accensioni da zero del Wi-Fi) oppure `sessione MINUTI [URL]`
(Wi-Fi acceso una volta, ping e uno scaricamento ogni 3 minuti). `system-init` rinomina il file
e lancia `m2/prova-wifi` prima del launcher; ogni passo va in `Logs/prova-wifi.txt` con sync, i
messaggi del kernel in `Logs/prova-wifi-kernel.txt`. Il `wget` di BusyBox non fa HTTPS: per la
sessione serve un URL http, per esempio `python3 -m http.server` sul Mac.

## Ricostruire

```bash
sh m2/build-m2.sh        # wrapper; il lavoro è in m2/build-in-container.sh
```

Per aggiungere un modello: estrarne il firmware con `toolchain/analyze-firmware.sh`, scrivere
`m2/hw-init-<id>.sh` e aggiungere una riga a `MODELS` in `m2/build-in-container.sh`.

## Limiti

- Il FAT arriva al massimo a 256 GB. Una scheda scritta con un'immagine di prima del 7
  novembre ha il FAT da 1,9 GB non preparato a crescere: la partizione si allarga, il FAT no.
  Per avere tutto lo spazio va riscritta con l'immagine nuova (prima salva ROM e salvataggi).
- Bluetooth solo per l'audio (cuffie e casse, dalla 0.9.88): niente controller. I giochi
  Linux e gli emulatori a sé non seguono le cuffie collegate o scollegate durante l'uso.
- Calibrazione di fabbrica delle levette: è nella memoria interna. Si calibra dalle
  Impostazioni, oppure si copia `joypad.config` dal sistema originale in `Arcade/System`.
- Ogni modello ha la sua immagine, con i suoi pezzi di avvio: per altre console ne servirà una nuova.

## Storia delle prove hardware

**6 novembre, prima prova.** Schermata di avvio e poi riavvio, nessun log: lo script di avvio
del sistema (`/sbin/init`) era nello squashfs senza permesso di esecuzione, il passaggio al
sistema falliva e il kernel riavviava. Corretto (e verificato durante la costruzione). Da
allora ogni passo dell'avvio va a schermo e in `Arcade/Logs/passi.txt` con `sync` immediato, i
messaggi del kernel si salvano prima di caricare la GPU, e se il launcher non parte compare un
errore rosso invece di un riavvio.

**6 novembre, seconda prova.** Avvio completo fino al launcher, GPU riconosciuta (PowerVR,
framebuffer 1024×768), ma la SDL2 di TrimUI non partiva: `Failed loading libudev.so.0`. Ora il
sistema include `udevd`, `udevadm`, `libudev` e le regole del firmware e li avvia come
`/etc/init.d/udev` (trigger e settle) prima del launcher; c'è anche un `setterm` vuoto che la
SDL2 chiama.

**7 novembre, spazio della SD e data/ora.** Alla prima accensione la partizione si allarga da
sola fino alla fine della scheda (`m2/sdgrow.c`, provato con `m2/test-sdgrow.sh` su schede
simulate da 2,3 GB a 300 GB e letto dal Mac). Il FAT dell'immagine nasce con la tabella dei
cluster già grande per 256 GB (cluster da 32 KB, `m2/fatprep.py`): allargarlo vuol dire solo
cambiare il numero di settori, senza spostare dati. `fsck.fat` parte solo se all'ultimo
spegnimento il FAT non è stato chiuso bene: con la tabella grande ogni controllo legge decine
di MB. "Data e ora" scrive anche l'orologio della console in UTC (`hwclock -w -u`); il fuso
orario è una regola POSIX nella variabile TZ, con l'ora legale. Allo spegnimento si fermano i
programmi con file aperti sulla SD (`trimui_inputd` tiene aperto il suo log), altrimenti il FAT
non si chiude. La pulizia dei file di macOS (`._*`) parte 20 s dopo l'avvio.

**7 novembre, Wi-Fi.** Entrambe le console hanno il chip XR829 (driver `xradio_mac`,
`xradio_core`, `xradio_wlan`, compilati per il kernel di ogni modello, nello strato
`/fw/<id>/modules`; il firmware del chip è in `/lib/firmware`). `m2/wifi-ctl on|off` carica il
driver e avvia `wpa_supplicant` 2.6 del firmware (controllo in `/run/wpa`, reti in
`Arcade/System/wifi.conf`) e `wpa_cli -a m2/wifi-event`: a ogni connessione `udhcpc` chiede
l'indirizzo (`m2/udhcpc-script`, `resolv.conf` in `/run`) e alla prima `ntpd` prende l'ora e la
scrive nell'RTC. Il launcher (`frontend/src/common/wifi.c`) usa `wpa_cli` per stato, ricerca,
collegamento e reti da dimenticare. Log in `Arcade/Logs/wifi.log`. Servono anche `libnss_dns` e
`libnss_files`: senza, la glibc non risolve i nomi.

**7 novembre, LED e sospensione.** LED dal driver `led_anim` (zone `lr`, `m`, `f1`, `f2`,
`rear` se esistono; `max_scale` = livello × 4 come il firmware). Tasto di accensione: breve =
sospensione con `m2/arcade-suspend` (Wi-Fi e LED spenti, `/tmp/system_suspend` per
`trimui_inputd`, `wakeup_count` e `echo mem > /sys/power/state` come `/sbin/suspendtomem.sh`;
al risveglio audio, LED, Wi-Fi, luminosità e volume riapplicati); tenuto premuto = spegnimento
(in gioco salva prima).

**7 novembre, avvio più rapido e logo (Smart Pro).** Dall'avvio del kernel alla prima
schermata del launcher: da 9,6 a 4,9 s. Kernel 3,7 → 2,1 s con `loglevel=4` nell'ambiente di
U-Boot (`m2/envtool` lo scrive nella copia di riserva `env-redund`, con verifica): con 8 ogni
messaggio andava sulla porta seriale. Moduli GPU in parallelo con udev e tasti; audio con la
configurazione ricordata in una sola chiamata `amixer -s` (2,2 → 0,07 s). Logo: `bootlogo.bmp`
nella FAT (U-Boot lo legge da "boot-resource", come il logo TrimUI; 396x66, 32 bit Smart Pro /
24 bit Brick Pro, da `m2/mklogo.py`) e lo stesso logo con `fbmsg logo` fino al launcher.

**8 novembre, aggiornamento via Wi-Fi.** Il numero sta in `VERSION`; la build aggiunge la data
(`/etc/arcade-version`, per esempio `0.9.0 20261108.1010`) e la console accetta solo versioni
più recenti. La console usa `System/update-url.txt` se c'è (prove con
`python3 toolchain/update-server.py` sul Mac: `http://INDIRIZZO-DEL-MAC:8765`), altrimenti il
canale scelto: Stabile (`UPDATE_URL`, compilato nel launcher) o Sviluppo (la Release `testing`).
Bastano file statici: `update.txt`, `update.sig`, `file-0`, `file-1`...
(`update-server.py --out CARTELLA`). L'elenco è firmato ECDSA P-256 con la chiave privata in
`~/.config/arcade-update/update-key.pem`, **da conservare**: senza, le console già in giro non
accettano aggiornamenti; la pubblica è in `frontend/src/launcher/update_pubkey.h`. I file si
scaricano come `.nuovo`, si rileggono dalla SD e si controlla lo SHA-256; poi si sostituiscono,
il sistema per ultimo. Passano solo i file del sistema e gli indici arcade (mai ROM,
salvataggi, impostazioni, dati dello scraper). Si scarica a qualsiasi carica, si installa sotto
il 30% solo col caricatore. Il sistema precedente resta come `.prec`: l'initramfs conta gli
avvii di prova in `System/update-pending`, che il launcher toglie alla prima schermata, e alla
terza prova fallita rimette il precedente. `system-init` conta solo sulle schede con
l'initramfs più vecchio. Prove: `m2/test-update.sh`, `m2/test-boot.sh`.

**10 novembre, Brick Pro: blocco all'accensione del Wi-Fi (issue #1, trimui/firmware_brickpro#4).**
Pochi secondi dopo l'avvio di `wpa_supplicant`, durante la prima ricerca delle reti, la Brick Pro
si bloccava e ripartiva (panico del kernel in `sunxi_i2c_handler` dopo `[i2c3] xfer timeout
(dev addr:0x28)`, come nel rapporto TrimUI). `trimui_inputd` legge le levette dal bus I2C 3 con circa
1.030 interruzioni al secondo, Wi-Fi acceso o spento; sulla Smart Pro quel bus è disattivato e
la stessa prova dà 100 accensioni su 100. Di base tutte le interruzioni (`twi3`, `sunxi-mmc1`,
`xradio_irq`, `ledcirq`) vanno sul processore 0. Correzione: a ogni avvio `twi3` → CPU3,
`sunxi-mmc1` → CPU1, `ledcirq` → CPU2 (`/proc/irq/N/smp_affinity`, maschere 8, 2, 4;
`xradio_irq` non si sposta), e `trimui_inputd` fermo con SIGSTOP dal caricamento del driver al
collegamento (al massimo 10 s, `m2/wifi-ctl`). I LED non c'entrano (41 interruzioni al secondo,
accesi o spenti). Risultati: senza correzioni 6 blocchi in 189 accensioni; interruzioni separate
1 in 156; con anche la pausa 0 in 700 (200 + 500 in un solo avvio di 75 minuti), più 80 minuti
di sessione col Wi-Fi acceso (60 con ping, 20 con 217 MB scaricati) senza blocchi.

**10-11 novembre, caricatore collegato (issue #2).** Col caricatore collegato sia il riavvio
sia lo spegnimento lasciavano la console a schermo nero: il chip AXP2202 la riaccendeva e
U-Boot del firmware entrava nella sua modalità di sola carica (`charge_mode`,
`bat\battery_charge.bmp`, `androidboot.mode=charger`) senza mostrare nulla; serviva volume
giù + accensione, poi accensione. Copiare nella partizione boot-resource le immagini di carica
del firmware (`bat/*.bmp`, `magic.bin`) non ha cambiato nulla. Prima dello spegnimento il chip
aveva un'interruzione in sospeso (`0x4A = 0x08`) e `0x22 = 0x1E`. Correzione (`m2/axpctl.c`,
statico, come `poweroff_next` di NextUI): `axpctl poweroff` disattiva le interruzioni del chip
(0x40-0x44 = 0), le cancella (0x48-0x4C = 0xFF), poi 0x22 = 0x0A e 0x27 = 0x01. Lo usano, solo
con `axp2202-usb/online` = 1, `finish` e `halt_with` di `system-init` e l'initramfs (errori e
spegnimento dopo l'allargamento della SD, solo immagini nuove); un riavvio col caricatore
diventa uno spegnimento con messaggio. Dopo `axpctl poweroff` il chip ricorda 0x22 = 0x0E
anche alla riaccensione, e senza il bit 4 un riavvio diventa uno spegnimento anche senza
caricatore: `axpctl restore` lo rimette a ogni avvio. Provato su Smart Pro e Brick Pro: col
caricatore la console resta spenta e il solo tasto la riaccende.

**11 novembre, sospensione con l'altoparlante in muto (Smart Pro).** Sospendendo mentre lo
scraper scaricava (o con l'elenco delle reti Wi-Fi aperto) la console restava sulla finestra
con i tasti morti. Con l'altoparlante in muto (`arcade-suspend mute`, che il launcher usa
mentre lavora su SD o Wi-Fi) l'uscita audio si ferma e `SDL_CloseAudioDevice` aspettava per
sempre; la sospensione la sblocca. Correzione (`ui_suspend` in `common/ui.c`): chiusura in un
thread con limite di 3 s, poi la sospensione prosegue e al risveglio l'audio si riapre.

**11 novembre, aggiornamento interrotto (R03, issue #3, Smart Pro).** Con `prova-pausa 30`
nell'elenco firmato (solo per le prove: `MOSAIK_PROVA_PAUSA=30` a `update-server.py`)
l'installazione si ferma con i dati nuovi e il sistema vecchio; spegnimento forzato in quei
30 s. All'avvio dopo `system-init` ha scritto "aggiornamento interrotto durante
l'installazione" e "rimetto i file della versione precedente": lingue della build di prima,
nessun giornale né copia `.annulla` rimasti (restava il sistema `.nuovo`, ora tolto anche lui).

**11 novembre, SD che non risponde dopo un risveglio (Smart Pro, una volta).** Dopo una
sospensione automatica arrivata 2 minuti dopo il primo avvio di un aggiornamento, nessun gioco
partiva (suono d'errore) e sulla SD non si è più scritto niente, nemmeno lo spegnimento. Non
si è ripetuto in tre prove (10 s, 40 s, 26 minuti). Ora `arcade-suspend` al risveglio scrive e
rilegge un file sulla SD in sottofondo (al massimo 5 s): se non risponde lo scrive in
`/dev/kmsg` e `/run/sd-guasta`, il launcher propone di riavviare e `system-init` riavvia dal
kernel (`/proc/sysrq-trigger`) senza scrivere sulla scheda. Le righe del Wi-Fi e del risveglio
si scrivono subito sulla SD (`sync`), perché uno spegnimento forzato perdeva proprio quelle.

**11 novembre, fruscio della Brick Pro (issue #4, in prova).** L'amplificatore resta sempre
acceso e amplifica il rumore della scheda (trimui/firmware_brickpro#5). Impostazione
"Altoparlante a riposo": spento dopo 4 s senza suoni nel launcher, con
`/sys/class/speaker/mute` (A) o `HpSpeaker Switch` (B); da confrontare a orecchio lo schiocco a
ogni cambio. Con A il fruscio non si sente nelle prime prove; si notava soprattutto durante
gli scaricamenti.
