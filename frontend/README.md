# Prototipo frontend arcade (Mac)

Launcher con temi grafici e runner libretro per FinalBurn Neo e mame2003-plus, pensati
per TrimUI Smart Pro (TG5040) e Brick Pro (TG4040). Questa versione gira sul Mac, in una
finestra che simula lo schermo della console. Il progetto completo è descritto in
[../STUDIO-CFW-TG5040.md](../STUDIO-CFW-TG5040.md).

## Avvio rapido

```bash
make
FBNEO_ROMS="$HOME/Downloads/Emu/recalbox_OK/roms/fbneo" MAME_ROMS="$HOME/Downloads/Emu/Arcade - Mame 2003 Plus" ./setup-mac.sh
./run-mac.sh
```

Servono SDL2, SDL2_ttf, SDL2_image e SDL2_mixer (`brew install sdl2 sdl2_ttf sdl2_image sdl2_mixer`) e i due core
compilati in `../deps` (vedi sotto). `setup-mac.sh` non copia le ROM: collega le cartelle.

## Tasti sul Mac

| Console | Tastiera | Controller |
|---|---|---|
| Croce direzionale | Frecce | Croce |
| A / B / X / Y | X / Z / S / A | Disposizione Nintendo (A a destra) |
| L / R | Q / W | Dorsali |
| L2 / R2 | 1 / 2 | Grilletti |
| Start / Select | Invio / Shift destro | Start / Back |
| Menu | Esc | Tasto centrale |

Nel launcher:
- **A** gioca, **X** apre la scheda del gioco, **Y** preferito, **Start** impostazioni;
- **L/R** cambiano scheda nella libreria;
- **Select** apre il pannello Filtri: ricerca con tastiera a schermo, genere, collezione,
  ordinamento (nome, anno, più giocati, giocati di recente);
- nella scheda del gioco **X** apre le opzioni: aggiungere o togliere il gioco dalle
  collezioni, crearne una nuova, nascondere il gioco.

### Collezioni e giochi nascosti

Una collezione è un file `System/collections/<nome>.txt` con una riga `core<TAB>rom` per
gioco, come i preferiti. Le collezioni con almeno un gioco compaiono come schede nella
libreria e come categorie nel layout a barra. I giochi nascosti finiscono in
`System/hidden.txt`: spariscono ovunque e si ritrovano in **Filtri → Collezione → Nascosti**,
da dove si possono rendere di nuovo visibili. Una collezione si cancella togliendo il file.

### Due giocatori

I comandi della console e la tastiera sono il giocatore 1; il primo controller esterno
(Bluetooth o USB) è il giocatore 2, i successivi il 3 e il 4. Un controller è "della console"
se il nome contiene `TRIMUI` (o il testo in `TRIMUI_BUILTIN_PAD`). In **Impostazioni →
Controller** si può scegliere "Esterno = giocatore 1", per giocare da soli con il
controller esterno. Collegando o scollegando un controller compare un avviso con il numero
del giocatore. La levetta sinistra vale come croce direzionale.

### Musica di sottofondo

I file in `Music/` (MP3, OGG, FLAC, WAV, MOD/XM/IT/S3M) suonano in ordine casuale nel
launcher. **Impostazioni → Musica** regola il volume da 0 (spenta) a 10; **A** sulla stessa
voce passa al brano successivo. La musica si ferma quando parte un gioco.

In gioco:
- **Select** inserisce il gettone;
- **Esc** (Menu) premuto e rilasciato apre il menu;
- tenendo premuto Menu: **R** salva lo stato, **L** lo carica, **R2** attiva/disattiva
  l'avanti veloce (×3, senza audio), **X** salva uno screenshot in `Screenshots/`,
  **su/giù** volume, **sinistra/destra** luminosità.

Chiudere la finestra equivale a spegnere la console: la partita viene salvata e, con
"Riprendi l'ultimo gioco" attivo, riparte al prossimo avvio. Il tempo giocato e il numero di
partite finiscono in `System/stats.tsv` e compaiono nella scheda del gioco.

### Menu in gioco

