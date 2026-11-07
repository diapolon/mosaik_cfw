# Studio: sistema operativo custom su SD per TrimUI Smart Pro (TG5040) e Brick Pro (TG4040)

Data: 1 novembre 2026 (aggiornato il 2 novembre) · Stato: solo analisi, nulla scaricato o compilato.

Obiettivo: console **arcade** (FinalBurn Neo, MAME, Neo Geo) con un **launcher nostro**
che avvia direttamente i giochi, gestisce le impostazioni di base e supporta **temi
grafici** (stile Switch o altre console). Niente porta seriale.

Dispositivi: **Smart Pro originale (TG5040)** e **Brick Pro (TG4040)**, entrambi con
Allwinner A133P. La Smart Pro S (TG5050) è un hardware diverso ed è esclusa.

---

## 1. In sintesi

- È **fattibile** un sistema che si avvia da microSD senza toccare la eMMC interna,
  quindi con rischio di danneggiare la console molto basso.
- TrimUI **non pubblica i sorgenti** di kernel e bootloader. La strada pratica è
  riusare i pezzi binari del firmware originale (bootloader, kernel 4.9.191, moduli,
  driver GPU) e costruire tutto il resto, cioè il sistema in user space, con Buildroot.
- Knulli fa già questo sulla Smart Pro e i suoi file di configurazione pubblici sono
  la mappa migliore disponibile.
- I due veri ostacoli sono il **debug senza porta seriale documentata** e i **driver
  chiusi** (GPU PowerVR, Wi-Fi XR829, demone dei tasti), che legano il progetto al
  kernel 4.9.
- Gli emulatori arcade disegnano in 2D via software e RetroArch non serve: bastano due
  core libretro e un frontend nostro. La GPU (OpenGL ES) resta però **consigliata** per
  il launcher, perché temi animati a 1280×720 e 60 fps sono pesanti senza accelerazione.
- Stima indicativa per un sistema usabile (launcher + arcade + impostazioni base):
  **diverse settimane** di lavoro con test continui sulla console.

---

## 2. Hardware

| Componente | Smart Pro (TG5040) | Brick Pro (TG4040) | Note per il porting |
|---|---|---|---|
| SoC | Allwinner A133P, 4× Cortex-A53 | Uguale (1,8 GHz) | Stesso sistema compilato per entrambe |
| GPU | PowerVR GE8300 | Uguale | Stessi driver chiusi |
| RAM / memoria | 1 GB LPDDR4 / 8 GB eMMC | 1 GB / 8 GB (da confermare) | La eMMC resta intatta se avviamo da SD |
| Display | 1280×720, **16:9** | 3,95" 1024×768, **4:3** | I temi devono adattarsi a due formati |
| Wi-Fi / BT | XR829 (moduli `xradio_*_tsp.ko`) | Da verificare in M0 | |
| Tasti / stick | Demone `trimui_inputd_smart_pro` | `trimui_inputd` (gamepad virtuale "TRIMUI Player1"); doppio stick hall | |
| Firmware originale | `firmware_smartpro` v1.1.1 (nov 2025) | `firmware_brickpro` v1.1.1 (lug 2026) | Stessa linea software TrimUI |

### 2.0 Firmware originali v1.1.1 a confronto (analizzati il 6 novembre 2026)

Dai repository `trimui/firmware_brick`, `firmware_smartpro` e `firmware_brickpro`. Le immagini
`.awimg` (formato Allwinner IMAGEWTY v3) **non sono cifrate**: `toolchain/awimg.py` ne estrae
le partizioni e `toolchain/analyze-firmware.sh` stampa il riepilogo qui sotto.

| Voce | Brick (TG3040) | Smart Pro (TG5040) | Brick Pro (TG4040) |
|---|---|---|---|
| Nome in `MainUI` | "Trimui Brick" | "Trimui Smart Pro" | "Trimui Brick Pro" |
| Pannello (device tree) | 1024×768 | **720×1280, verticale**: la rotazione la fa l'SDL2 di TrimUI | 1024×768 (`otm1289a`) |
| glibc / SDL2 | 2.33 / 2.30.8 | 2.33 / 2.30.8 | 2.33 / 2.30.8 |
| OpenSSL 1.1, zlib, GLES/EGL | presenti | presenti | presenti |
| Batteria | `axp2202-battery`, `axp2202-usb` | uguale | uguale |
| Luminosità | `/dev/disp` | uguale | uguale |
| Tasti | `trimui_inputd`, gamepad uinput "TRIMUI Player1–4" | uguale | uguale |
| Mixer | `amixer`, `tinymix` | uguale | uguale |

Il kit di sviluppo (2023) ha glibc 2.23 e SDL2 2.26: i programmi compilati con quello girano
anche sui firmware attuali, più nuovi. Formato delle app del menu (dalla Brick Pro):
`/mnt/SDCARD/Apps/<app>/config.json` con `label`, `icon`, `icontop`, `launch`,
`description`; le app scrivono `1` in `/tmp/stay_awake` per evitare la sospensione.

