# Prova M1: primo avvio da SD (Brick Pro, TG4040)

**Esito della prima prova (6 novembre): avvio da SD riuscito.** Tre rapporti uguali perché il
watchdog del kernel riavviava la console dopo ~20 s; la versione 2 lo alimenta, avvia
`trimui_inputd` del firmware (i tasti non sono un dispositivo del kernel) e salva il rapporto
dopo ogni passo. Dettagli nello studio, §13.1.

Prima fase del sistema proprio su SD (studio §10): verificare che la console si avvii dalla
scheda con i pezzi originali TrimUI e un sistema nostro, e raccogliere le informazioni
sull'hardware che servono per le fasi successive. **Non scrive nella memoria interna.**

## Cosa contiene `dist/m1-brickpro.img` (256 MB)

| Posizione | Contenuto | Da dove |
|---|---|---|
| 8 KiB e 128 KiB | boot0 | firmware v1.1.1, identico alla scheda di ripristino TrimUI |
| settore 32800 | boot_package (U-Boot, ATF, device tree) | firmware v1.1.1, **modalità normale** (byte "work mode" = 0) |
| partizione `boot` | kernel originale 4.9.191 + nostro initramfs | `boot.fex` ricomposto da `toolchain/bootimg.py` |
| `env`, `env-redund` | ambiente di U-Boot originale | firmware v1.1.1 |
| `boot-resource` (FAT, "ARCADE") | `ARCADE-M1.txt` e cartella `LOG` | — |

La scheda di ripristino ufficiale ha lo stesso boot_package con il byte "work mode" a 0x11
("produzione da scheda"): all'accensione **riscrive la memoria interna**. Qui c'è quello
normale, come in Knulli: U-Boot legge solo la partizione `boot` della SD.

L'initramfs (BusyBox statico, `rdinit`, `m1test`) mostra una schermata con barre di colore,
salva in `LOG/m1-N.txt` kernel, CPU, dischi, schermo, ingressi, batteria, audio e i messaggi
del kernel, registra per 25 secondi i tasti premuti e spegne. Scrive solo nella partizione
FAT che contiene `ARCADE-M1.txt`.

## Come si prova

1. Una microSD **di scorta** (verrà cancellata tutta), almeno 512 MB.
2. Sul Mac, con [balenaEtcher](https://etcher.balena.io): *Flash from file* →
   `dist/m1-brickpro.img` → la SD → *Flash*. Oppure da Terminale, dopo aver controllato con
   `diskutil list` qual è la SD (qui `diskN`, attenzione a non sbagliare disco):
   `diskutil unmountDisk /dev/diskN && sudo dd if=dist/m1-brickpro.img of=/dev/rdiskN bs=4m && diskutil eject /dev/diskN`
3. Console spenta, inserisci la SD, accendi.
4. Atteso: schermata blu **ARCADE M1 – AVVIO RIUSCITO**, poi **PREMI TUTTI I TASTI** (20 s:
   premi ogni tasto, Menu compreso, e muovi le levette; il numero compare in giallo), poi
   verde **RAPPORTO SALVATO**, poi la console si spegne.
5. Rimetti la SD nel Mac: il volume ARCADE contiene `LOG/m1-1.txt`.

Se lo schermo resta nero per più di un minuto: tieni premuto il tasto di accensione
10–20 secondi, togli la SD e riaccendi; parte il sistema originale. Una schermata nera non
dice da sola dove si è fermato l'avvio: in quel caso il passo successivo è provare la
copia di boot0 a 128 KiB da sola, o l'ambiente di U-Boot di Knulli.

## Ricostruire

```bash
# una volta: estrae il firmware Brick Pro nel volume Docker
docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro alpine:3.20 \
    sh /trimui/toolchain/analyze-firmware.sh deps/firmware/tg4040/dl/trimui_tg4040_20260717_v1.1.1.7z tg4040b
sh m1/build-m1.sh
```