| Voce | Cosa fa |
|---|---|
| Riprendi, Salva stato, Carica stato | Stato salvato in `Saves/<core>/<rom>.state` |
| Proporzioni, Filtro video, Cornice | Originale / Pixel interi / Schermo intero; i filtri di `Filters/`; cornice sì/no |
| Rotazione schermo | Normale, oppure 90° a sinistra o a destra per giocare i verticali con la console di lato (TATE); la croce direzionale ruota di conseguenza |
| Opzioni del gioco › | Opzioni del core: prima i **DIP switch** (vite, difficoltà, monete…), poi le altre. Con FBNeo "Diagnostic Input" dà accesso al **menu di servizio** del gioco |
| Comandi › | Per ogni tasto del gioco, con il nome dato dal core ("Weak Punch"…), il tasto della console da usare |
| Ricomincia, Esci al menu, Spegni | |

Proporzioni, filtro, rotazione e comandi valgono per il singolo gioco
(`Saves/<core>/<rom>.cfg`); senza file valgono quelli delle impostazioni generali. Le opzioni
del core cambiate dal menu finiscono in `Saves/<core>/<rom>.opt`; quelle comuni a tutti i
giochi si possono scrivere in `System/core-options.txt`.

## Struttura

```
src/common/    libui: finestra, disegno, testo, input, suoni, temi, impostazioni, JSON
src/launcher/  home a carosello, libreria (griglia o elenco), impostazioni, giochi
src/runner/    esecuzione dei core libretro e menu in gioco
themes/        temi di esempio: tessera (carosello) e cabinato (elenco neon)
tools/         build_index.py: dai DAT XML all'indice compatto dei titoli
sdcard/        SD simulata, con le stesse cartelle che avrà la scheda della console
```

Il launcher e il runner sono due programmi separati, avviati a turno da `run-mac.sh`. Il
launcher scrive il gioco scelto in `System/next.txt` ed esce con codice 10. Il runner esce
con 0 per tornare al menu e con 20 per spegnere. Sulla console lo stesso ciclo lo farà lo
script di avvio.

### Cartelle della SD

| Cartella | Contenuto |
|---|---|
| `Emulators/<id>.json` | Un file per emulatore (vedi sotto) |
| `Roms/FBNEO`, `Roms/MAME2003P` | ROM per emulatore, anche in sottocartelle (`neogeo/`, `cps2/`…) |
| `Bios` | Cartella di sistema dei core (il BIOS Neo Geo può stare anche accanto alle ROM) |
| `Saves/<core>` | Stati salvati e NVRAM |
| `Media/<tipo>/<rom>.png` | Immagini scaricate dallo scraper: `snap`, `title`, `marquee`, `flyer`. Si possono mettere anche a mano in `Media/<core>/<rom>.png`; senza immagini il launcher genera una copertina |
| `Linux/<gioco>/launch.sh` | Giochi Linux: `game.json` (titolo, anno, autore, `env`, `keys`) e `cover.png` facoltativi |
| `Music/` | Musica di sottofondo del launcher |
| `Filters/<id>/filter.json` | Filtri video (vedi sotto) |
| `Bezels/` | Cornici: `<core>/<rom>.png`, `<rom>.png`, `_default.png`, `_default_vertical.png` |
| `Themes/<tema>/theme.json` | Temi; `Themes/_shared/fonts` contiene i font condivisi (licenza OFL) |
| `Lang/<codice>.json` | Testi dell'interfaccia: `it`, `en`, `fr`, `es`, `de`, `pt` |
| `System` | Impostazioni, preferiti, recenti, collezioni, nascosti, `core-options.txt` per le opzioni dei core |
| `Logs` | `Arcade.log`, unico log di launcher e runner in ordine di tempo: senza porta seriale è il primo strumento di debug |

## Emulatori

Ogni emulatore è un file `Emulators/<id>.json`:

```json
{
  "name": "FinalBurn Neo",
  "core": "fbneo_libretro",
  "roms": "Roms/FBNEO",
  "extensions": ["zip", "7z"],
  "index": "Dats/fbneo.tsv",
  "order": 10
}
```

Per aggiungerne uno servono:
1. il file di configurazione;
2. il core libretro in `Cores/` (`<core>.dylib` sul Mac, `.so` sulla console);
3. la cartella delle ROM.

L'indice dei titoli (`index`) è facoltativo: senza, il titolo è il nome del file. Le
schede della libreria, la voce "Giochi doppi" e lo scraper si adattano da soli agli
emulatori presenti. I salvataggi finiscono in `Saves/<id>/`.