Conclusione: **un solo pacchetto** per le tre console. Restano da verificare sul dispositivo i
numeri dei pulsanti (dal demone dei tasti, non leggibili senza console) e i nomi dei
controlli del volume.

### 2.1 Cosa cambia tra le due console

Il SoC è lo stesso, quindi kernel, moduli GPU, rootfs, runner, launcher ed emulatori
sono **condivisi**. Cambiano:

- **pezzi di avvio**: `boot_package.fex` contiene il device tree (DTB) del singolo
  modello (schermo, tasti, LED, alimentazione) e va estratto dal firmware di ciascuna;
- **demone dei tasti** e mappatura dei pulsanti;
- **risoluzione e proporzioni** dello schermo;
- percorsi di luminosità, LED e batteria, se diversi.

Soluzione: **un'immagine SD per modello** (cambiano solo i primi ~50 MB con i pezzi di
avvio) e un sistema unico che all'avvio legge il modello da un file (come fa Knulli con
`batocera.board`) e carica la configurazione giusta.

Knulli supporta la Brick originale (TG3040) ma, nel repository consultato, **non la
Brick Pro**: per la TG4040 non c'è un riferimento già pronto e i blob vanno estratti
dal suo firmware.

Per l'arcade lo schermo 4:3 della Brick Pro è ideale: i giochi orizzontali la riempiono
senza bande nere.

---

## 3. Cosa pubblica TrimUI

| Repository | Contenuto | Utilità |
|---|---|---|
| `trimui/firmware_smartpro` | Solo changelog + immagini binarie (v1.1.1 ≈250 MB, recovery SD ≈2 GB) | Fonte dei blob: bootloader, kernel, moduli, librerie GPU |
| `trimui/toolchain_sdk_smartpro` | Compilatore Linaro GCC 7.5 aarch64, sysroot `SDK_usr_tg5040_a133p`, SDL2 2.26.1 per GE8300 | Compilare app compatibili con il sistema originale |
| `trimui/assets_smartpro` | Pacchetto base per la SD originale | Riferimento per emulatori e configurazioni |

Non ci sono sorgenti di kernel, U-Boot o MainUI.

---

## 4. Come fa Knulli (progetto di riferimento)

Fonte: `knulli-cfw/distribution`, cartella `board/batocera/allwinner/a133/trimui-smart-pro`.

### 4.1 Layout della scheda SD (dal suo `genimage.cfg`)

| Offset | Contenuto | Ruolo |
|---|---|---|
| 128 KiB | `boot0.img` (64 KiB) | Primo stadio Allwinner (inizializza la RAM), letto dalla ROM del SoC |
| ≈16 MiB (settore 32800) | `boot_package.fex` | Pacchetto Allwinner: U-Boot + ATF + firmware SCP + DTB |
| 36 MiB | `boot.img` (≈15 MB) | Immagine in stile Android con il kernel 4.9.191 |
| — | `env` (+ copia) | Ambiente U-Boot |
| — | FAT32 "BATOCERA" | Sistema in squashfs, logo, file di configurazione |
| — | ext4 "SHARE" | Dati utente, ROM e salvataggi |

Partizioni in GPT. La ROM del SoC prova la SD prima della eMMC, per questo
basta inserire la scheda per avviare il sistema alternativo.

### 4.2 Pezzi riusati dal firmware originale

- **Bootloader e kernel**: `boot0.img`, `boot_package.fex` e `boot.img` sono
  file binari presenti nel repository, non compilati.
- **Moduli kernel 4.9.191**: `pvrsrvkm.ko` (GPU), `dc_sunxi.ko`, `xradio_*_tsp.ko`
  (Wi-Fi), più qualche modulo netfilter.
- **Firmware**: `rgx.fw/rgx.sh.22.102.54.38` per la GPU e `*_xr829.bin` per Wi-Fi/BT.
- **Librerie GPU**: EGL/GLES fbdev dal repository `knulli-cfw/ge8300-drivers`
  (pacchetto `powervr-ge8300-driver`).
- **Input**: binario `trimui_inputd_smart_pro`.
- **SDL2**: con la patch `001-add-pvr-ge8300-mali-driver.patch`. Ci sono patch anche
  per RetroArch e PPSSPP per aggirare limiti del driver GLES.

### 4.3 Cosa compila Knulli

Compila solo il sistema in user space: toolchain glibc Buildroot, rootfs squashfs+zstd,
EmulationStation, RetroArch, emulatori e servizi.

La configurazione kernel nel repository (`linux-sunxi64-legacy.config`) sembra copiata da
un'altra piattaforma: contiene `SUNXI_SOC_NAME="sun50iw9"` e `GPU_TYPE="mali-g31"`, valori
dell'H700. È un forte indizio che il kernel avviato sia davvero quello binario di `boot.img`.
**Da verificare.**

---

## 5. Strade possibili

