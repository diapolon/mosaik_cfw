# M2: sistema Arcade su SD (Brick Pro TG4040, Smart Pro TG5040)

**Due modelli, un solo sistema (6 novembre).** `arcade-system.squashfs` è unico: una parte
comune (BusyBox, launcher, runner, glibc) e uno strato per modello in `/fw/tg4040` e
`/fw/tg5040` con ciò che nei firmware è compilato a parte (moduli e librerie della GPU,
SDL2, ALSA, udev, `trimui_inputd`) più `hw-init.sh` (GPIO come `runtrimui.sh`: sulla Smart
Pro anche 110 e 114, l'alimentazione delle schede delle levette). L'initramfs del modello
collega `/run/fw` allo strato giusto. Le immagini sono due perché l'area di avvio (boot0,
U-Boot con la configurazione dell'hardware, kernel) è propria di ogni console e il chip la
legge da una posizione fissa: un'immagine unica non è possibile, ma il contenuto (cartella
`Arcade`, ROM, salvataggi) è lo stesso e si sposta da una scheda all'altra.

- `dist/m2-brickpro.img` per la Brick Pro (provata sulla console)
- `dist/m2-smartpro.img` per la Smart Pro (provata sulla console)

**Seconda prova**: avvio completo fino al launcher, moduli della GPU caricati (PowerVR
riconosciuta, framebuffer 1024×768), ma la SDL2 di TrimUI non partiva: `Failed loading
libudev.so.0`. Ora il sistema include `udevd`, `udevadm`, `libudev` e le regole del firmware e
li avvia come `/etc/init.d/udev` (trigger e settle) prima del launcher; aggiunto anche un
`setterm` vuoto che la SDL2 chiama.

**Prima prova (6 novembre)**: schermata di avvio e poi riavvio, nessun log. Causa: lo script
di avvio del sistema (`/sbin/init`) era nello squashfs senza permesso di esecuzione, il
passaggio al sistema falliva e il kernel riavviava. Corretto (e ora verificato durante la
costruzione). In più: ogni passo dell'avvio va a schermo e in `Arcade/Logs/passi.txt` con
`sync` immediato, i messaggi del kernel si salvano prima di caricare la GPU, e se il
launcher non parte compare un errore rosso invece di un riavvio.

La console si accende direttamente nel launcher Arcade, senza il sistema TrimUI e senza
toccare la memoria interna. Si toglie la scheda e riparte il sistema originale.

**Spazio della SD e data/ora (7 novembre).** Alla prima accensione la partizione ARCADE si
allarga da sola fino alla fine della scheda (`m2/sdgrow.c`, provato con `m2/test-sdgrow.sh` su
schede simulate da 2,3 GB a 300 GB e letto dal Mac). Il FAT dell'immagine nasce con la
tabella dei cluster già grande per 256 GB (cluster da 32 KB, `m2/fatprep.py`): allargarlo
vuol dire solo cambiare il numero di settori, senza spostare dati. Sopra i 256 GB la
partizione occupa tutta la scheda ma il FAT si ferma a 256 GB. Il controllo del FAT
(`fsck.fat`) ora parte solo se all'ultimo spegnimento il FAT non è stato chiuso bene: con la
tabella più grande ogni controllo legge decine di MB. In Impostazioni c'è "Data e ora", che
scrive anche l'orologio della console in UTC (`hwclock -w -u`); all'avvio il sistema lo rilegge.
Il fuso orario (Impostazioni, predefinito Europe/Rome) è una regola POSIX nella variabile TZ,
con l'ora legale. Allo spegnimento si fermano i programmi con file aperti sulla SD
(`trimui_inputd` tiene aperto il suo log), altrimenti il FAT non si chiude e al prossimo
avvio riparte il controllo. La pulizia dei file di macOS (`._*`) parte 20 s dopo l'avvio.