**Giochi senza ROM**: con Impostazioni → *Giochi senza ROM: Mostrati* la libreria mostra
anche il catalogo degli emulatori. Sono solo i giochi principali dell'indice, non cloni né
BIOS: circa 2.600 per FBNeo e 3.100 per MAME 2003+. I giochi mancanti compaiono scuriti
con l'etichetta "Senza ROM", e la scheda indica quale file copiare e dove.

## Salvaschermo e batteria

- **Salvaschermo** (Impostazioni): dopo 2 o 5 minuti senza toccare i tasti, presentazione
  delle schermate dei giochi con titolo e orologio, oppure schermo spento. Il primo tasto
  lo chiude senza fare altro.
- **Batteria**: sotto il 15% un avviso nel launcher, sotto il 5% un'etichetta rossa fissa.
  In gioco: avviso al 10%; al 4% la partita viene salvata da sola e riparte alla
  riaccensione. Per le prove: `TRIMUI_FAKE_BATTERY=4 ./build/launcher …`.

## Temi

Un tema è una cartella con `theme.json` (commenti `//` ammessi). Campi principali:

- `layout`: `carousel` (home con riquadri, libreria a griglia), `list` (elenco con anteprima) o
  `bar` (categorie in orizzontale e giochi in verticale, come il tema Onda);
- `fonts.regular`, `fonts.title`: percorsi relativi al tema. Con un font variabile si sceglie
  lo spessore con `:N` (per Nunito 3 = Regular, 6 = Bold);
- `metrics`: misure in frazioni dell'altezza dello schermo, quindi lo stesso tema si adatta
  a 1280×720 e a 1024×768;
- `variants`: una o più combinazioni di colori (`background`, `background2` per il
  gradiente, `surface`, `text`, `text_dim`, `accent`, `accent_text`, `shadow`, `line`);
- `metrics.tile_aspect`: proporzione dei riquadri (1 quadrati, 1.333 per mostrare intere le
  schermate 4:3 dei giochi arcade);
- `media.cover`: tipi di immagine per i riquadri, in ordine di preferenza (es. `["title", "snap"]`);
  `media.cover_style`: `fill` (ritaglia per riempire) o `fit` (immagine intera su fondo scuro);
  `media.preview`: immagini per l'anteprima del layout a elenco; `media.marquee`: mostra il marquee
  sopra l'anteprima;
- `animation.speed`, `animation.pulse`, `effects.scanlines`, `effects.waves` (onde animate di
  sfondo), `sound_style` (`soft` o `retro`),
  `title`, `title_uppercase`, `background_image`.

I temi possono richiamare lo stile di altre console, ma non devono contenere loghi, font,
suoni o immagini originali di altri produttori.

## Lingue

I testi stanno in `lang/<codice>.json`, copiati in `Lang/` sulla SD. Sono oggetti annidati
letti per chiave con punti (`settings.brightness`), e `_name` è il nome mostrato nel menu.
Se una chiave manca si usa l'inglese, poi la chiave stessa. Alcuni testi sono formati con
`%d`, per esempio `"info.players": "%d giocatori"`.

Per aggiungere una lingua basta copiare `en.json` con un altro codice e tradurlo: compare
da sola nelle impostazioni. La lingua scelta viene comunicata anche ai core. I generi
arrivano da ArcadeDB in inglese e si traducono con le chiavi `genre.<categoria>`
(`genre.fighter`, `genre.ball_and_paddle`). Nunito copre gli alfabeti latino e cirillico;
per giapponese, cinese o coreano servirebbe un font in più.

## Filtri video e cornici

Un filtro è una cartella `Filters/<id>/` con `filter.json`:

```json
{
  "name": { "it": "CRT", "en": "CRT" },
  "order": 50,
  "smooth": true,
  "mask": "mask.png",
  "mask_scale": "pixel",
  "mask_strength": 0.75,
  "vignette": 0.3
}
```

- `smooth`: filtro lineare sull'immagine del gioco (morbida) invece dei pixel netti.
- `mask`: piccola PNG che moltiplica l'immagine (bianco = invariato). Con `mask_scale`
  `pixel` viene ripetuta una volta per pixel del gioco e ruota con lui in TATE; con
  `screen`, una volta per pixel dello schermo.