| | A. Kernel e boot originali + rootfs nostro | B. Kernel BSP ricompilato | C. Kernel mainline |
|---|---|---|---|
| Idea | Si riusano boot0, boot_package, boot.img, moduli e blob; si costruisce solo il rootfs | Si ricompila il 4.9 Allwinner da un SDK A133 | Linux recente, driver open |
| Sorgenti necessari | Nessuno | SDK Tina A133 (non ufficiale per TG5040, licenza incerta) + DTB estratto | Supporto A133 ancora in arrivo upstream |
| GPU 3D | Sì (blob) | Sì, se `pvrsrvkm` combacia | Di fatto no per GE8300 oggi |
| Rischio / sforzo | Basso / medio | Medio / alto | Alto / molto alto |
| Limiti | Bloccati al kernel 4.9.191 e ai suoi moduli | Va ricostruito e verificato tutto il DTB | Display, audio e risparmio energetico incerti |

**Raccomandazione: strada A.** È quella già dimostrata da Knulli. Lascia libertà
totale su sistema, launcher ed emulatori. La strada B si può valutare dopo, solo se
servono funzioni kernel mancanti.

---

## 6. Emulatori arcade

### 6.1 Scelta

| Emulatore | Cosa copre | Sulla Smart Pro | Ruolo |
|---|---|---|---|
| **FinalBurn Neo** (core `fbneo`) | Neo Geo MVS/AES/CD, Capcom CPS1/2/3, Cave, Toaplan, Irem, Konami, Data East… | Neo Geo e CPS1/2 a piena velocità; CPS3 da verificare | **Principale** |
| **mame2003-plus** (MAME 0.78 migliorato) | Giochi che FBNeo non copre | Leggero, adatto a questo hardware | **Complemento** |
| mame2010 (MAME 0.139) | Più giochi di mame2003-plus | Più pesante | Solo se serve un titolo specifico |
| Geolith | Solo Neo Geo, più fedele all'originale | Prestazioni da verificare | Da provare |
| MAME attuale | Tutto | Troppo pesante per 4× A53 e 1 GB di RAM | Escluso |

Frontend: niente RetroArch. I due core li carica un runner libretro nostro (vedi §7).

### 6.2 ROM e romset

- Ogni emulatore vuole ROM nella versione esatta per cui è scritto: una cartella per
  emulatore, ROM verificate con il suo file DAT.
- Il Neo Geo richiede il BIOS `neogeo.zip` (nella cartella delle ROM o in quella di sistema).
- Il DAT serve anche al launcher per mostrare i nomi leggibili ("Metal Slug" invece di
  `mslug.zip`) e per sapere quali giochi sono verticali.

### 6.3 Esigenze specifiche dell'arcade

- **Giochi verticali**: immagine piccola al centro oppure modalità ruotata (TATE, console
  tenuta di lato), scelta per singolo gioco.
- **Proporzioni**: 4:3 con bande nere sul 16:9, con opzione di scaling intero per
  un'immagine nitida.
- **Scaling**: verso 1280×720 con OpenGL ES, che permette anche filtri come le
  scanline. Come riserva: scaler hardware del display Allwinner (da verificare) o
  software con NEON.
- **Tasti**: A/B/X/Y + L/R coprono i 6 tasti di Street Fighter; servono anche gettone,
  start, menu di servizio e DIP switch (difficoltà, vite).

---

## 7. Frontend: launcher, runner e temi

### 7.0 Scelta

Tutto il frontend è **scritto da noi**, in C con SDL2 e OpenGL ES 2. È diviso in tre parti:

| Parte | Cosa fa |
|---|---|
| `libui` (libreria comune) | Motore dei temi: caricamento, disegno, animazioni, font, suoni, input |
| `launcher` | Home, libreria giochi, impostazioni della console |
| `runner` | Esegue i core libretro (FBNeo, mame2003-plus) e il menu in gioco, con lo stesso tema |

Perché non riusare un frontend esistente:

| Alternativa | Motivo per cui non la scelgo |
|---|---|
| minarch (MinUI) | Nessuna licenza dichiarata nel repository: il codice non si può riusare liberamente |
| NextUI | Da agosto 2026 licenza PolyForm Noncommercial: vincola qualsiasi distribuzione futura |
| picoarch | Repository originale non più raggiungibile; resta un riferimento |
| EmulationStation (ES-DE / Batocera) | Temi maturi, ma pesante e generico; il controllo su avvio e impostazioni sarebbe minore |
| RetroArch | Troppo complesso per due core |

L'API libretro è piccola e documentata (`libretro.h`): un runner per due core sono
poche migliaia di righe. Il codice di minarch e picoarch resta utile come riferimento
su come affrontare i problemi, senza copiarlo.

### 7.1 Struttura

Come in MinUI, uno script all'avvio fa girare in ciclo **launcher → runner → launcher**:

1. si sceglie un gioco, il launcher scrive cosa avviare e si chiude;
2. lo script lancia il runner con il core (FBNeo o mame2003-plus) e la ROM;
3. all'uscita dal gioco il launcher riparte.

Vantaggi con 1 GB di RAM: durante il gioco il launcher non occupa memoria, e un crash
dell'emulatore riporta comunque al menu.

### 7.2 Avvio dei giochi