**Wi-Fi (7 novembre, da provare sulla console).** Entrambe le console hanno il chip XR829
(driver `xradio_mac`, `xradio_core`, `xradio_wlan`, compilati per il kernel di ogni modello:
sono nello strato `/fw/<id>/modules`; il firmware del chip è in `/lib/firmware`, uguale per
tutti). `m2/wifi-ctl on|off` carica il driver e avvia `wpa_supplicant` 2.6 del firmware
(controllo in `/run/wpa`, reti salvate in `Arcade/System/wifi.conf`) e `wpa_cli -a
m2/wifi-event`: a ogni connessione `udhcpc` di BusyBox chiede l'indirizzo
(`m2/udhcpc-script`, `resolv.conf` in `/run`) e alla prima prende l'ora con `ntpd` e la
scrive nell'RTC. Il sistema accende il Wi-Fi all'avvio se è attivo nelle impostazioni; il
launcher (`frontend/src/common/wifi.c`) usa `wpa_cli` per stato, ricerca, collegamento e
reti da dimenticare. Log in `Arcade/Logs/wifi.log` e nella sezione wi-fi di `sistema.txt`.
Aggiunte anche `libnss_dns` e `libnss_files`: senza, la glibc non risolve i nomi internet.

**LED e sospensione (7 novembre).** LED dal driver `led_anim` (zone `lr`, `m`, `f1`, `f2`,
`rear` se esistono; luminosità `max_scale` = livello × 4 come il firmware), provati sulla Smart
Pro. Tasto di accensione: breve = sospensione con `m2/arcade-suspend` (Wi-Fi e LED spenti,
`/tmp/system_suspend` per `trimui_inputd`, `wakeup_count` e `echo mem > /sys/power/state` come
`/sbin/suspendtomem.sh`; al risveglio audio, LED, Wi-Fi, luminosità e volume riapplicati);
tenuto premuto 1,2 s = spegnimento (in gioco salva prima). Da provare sulla console.

**Avvio più rapido e logo (7 novembre, provati sulla Smart Pro).** Dall'avvio del kernel alla
prima schermata del launcher: da 9,6 a 4,9 s. Kernel 3,7 → 2,1 s con `loglevel=4`
nell'ambiente di U-Boot (`m2/envtool` lo scrive nella copia di riserva `env-redund`, con
verifica; le immagini nuove nascono così): con 8 ogni messaggio andava sulla porta seriale.
Moduli GPU in parallelo con udev e tasti; audio con la configurazione ricordata applicata
senza prova e in una sola chiamata `amixer -s` (2,2 → 0,07 s); niente sync e schermate a ogni
passo. Logo: `bootlogo.bmp` nella FAT ARCADE (U-Boot lo legge da "boot-resource", come il
logo TrimUI; 396x66, 32 bit Smart Pro / 24 bit Brick Pro, da `m2/mklogo.py`) e lo stesso logo
mostrato da `fbmsg logo` fino al launcher; le schermate a strisce restano per gli errori.
Tempi del launcher nel log (`tempi: ...`).

**Versione e aggiornamento via Wi-Fi (8 novembre).** Il numero sta nel file `VERSION`; la build
aggiunge la data di compilazione (`/etc/arcade-version`, per esempio `0.9.0 20261108.1010`) e la
console accetta solo versioni più recenti. In Impostazioni > Strumenti > Aggiorna sistema la
console cerca: un Mac in rete locale (`python3 toolchain/update-server.py`, broadcast UDP 8766),
poi `System/update-url.txt`, poi l'indirizzo online (file `UPDATE_URL`, compilato nel launcher).
Bastano file statici: `update.txt`, `update.sig`, `file-0`, `file-1`...
(`update-server.py --out CARTELLA`). L'elenco è firmato ECDSA P-256 con la chiave privata in
`~/.config/arcade-update/update-key.pem`, **da conservare**: senza, le console già in giro non
accettano aggiornamenti; la pubblica è in `frontend/src/launcher/update_pubkey.h`. I file si
scaricano come `.nuovo`, si rileggono dalla SD e si controlla lo SHA-256; poi si sostituiscono,
il sistema per ultimo. Passano solo i file del sistema (mai ROM, salvataggi, impostazioni).
Il sistema precedente resta come `.prec`: se il nuovo non arriva alla prima schermata del
launcher per 3 avvii, `system-init` lo rimette. Prove: `m2/test-update.sh`.