- `mask_strength`, `vignette`: intensità della maschera e scurimento dei bordi.
- `shader`: shader nel formato **GLSL di RetroArch**, così molti shader di libretro
  funzionano così come sono:
  - `.glsl`: un passaggio, un file con le sezioni `#if defined(VERTEX)` / `#elif defined(FRAGMENT)`,
    attributi `VertexCoord`/`TexCoord`, uniform `MVPMatrix`, `FrameCount`, `OutputSize`,
    `TextureSize`, `InputSize`, `Texture`;
  - `.glslp`: catena di più passaggi (`shaders`, `shaderN`, `scale_typeN` source/viewport/absolute,
    `scaleN`, `filter_linearN`, `aliasN`, `frame_count_modN`, `textures` per le immagini
    aggiuntive, `parameters`). Ogni passaggio vede anche l'immagine originale (`OrigTexture`,
    `OrigInputSize`, `OrigTextureSize`) e le uscite dei passaggi precedenti
    (`PassPrevNTexture` o `<alias>Texture`);
  - `#pragma parameter NOME "Descrizione" predefinito minimo massimo passo`: il parametro
    compare nel menu in gioco (**Parametri shader**) e si regola per singolo gioco.

  Il runner usa il renderer OpenGL sul Mac e OpenGL ES 2 sulla console. Se lo shader non
  compila, il gioco si vede senza shader e l'errore finisce nel log.

Inclusi:
- Nitido, Morbido, Scanline, Griglia LCD, CRT: generati da `tools/make_assets.py`;
- **Pixel nitidi**, **CRT curvo** (5 parametri) e **CRT con bagliore** (preset a 3 passaggi):
  shader, con i sorgenti in `filters/pixel-nitidi`, `filters/crt-curvo`, `filters/crt-bagliore`.

Le **cornici** sono PNG con una finestra trasparente dove va il gioco, cercate in
quest'ordine:
1. `Bezels/<core>/<rom>.png`;
2. `Bezels/<rom>.png`;
3. `Bezels/_default.png` per i giochi orizzontali o `_default_vertical.png` per i verticali.

La finestra viene trovata da sola. In alternativa si indica con `"viewport": [x, y, w, h]` in
un `.json` con lo stesso nome della PNG. Le cornici predefinite sono originali e pensate per
lo schermo 16:9 della Smart Pro; sul 4:3 della Brick Pro conviene lasciarle spente.

## Giochi Linux

Ogni gioco è una cartella `Linux/<gioco>/` con:
- `launch.sh`;
- `game.json`, facoltativo: `title`, `year`, `author`, `env` (variabili d'ambiente, per
  esempio `SDL_GAMECONTROLLERCONFIG`) e `keys` (vedi sotto);
- `cover.png`, facoltativa.

Per i giochi che usano solo la tastiera, `keys` dice quale tasto premere per ogni tasto
della console:

```json
"keys": { "A": "SPACE", "B": "ESC", "START": "ENTER", "SELECT": "TAB",
          "UP": "UP", "DOWN": "DOWN", "LEFT": "LEFT", "RIGHT": "RIGHT", "L": "Q", "R": "E" }
```

Tasti della console: `UP DOWN LEFT RIGHT A B X Y L R L2 R2 SELECT START` (croce e levetta
sinistra valgono entrambe come direzioni). Tasti della tastiera: lettere, cifre, `F1`–`F12`,
`ENTER ESC SPACE TAB BACKSPACE UP DOWN LEFT RIGHT LCTRL RCTRL LSHIFT RSHIFT LALT RALT HOME
END PAGEUP PAGEDOWN INSERT DELETE PAUSE MINUS EQUAL COMMA DOT SLASH SEMICOLON APOSTROPHE`.
Il launcher la passa a `gamewatch` come `GAMEWATCH_KEYS`; `gamewatch` crea una tastiera
virtuale (uinput) e preme quei tasti, come gptokeyb di PortMaster. I codici dei tasti della
console seguono lo standard Linux dei gamepad; se il controller della console ne usa altri si
correggono in `System/linux-env.sh` con `export GAMEWATCH_PAD="A=0x131,B=0x130"`. Provato in
un container Linux con un gamepad virtuale; sul Mac non fa nulla.

Il ciclo di avvio legge le variabili comuni da `System/linux-env.sh` e quelle del gioco, poi
avvia `launch.sh` tramite `gamewatch`. `gamewatch` mette il gioco in un proprio gruppo di
processi e, sulla console, lo chiude con tutti i suoi figli se si tiene premuto Menu per
1,5 secondi (`--hold`, `--key` per cambiare tempo e tasti). Sul Mac si limita ad avviare e
attendere il gioco.

## Immagini dei giochi

```bash
python3 tools/scrape.py sdcard
```