- Lista per sistema o unica, con nomi dal DAT.
- Preferiti e ultimi giocati.
- Opzione "riprendi l'ultimo gioco all'accensione".
- Impostazioni per singolo gioco (proporzioni, TATE, DIP switch) salvate in un file per ROM.

### 7.3 Impostazioni della console

| Impostazione | Meccanismo probabile | Certezza |
|---|---|---|
| Luminosità | `/sys/class/backlight` oppure ioctl di `/dev/disp` (driver video Allwinner) | Da verificare in M0 |
| Volume | Mixer ALSA (libasound) | Alta |
| Batteria e ricarica | `/sys/class/power_supply/...` | Alta, percorso da verificare |
| Spegnimento / riavvio | `poweroff` / `reboot` | Alta |
| Spegnimento automatico, schermo off | Timer nel launcher + retroilluminazione | Alta |
| Profilo prestazioni / batteria | cpufreq: governor e frequenza massima | Alta |
| Data e ora | RTC + `hwclock` | Alta |
| Wi-Fi | Moduli xradio + `wpa_supplicant` | Media (driver chiusi) |
| LED | `/sys/class/leds` | Media |
| Mappatura tasti | Configurazione nostra sopra `trimui_inputd_smart_pro` | Media |
| Sospensione | Sospensione del kernel 4.9 Allwinner | **Bassa**: punto più delicato |

Piano di riserva per la sospensione: schermo spento + CPU al minimo + emulatore in pausa.

### 7.4 Menu in gioco

Tasto Menu durante la partita: salva/carica stato, proporzioni, DIP switch, filtri video
(es. scanline), torna al launcher. Sta nel runner e usa lo stesso tema del launcher.

### 7.5 Sviluppo

Launcher e runner si sviluppano e si provano **sul Mac** (SDL2 in una finestra 1280×720,
percorsi di sistema simulati). Anche i core FBNeo e mame2003-plus si compilano per macOS,
quindi sul Mac si può giocare davvero. Poi tutto si compila per aarch64; sulla console
restano da provare solo le funzioni hardware.

### 7.6 Temi grafici

Un tema è una cartella con un file descrittivo e le sue risorse:

```
themes/
  nome-tema/
    theme.json      layout delle schermate, colori, animazioni
    fonts/          caratteri TTF/OTF
    images/         sfondi, icone, cornici (PNG)
    sounds/         suoni di navigazione (WAV/OGG)
    preview.png     anteprima per il selettore temi
```

Cosa può descrivere `theme.json`:
- **schermate**: home, libreria, scheda gioco, impostazioni, menu in gioco;
- **elementi**: immagine, testo, rettangolo arrotondato, barra di stato (ora, batteria,
  Wi-Fi), barra dei tasti, lista o carosello orizzontale o griglia di riquadri;
- **stati**: aspetto dell'elemento selezionato (ingrandimento, bordo luminoso, ombra);
- **animazioni**: durata e curva di transizioni, scorrimenti, comparse;
- **varianti** chiaro/scuro e colore d'accento;
- **più risoluzioni**: posizioni e dimensioni relative (ancore, percentuali) più
  eventuali layout specifici per 16:9 (Smart Pro, 1280×720) e 4:3 (Brick Pro, 1024×768).

Copertine e immagini dei giochi stanno a parte (`media/<nome-rom>.png`) e ogni tema
decide come mostrarle.

Esempio, una home "stile Switch":
- barra in alto con ora, Wi-Fi e batteria;
- fila orizzontale di riquadri quadrati con le copertine degli ultimi giochi, bordo
  animato sul riquadro selezionato;
- fila di icone rotonde sotto (preferiti, libreria, impostazioni, temi);
- barra dei tasti in basso;
- suono breve a ogni spostamento.

Tutto questo è esprimibile col formato sopra.

**Nota legale**: i temi possono imitare lo *stile* di un'altra console, ma non devono
includere loghi, font, suoni o grafica originali di Nintendo o di altri produttori.
Le risorse vanno create da zero o prese con licenza libera.

**Prestazioni**: con OpenGL ES (driver PowerVR) le animazioni a 60 fps sono alla portata.
Senza GPU, SDL2 disegna via software: funziona, ma con effetti ridotti (meno trasparenze,
animazioni più semplici). Il motore prevede entrambe le modalità.

---

## 8. Debug e sicurezza

- **Rischio brick**: basso finché non si scrive sulla eMMC. Se la SD non avvia,
  basta toglierla e la console riparte col firmware originale. In ogni caso conviene
  tenere a portata di mano l'immagine di recovery ufficiale.
- **Console seriale: esclusa** (decisione presa). La console non si apre.
- **Come si fa il debug senza seriale**:
  1. bootloader e kernel restano quelli originali, già funzionanti: le parti nuove
     partono dopo, quando il kernel può già scrivere su SD e schermo;
  2. il primo programma avviato (init) scrive subito un log sulla partizione FAT,
     da leggere sul Mac togliendo la SD;
  3. testo e messaggi di errore a schermo dalle primissime fasi;
  4. `adbd` via USB (come fa Knulli) per avere una shell dal Mac col cavo;
  5. SSH via Wi-Fi quando il Wi-Fi funziona;
  6. un visualizzatore dei log dentro il launcher.