## Com'è fatta la scheda (`dist/m2-brickpro.img`, 2,3 GB)

| Parte | Contenuto |
|---|---|
| boot0, boot_package | originali del firmware v1.1.1, modalità normale (come M1) |
| partizione `boot` | kernel originale 4.9.191 + nostro initramfs (`m2/initramfs-init`) |
| `env`, `env-redund` | ambiente di U-Boot originale |
| FAT "ARCADE" (2,2 GB, poi tutta la scheda) | cartella `Arcade` (temi, lingue, filtri, core, indici, ROM, salvataggi, log) e `LEGGIMI.txt` |
| `Arcade/System/arcade-system.squashfs` (13 MB) | il sistema: BusyBox, librerie di base, GPU, audio, `trimui_inputd`, `amixer`/`tinymix` del firmware, i nostri programmi, `m2/system-init` |

Avvio:
1. **initramfs**: alimenta il watchdog, trova la FAT "ARCADE" (controllo con `fsck.fat` se
   serve, allargamento con `sdgrow` alla prima accensione), monta lo squashfs e gli passa
   il controllo (`switch_root`). Se qualcosa manca lo dice a schermo (rosso) e lo scrive in
   `Arcade/Logs/initramfs.txt`.
2. **system-init**: carica i moduli della GPU (`pvrsrvkm`, `dc_sunxi`), avvia `trimui_inputd`,
   imposta l'audio come il firmware, scrive `Arcade/Logs/sistema.txt` (moduli, ingressi,
   controlli audio, messaggi del kernel) e avvia il launcher in modalità autonoma.
3. **launcher/runner**: come nell'app; "Spegni" spegne davvero (`poweroff -f`).
   Tasti extra: HOME torna al launcher, F1/F2 salvano e caricano, FN = prestazioni massime.

Per aggiornare il sistema basta copiare dal Mac un nuovo `arcade-system.squashfs` (e i file
della cartella `Arcade`) sulla scheda: non serve riscriverla.

## Provare

1. Scrivi `dist/m2-brickpro.img` su una microSD con balenaEtcher (come M1).
2. Dal Mac copia qualche ROM in `Arcade/Roms/FBNEO` (e `neogeo.zip` in `Arcade/Bios`).
3. Accendi la console con la scheda.
4. Manda i file di `Arcade/Logs`: `avvio.log`, `sistema.txt`, `Arcade.log`.

## Limiti di questa versione

- Il FAT arriva al massimo a 256 GB. Una scheda scritta con un'immagine di prima del 7
  novembre ha il FAT da 1,9 GB non preparato a crescere: la partizione si allarga, il FAT no.
  Per avere tutto lo spazio va riscritta con l'immagine nuova (prima salva ROM e salvataggi).
- Niente Bluetooth, sospensione e LED (fase M6).
- Calibrazione delle levette: quella di fabbrica è nella memoria interna. Se le levette
  sono imprecise, si può copiare `joypad.config` dal sistema originale in `Arcade/System`.
- Il nome del modello è fisso ("Trimui Brick Pro"): per le altre console servirà un'immagine
  con i loro pezzi di avvio.

## Ricostruire

```bash
sh m2/build-m2.sh        # wrapper; il lavoro è in m2/build-in-container.sh
```

Per aggiungere un modello: estrarne il firmware con `toolchain/analyze-firmware.sh`, scrivere
`m2/hw-init-<id>.sh` e aggiungere una riga a `MODELS` in `m2/build-in-container.sh`.