Oppure dal launcher: **Impostazioni → Scarica immagini**, con barra di avanzamento e
possibilità di interrompere. È la stessa logica scritta in C con libcurl, quindi funzionerà
anche sulla console via Wi-Fi.

Lo scraper cerca su **ArcadeDB** (adb.arcadeitalia.net) per nome ROM, e usa le **miniature
libretro** come riserva per titolo. Scarica schermata di gioco, schermata titolo e marquee
(`--types snap,title,marquee,flyer` per scegliere), più genere e numero di giocatori in
`Dats/arcadedb.tsv`. Funziona così:
- fa una richiesta al secondo e scarica solo ciò che manca;
- ricorda le immagini non trovate in `Media/non-trovate.txt` (`--retry` per riprovarle);
- riduce a 640 px le immagini più grandi.

Non serve nessun account. Sono servizi gratuiti della comunità: conviene non abbassare
`--delay`.

## Sulla console (app per il firmware originale)

È il "gradino 2": il sistema gira come app del menu TrimUI, senza toccare il firmware.
Si toglie cancellando le due cartelle.

### Installazione

1. Decomprimi `dist/Arcade-trimui.zip` nella radice della SD (accanto alle cartelle
   `Apps`, `Roms` e `Bios` del firmware). Si aggiungono:
   - `Apps/Arcade/`: icona, `launch.sh`, programmi;
   - `Arcade/`: temi, lingue, filtri, core, indici dei giochi; qui vanno ROM, BIOS, musica.
2. Copia le ROM in `Arcade/Roms/FBNEO` e `Arcade/Roms/MAME2003P`. Il BIOS Neo Geo
   (`neogeo.zip`) va in `Arcade/Bios` o accanto ai giochi Neo Geo. In alternativa, in
   `Arcade/Emulators/<id>.json` la voce `roms` accetta un percorso assoluto, per esempio
   `"/mnt/SDCARD/Roms/FBNEO"`.
3. Facoltativo: copia `sdcard/Media` del Mac in `Arcade/Media` per avere subito le immagini
   (oppure `toolchain/package.sh --media`), e la musica in `Arcade/Music`.
4. Sulla console: **Apps → Arcade**. Dal tasto di spegnimento del launcher si sceglie
   **Esci** (torna al menu TrimUI) o **Spegni**.

Aggiornando con un nuovo zip, impostazioni, preferiti, collezioni e salvataggi restano.

### Cosa fa sulla console

- Luminosità (retroilluminazione via `/dev/disp`), volume (`amixer`, "digital volume") e
  profilo CPU (1,2 / 1,6 / 2,0 GHz) sono veri; batteria e ricarica da
  `/sys/class/power_supply/axp2202-*`. Percorsi come in MinUI.
- Il modello (Smart Pro 1280×720, Brick e Brick Pro 1024×768) si riconosce da solo dal
  nome nel menu originale. Lo stesso pacchetto vale per tutte e tre: i firmware v1.1.1
  (analizzati, vedi lo studio §2.0) hanno glibc 2.33 e SDL2 2.30.8, compatibili con i
  programmi. La finestra ha la misura del modello: lo schermo della Smart Pro è fisicamente
  verticale e lo ruota l'SDL2 di TrimUI.
- I tasti della console li legge `trimui_inputd` del firmware, che li presenta come un
  controller Xbox 360 ("TRIMUI Player1", verificato sulla Brick Pro): SDL lo riconosce da
  solo e A a destra conferma, Menu è il tasto Guide. Se il firmware lo presentasse come
  joystick generico, vale la mappatura di MinUI (A=1 B=0 X=3 Y=2 L=4 R=5 Select=6 Start=7
  Menu=8, croce sul cappello, L2/R2 sugli assi 2 e 5). Se qualcosa non torna, rinominare
  `Arcade/System/device-env.sh.esempio` in `device-env.sh` e attivare `TRIMUI_JOYDEBUG=1`:
  il log scrive il numero di ogni pulsante premuto; poi si corregge con `TRIMUI_JOYMAP`.
- Log: `Arcade/Logs/avvio.log` (script e uscite dei programmi) e `Arcade/Logs/Arcade.log`.

### Compilazione

Tutto in Docker con il kit ufficiale TrimUI (Linaro GCC 7.5, librerie del firmware):