- Senza seriale, se qualcosa si blocca prima che il kernel parta non si vede nulla.
  Il rischio è basso proprio perché quella parte non la modifichiamo.

---

## 9. Ambiente di build su questo Mac

| Voce | Stato |
|---|---|
| Docker Desktop | Attivo, Linux **arm64** nativo (5 CPU, 4 GB RAM): compila per aarch64 senza emulazione |
| Spazio disco | **26 GB liberi (disco al 94%)**: critico. Un sistema solo arcade è più leggero di Knulli, ma Buildroot resta sui ~15–25 GB |
| Recuperabile | ~9,8 GB in volumi Docker inutilizzati + ~5,6 GB di immagini (`docker system df`) |
| Filesystem | APFS non distingue maiuscole/minuscole: la build va in un volume Docker, non in una cartella condivisa con macOS |
| RAM Docker | 4 GB vanno bene; 6–8 GB velocizzano la compilazione dei core |

---

## 10. Tabella di marcia proposta (strada A, obiettivo arcade)

| Fase | Obiettivo | Si considera finita quando |
|---|---|---|
| M0 · Analisi originale | Scaricare `trimui_tg5040_20251128_v1.1.1.zip` e `trimui_tg4040_20260717_v1.1.1.7z`, estrarre partizioni, DTB, `/lib/modules`, firmware e librerie GPU; leggere MainUI e script originali per trovare i percorsi di luminosità, LED, batteria; confrontare i due modelli tra loro e con i blob Knulli | Inventario dei blob per modello con versioni e hash + elenco delle differenze tra TG5040 e TG4040 |
| M1 · Primo avvio | SD con boot originale + initramfs BusyBox che scrive un log sulla FAT e un testo a schermo | Il testo compare sulla console e il log sul Mac |
| M2 · Sistema base | Rootfs Buildroot: schermo, tasti (`trimui_inputd`), audio ALSA, batteria, spegnimento, `adbd` via USB | Un'app SDL di test mostra immagini, risponde ai tasti, shell dal Mac via USB |
| M3 · GPU | `pvrsrvkm` + librerie EGL/GLES + SDL2 con backend OpenGL ES | Un'animazione di prova gira a 60 fps a 1280×720 |
| M4 · Arcade | Runner libretro + FBNeo + mame2003-plus, menu in gioco, salvataggi | Neo Geo e CPS2 giocabili a piena velocità con audio |
| M5 · Launcher e temi | Motore dei temi, home, libreria con nomi dal DAT, preferiti, avvio diretto, impostazioni; un primo tema completo | Si accende, si sceglie un gioco, si gioca, si torna al menu, si cambia tema |
| M6 · Extra e rifinitura | Wi-Fi + SSH, sospensione, LED, TATE, altri temi, aggiornamenti | Uso quotidiano senza bisogno del Mac |

Le fasi M1–M2 si fanno prima su una console e poi si ripetono sull'altra (cambiano solo
pezzi di avvio, tasti e schermo); da M3 in poi il lavoro vale per entrambe.

**Sul Mac, in parallelo**, si possono sviluppare da subito motore dei temi, launcher e
runner (anche con i core arcade veri). Le fasi M4 e M5 sulla console diventano quindi
soprattutto lavoro di porting e messa a punto.

---

## 11. Punti aperti da verificare

1. Il `boot.img` di Knulli coincide con quello del firmware originale v1.1.1?
2. La SD ha sempre la precedenza sulla eMMC su tutte le revisioni hardware?
3. `trimui_inputd_smart_pro` richiede librerie o file del sistema originale?
4. Le licenze di redistribuzione dei blob: per uso personale va bene, ma per pubblicare
   un'immagine bisogna valutarle.
5. Il kernel originale supporta la modalità USB device per `adbd` (Knulli la usa:
   probabile).
6. Percorsi e ioctl per luminosità, LED e batteria usati dal firmware originale.
7. Lo scaler hardware del display Allwinner è usabile da user space per ingrandire
   l'immagine dei giochi?
8. CPS3 in FinalBurn Neo gira a piena velocità sull'A133P?
9. La sospensione funziona in modo affidabile con il kernel 4.9 originale?
10. SDL2 con OpenGL ES sui driver PowerVR (fbdev, senza X11/Wayland) regge le
    animazioni dei temi a 60 fps? Knulli usa una patch SDL2 apposita.
11. (In parte risolto, §2.0: librerie, schermo, batteria, luminosità e formato delle app.)
    Brick Pro: kernel, moduli e driver GPU sono gli stessi della Smart Pro? Chip Wi-Fi,
    tasti e percorsi hardware coincidono?
12. Brick Pro: la SD ha la precedenza sulla eMMC come sulla Smart Pro?

---

## 12. Decisioni

