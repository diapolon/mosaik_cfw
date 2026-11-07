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

## Com'è fatta la scheda (`dist/m2-brickpro.img`, 2 GB)

| Parte | Contenuto |
|---|---|
| boot0, boot_package | originali del firmware v1.1.1, modalità normale (come M1) |
| partizione `boot` | kernel originale 4.9.191 + nostro initramfs (`m2/initramfs-init`) |
| `env`, `env-redund` | ambiente di U-Boot originale |
| FAT "ARCADE" (1,9 GB) | cartella `Arcade` (temi, lingue, filtri, core, indici, ROM, salvataggi, log) e `LEGGIMI.txt` |
| `Arcade/System/arcade-system.squashfs` (13 MB) | il sistema: BusyBox, librerie di base, GPU, audio, `trimui_inputd`, `amixer`/`tinymix` del firmware, i nostri programmi, `m2/system-init` |

Avvio:
1. **initramfs**: alimenta il watchdog, trova la FAT "ARCADE", monta lo squashfs e gli passa
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

- La FAT è di 1,9 GB qualunque sia la scheda: l'allargamento automatico allo spazio libero
  arriverà più avanti (oggi si può fare a mano con un programma di partizionamento).
- Niente Wi-Fi, Bluetooth, sospensione e LED (fase M6).
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