```bash
docker build --platform linux/amd64 -t trimui-tg5040 -f toolchain/Dockerfile deps/trimui-sdk/dl
docker volume create trimui-build
docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro trimui-tg5040 sh /trimui/toolchain/build-deps.sh
docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui -w /trimui/frontend trimui-tg5040 make -f Makefile.tg5040 check
docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro trimui-tg5040 sh /trimui/toolchain/build-cores.sh
toolchain/package.sh
```

(dalla cartella `trimui`; gli archivi del kit vanno in `deps/trimui-sdk/dl`). Il compilatore
è x86-64 e sul Mac gira emulato: i core richiedono parecchi minuti. La console ha glibc
2.23: `make check` mostra la versione più alta richiesta dai programmi (oggi 2.17).
Dal firmware si usano SDL2 2.26 (con il driver della GPU), OpenSSL e zlib; SDL2_ttf,
SDL2_image, SDL2_mixer e libcurl sono compilate a parte e incluse nei programmi, perché
quelle del firmware sono troppo vecchie.

## Core (in `../deps`)

```bash
git clone --depth 1 https://github.com/libretro/FBNeo.git fbneo
make -C fbneo/src/burner/libretro -j6
git clone --depth 1 https://github.com/libretro/mame2003-plus-libretro.git mame2003-plus
make -C mame2003-plus -j6
```

Su macOS recente mame2003-plus non compila senza una correzione: in
`src/lib/zlib/zutil.h` va tolta la riga `define fdopen(fd,mode) NULL`.

## Prove automatiche

Launcher e runner accettano `--auto` con una sequenza di tasti, attese e screenshot, e
girano senza finestra visibile:

```bash
./build/launcher --root sdcard --auto "wait:30,right,right,shot:/tmp/home.png,quit"
./build/runner --root sdcard --core sdcard/Cores/fbneo_libretro.dylib --rom sdcard/Roms/FBNEO/neogeo/mslug.zip --auto "wait:700,menu,shot:/tmp/menu.png,quit"
```

`down:<tasto>` e `up:<tasto>` tengono premuto e rilasciano un tasto, per provare le
combinazioni (es. `down:menu,up,up,up:menu`).

## Limiti del prototipo

- Luminosità, Wi-Fi e profilo CPU sono simulati: sulla console agiranno su retroilluminazione,
  rete e frequenza della CPU.
- I valori delle opzioni dei core restano nella lingua del core (di solito inglese).
- Giochi Linux: i tasti come tastiera non gestiscono il mouse (alcuni giochi PC lo
  richiedono); i codici dei tasti della console vanno verificati sul dispositivo.
- Gli shader sono provati sul Mac (OpenGL 2.1); su OpenGL ES 2, il renderer della console,
  vanno verificati sul dispositivo. Non sono supportati il formato slang (Vulkan) né le
  texture del frame precedente (`PrevTexture`).
- Il catalogo dei giochi senza ROM mostra le immagini solo se presenti: lo scraper scarica
  quelle dei giochi che hai (per tutto il catalogo servirebbero ore e qualche GB).
- La storia dei giochi arriva da ArcadeDB solo in inglese.

## Licenze e distribuzione

Il progetto è pensato per uso personale. Se un giorno si volessero pubblicare le immagini o il
pacchetto, va tenuto conto di cosa contengono:

| Parte | Licenza | Cosa comporta |
|---|---|---|
| Pezzi di avvio, kernel, moduli e librerie GPU, `trimui_inputd`, SDL2 e librerie del firmware TrimUI | proprietari / non dichiarati | Ridistribuirli richiede il permesso di TrimUI: meglio far estrarre i pezzi dal firmware ufficiale a chi installa (come fanno gli script di `toolchain/`) |
| FinalBurn Neo | licenza FBNeo, **non commerciale** | Niente vendita né uso commerciale |
| mame2003-plus | licenza MAME storica, **non commerciale** | Come sopra |
| BusyBox, dosfstools (`fsck.fat`), kernel Linux | GPL | Chi distribuisce i binari deve offrire i sorgenti |
| SDL2_ttf, SDL2_image, SDL2_mixer, libcurl | zlib, MIT/curl | Libere, con nota di copyright |
| Font dei temi | SIL OFL | Liberi, con la licenza allegata |
| Certificati (`cacert.pem`) | MPL 2.0 (Mozilla) | Liberi |
| Valori audio presi da Knulli | dati di configurazione | Citare la fonte |

Le ROM e i BIOS non fanno parte del progetto: ognuno usa i propri.