Prese:
- **Emulatori**: solo arcade, con FinalBurn Neo principale e mame2003-plus come complemento.
- **Launcher**: scritto da zero, con avvio diretto dei giochi e impostazioni della console.
- **Dispositivi**: Smart Pro (TG5040) e Brick Pro (TG4040), stesso sistema (§2.1).
- **Seriale**: no. Debug con log su SD, testo a schermo, `adbd` via USB e SSH (§8).
- **Frontend**: tutto nostro (`libui` + `launcher` + `runner`), in C con SDL2 e
  OpenGL ES, con motore di temi grafici (§7).
- **Spazio disco**: risolto (78 GB liberi, disco virtuale Docker portato a 64 GB).
- **Giochi Linux**: cartella `Linux/<gioco>/launch.sh`, avviati dallo stesso ciclo del
  launcher. Supporto minimo già nel prototipo, da completare (controlli, ritorno al menu).

---

## 13. Stato del prototipo (6 novembre 2026)

Gradino 1 del percorso Mac → app sul firmware originale → sistema su SD: **fatto**.
Codice in [frontend/](frontend/README.md).

| Parte | Stato |
|---|---|
| Launcher: home a carosello, libreria a griglia o elenco, preferiti, recenti | Funziona sul Mac |
| Impostazioni: luminosità, volume, tema, variante, proporzioni, filtro, CPU, Wi-Fi, spegnimento automatico, ripresa partita | Funzionano; luminosità, Wi-Fi e CPU sono simulati |
| Temi: Tessera (carosello, chiaro/scuro) e Cabinato (elenco neon, scanline, due varianti) | Funzionano a 1280×720 e 1024×768 |
| Runner: FBNeo e mame2003-plus, rotazione dei verticali, proporzioni, filtri, stati, menu in gioco | Provato con Metal Slug (Neo Geo) e 1942 (verticale) |
| Giochi Linux | Elencati e avviati tramite `launch.sh` |
| Immagini: scraper ArcadeDB + miniature libretro, riquadri 4:3, marquee, genere e giocatori | Funziona sul Mac (`tools/scrape.py`) |
| Lingue: italiano e inglese, file `Lang/*.json`, lingua passata ai core | Funziona; generi tradotti dal launcher |

### 13.1 Cosa resta da fare

**Gradino 2: app sul firmware originale** (6 novembre: pacchetto pronto, da provare)
1. Fatto: immagine Docker `trimui-tg5040` con il kit ufficiale (Linaro GCC 7.5, librerie del
   firmware). Il firmware ha glibc 2.23 e SDL2 2.26 con driver PowerVR; SDL2_ttf, SDL2_image,
   SDL2_mixer (2.0.x nel firmware) e libcurl (assente) compilate a parte e incluse.
2. Fatto: launcher, runner, gamewatch e i core FBNeo e mame2003-plus per aarch64
   (Cortex-A53). Il launcher è stato avviato in un container Linux arm64 con l'SDL2 del
   firmware (disegno software): giochi, immagini e testi corretti.
3. Fatto, da verificare: parte "piattaforma" con i percorsi di MinUI (luminosità via
   `/dev/disp`, volume `amixer`, CPU `scaling_setspeed`, batteria `axp2202`), tasti integrati
   come joystick SDL con mappatura modificabile, modello riconosciuto da `MainUI`.
4. Fatto: pacchetto `dist/Arcade-trimui.zip` (`Apps/Arcade` + `Arcade/`). **Da fare sulla
   console**: tasti, GPU (OpenGL ES, shader), audio, luminosità/volume, velocità di Neo Geo,
   CPS2 e CPS3, scraper via Wi-Fi, Brick Pro.

**Gradino 3: sistema proprio su SD** (fasi M0–M6, §10)
- **M1 riuscita sulla Brick Pro (6 novembre)**: la console si avvia dalla SD con boot0,
  U-Boot e kernel originali e un nostro initramfs, che ha scritto il rapporto sulla FAT.
  Dal rapporto: kernel 4.9.191 #913; con avvio da SD si vede solo `mmcblk0` = la SD (la eMMC
  non compare); framebuffer 1024×768 32 bpp ARGB (pannello `lcd=gh7003` da U-Boot); batteria
  `axp2202` leggibile; audio `audiocodec`; CPU 408 MHz–2 GHz. Tre riavvii dopo ~20 s: il
  **watchdog** è attivo (nel sistema originale lo alimenta procd), corretto nella versione 2.
  I **tasti non sono un dispositivo del kernel** (ci sono solo volume `sunxi-keyboard` 114/115
  e accensione `axp2202-pek` 116): li legge `trimui_inputd` (GPIO via `/dev/mem`, levette
  via `/dev/i2c-3`) che crea il gamepad uinput "TRIMUI Player1"; la versione 2 lo avvia per
  registrare i codici. La calibrazione delle levette è in `/mnt/UDISK/joypad.config` (eMMC).
- **M1 versione 2 riuscita (6 novembre)**: watchdog alimentato, nessun riavvio.
  `trimui_inputd` del firmware gira anche nel nostro sistema e crea "TRIMUI Player1", che si
  presenta come **controller Xbox 360** (USB 045e:028e) con i tasti per posizione, come il
  driver xpad: A (destra) 305, B (basso) 304, X (alto) 308, Y (sinistra) 307, L 310, R 311,
  i tre tasti in basso Menu (logo TrimUI) 316, Select (quadrato) 314, Start (play) 315,
  L3 317, R3 318, volume 114/115; **i due tasti a barretta tra croce e A/B/X/Y sono 59 e 60**
  (F1/F2; quale sia il sinistro va confermato), **172 (KEY_HOMEPAGE) è il tasto HOME sul
  lato sinistro**, sotto + e − (volume 115/114); sul lato destro FN e accensione; **FN è l'interruttore a scorrimento
  laterale**: arriva come evento non-tasto (tipo da confermare, probabilmente EV_SW) e il
  firmware lo legge anche dal GPIO 243 (`/sys/class/gpio/gpio243/value`);
  croce sul cappello (ABS 16/17), L2/R2 sugli assi 2 e 5 (0–255), levette sugli assi 0/1 e
  3/4 (±32760). Accensione: `axp2202-pek` 116. SDL lo riconosce come controller standard.
- **M2 funziona sulla Brick Pro (6 novembre sera)**: launcher, giochi FBNeo e MAME con
  video e audio, gioco Linux (RetroStrike), tasti extra, spegnimento pulito. Problemi risolti
  strada facendo: permesso di `/sbin/init`, PATH di `trimui_inputd`, udev richiesto dalla
  SDL2 di TrimUI, audio fermo finché non si impostano i controlli come in Knulli (interruttori
  di cuffie, altoparlante e LINEOUT, uscita `DAC_SINGLE`), FAT danneggiato dagli spegnimenti
  bruschi (ora `fsck.fat` a ogni avvio e montaggio con `flush`), gioco trasparente (texture
  ARGB con l'alfa a 0: ora RGB888/RGB555).
- **M2 funziona anche sulla Smart Pro (7 novembre)**: stesso sistema con uno strato per
  modello (`/fw/tg5040`), immagine `dist/m2-smartpro.img`. Gradino 3 raggiunto su entrambe le
  console: sistema proprio su SD, senza toccare la memoria interna.
- **Wi-Fi (7 novembre, da provare sulla console)**: XR829 con i moduli `xradio` del firmware,
  `wpa_supplicant`/`wpa_cli` 2.6, `udhcpc` e `ntpd` di BusyBox (`m2/wifi-ctl`); schermata
  delle reti nel launcher con tastiera a tre disposizioni per le password.
- **Spazio e orologio (7 novembre, da provare sulla console)**: la partizione ARCADE si
  allarga a tutta la scheda alla prima accensione (`m2/sdgrow.c`; FAT con tabella preparata
  per 256 GB da `m2/fatprep.py`), `fsck.fat` solo dopo uno spegnimento brusco (segno "in uso"
  del FAT), "Data e ora" nelle impostazioni con scrittura dell'RTC, luminosità registrata nel
  log (`luminosità: livello … esito …`).
- **M2 pronta per la Brick Pro (6 novembre, `m2/`)**: SD che avvia direttamente il launcher.
  Sistema in un unico squashfs (13 MB) sulla FAT, con BusyBox e le librerie del firmware
  (glibc 2.33, SDL2 2.30.8 con driver "mali", PowerVR, ALSA, `trimui_inputd`); initramfs che
  monta e fa `switch_root`; moduli GPU caricati come il firmware. Provata in un container
  arm64 fino all'avvio del launcher (la SDL2 del firmware ha solo il driver video "mali",
  quindi il disegno si vede solo sulla console). Da provare sulla console.
- M0 fatto per le tre console (§2.0). M1 per la Brick Pro (`m1/`):
  immagine SD con boot0 e boot_package originali in modalità normale, kernel originale e un
  initramfs nostro che salva un rapporto sull'hardware nella FAT della SD. Da provare sulla
  console. Nota: la scheda di ripristino ufficiale differisce dal firmware normale solo per
  il byte "work mode" del boot_package (0x11): è quello che le fa riscrivere la eMMC.
- M0 analisi dei due firmware, M1 primo avvio con log su SD, M2 sistema base con
  `adbd`, M3 GPU, M4–M5 runner e launcher sulla console (in gran parte già pronti),
  M6 Wi-Fi + SSH, sospensione, LED, aggiornamenti.

**Frontend: funzioni ancora mancanti**
- Verifica sulla console di shader (OpenGL ES 2), `gamewatch` (codici dei tasti del pad
  integrato, uinput nel kernel), scraper, batteria (sysfs), controller Bluetooth.
- Giochi Linux: mouse simulato con la levetta, per i giochi PC che lo richiedono.
- Due giocatori in rete o a schermo condiviso non previsti: solo due controller sulla stessa console.

**Frontend: fatte il 6 novembre** (vedi `frontend/README.md`)
- Menu in gioco a pagine: DIP switch e opzioni del core (con accesso al menu di servizio),
  comandi rimappabili con i nomi dei tasti del gioco.
- Impostazioni per singolo gioco: proporzioni, filtro, rotazione TATE con croce ruotata.
- Giochi doppi FBNeo/MAME: impostazione "Giochi doppi" (predefinito: preferisci FBNeo).
- Filtro per genere in libreria (SELECT).
- Icone ingranaggio e stella con bordi morbidi.
- Terzo tema "Onda" con layout a barre (categorie e generi in orizzontale) e onde animate.
- Scraper dentro il launcher (C + libcurl), con barra di avanzamento.
- Lingue: italiano, inglese, francese, spagnolo, tedesco, portoghese.
- Giochi Linux: variabili d'ambiente da game.json, `gamewatch` per l'uscita forzata con Menu.
- Filtri video da cartella (Nitido, Morbido, Scanline, Griglia LCD, CRT) e cornici (bezel).
- Statistiche (partite, tempo giocato) e ordinamento della libreria.
- Ricerca con tastiera a schermo e pannello Filtri (SELECT).
- Scheda del gioco con storia da ArcadeDB, informazioni e statistiche (X).
- Tasti rapidi in gioco con Menu tenuto premuto: salvataggio/caricamento rapido, avanti
  veloce, screenshot, volume e luminosità con indicatore a schermo.
- Shader GLSL nel formato a passaggio singolo di RetroArch: CRT curvo e Pixel nitidi inclusi.
- Emulatori da file di configurazione (`Emulators/<id>.json`): aggiungerne uno non richiede
  di toccare il codice.
- Catalogo completo dei giochi supportati, con i giochi senza ROM nascondibili.
- Salvaschermo (schermate dei giochi o schermo spento) e avvisi/salvataggio per batteria scarica.
- Shader multi-pass (`.glslp`) e parametri regolabili per gioco; preset "CRT con bagliore".
- Verifica delle ROM: CRC di ogni file dentro gli zip confrontati con il DAT, parent e BIOS
  controllati; esito nella scheda e filtro "solo ROM da sistemare".
- Due giocatori: comandi della console = giocatore 1, controller esterni = 2–4 (o esterno
  come giocatore 1), avviso al collegamento.
- Collezioni personalizzate (schede e categorie) e giochi nascosti, dalla scheda del gioco (X).
- Musica di sottofondo da `Music/` (SDL_mixer: MP3, OGG, FLAC, MOD), volume e brano successivo.
- Giochi Linux: tasti della console come tastiera (`keys` in game.json, uinput in
  `gamewatch`), provato in un container Linux con un gamepad virtuale.
- Scheda del gioco ridisegnata: marquee come insegna sopra lo schermo, schermo nelle
  proporzioni vere con sfondo sfocato per i verticali, etichette colorate.

---

## Fonti

- [trimui/firmware_smartpro](https://github.com/trimui/firmware_smartpro): changelog e release
- [trimui/toolchain_sdk_smartpro](https://github.com/trimui/toolchain_sdk_smartpro): SDK e toolchain
- [trimui/firmware_brickpro](https://github.com/trimui/firmware_brickpro) e [assets_brickpro](https://github.com/trimui/assets_brickpro): firmware e pacchetto SD della Brick Pro
- [Android Authority: specifiche Brick Pro](https://www.androidauthority.com/trimui-brick-hammer-pro-u-brick-pro-specs-reveal-3676159/), [Notebookcheck: uscita Brick Pro](https://www.notebookcheck.net/TrimUI-Brick-Pro-Game-Boy-style-retro-handheld-released-globally-with-larger-display-and-better-battery-life.1339564.0.html)
- [knulli-cfw/distribution](https://github.com/knulli-cfw/distribution): `board/batocera/allwinner/a133/`, `configs/knulli-a133.board`, `package/batocera/gpu/powervr-ge8300-driver`
- [linux-sunxi: Mainlining effort](https://linux-sunxi.org/Mainlining_Effort)
- [LKML: A133 support (ottobre 2024)](https://lkml.iu.edu/hypermail/linux/kernel/2410.2/05973.html)
- [linux-arm-kernel: DTS Baijie Helper A133 (maggio 2026)](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-May/1130667.html)
- [CNX Software: A133 con XR829](https://www.cnx-software.com/2020/10/31/allwinner-a133-tablet-processor-pairs-with-xr829-or-aw859a-wifi-bluetooth-chip/)
- [linux-sunxi: TrimUI Model S](https://linux-sunxi.org/TrimUI_Model_S): esempio di pad UART su un altro modello TrimUI
- [FinalBurn Neo](https://github.com/finalburnneo/FBNeo) e [core libretro](https://github.com/libretro/FBNeo)
- [mame2003-plus](https://github.com/libretro/mame2003-plus-libretro)
- [Geolith](https://github.com/libretro/geolith-libretro)
- [MinUI](https://github.com/shauninman/MinUI) (nessuna licenza dichiarata) e [NextUI](https://github.com/LoveRetro/NextUI) (PolyForm Noncommercial 1.0.0 da agosto 2026): launcher + minarch, già su TG5040
