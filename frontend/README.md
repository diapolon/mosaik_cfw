# Frontend di Mosaik: launcher, runner e app

Launcher con temi grafici, runner libretro e app di Mosaik per TrimUI Smart Pro (TG5040) e
Brick Pro (TG4040): arcade, console, computer e giochi Linux. Gli emulatori non sono nel
sistema: ogni core è un pacchetto dello Store (`toolchain/package.sh`, `toolchain/make-store.py`).
Sul Mac gira in una finestra che simula lo schermo della console. Il progetto completo è descritto in
[../STUDIO-CFW-TG5040.md](../STUDIO-CFW-TG5040.md).

## Avvio rapido

```bash
make
FBNEO_ROMS=/percorso/delle/rom/fbneo MAME_ROMS=/percorso/delle/rom/mame2003plus ./setup-mac.sh
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

I file in `Music/Menu` (MP3, OGG, FLAC, WAV, MOD/XM/IT/S3M) suonano in ordine casuale nel
launcher; il resto di `Music/` è del Lettore musicale. La musica è spenta di partenza:
**Impostazioni → Schermo e suono → Musica** la accende e ne regola il volume; **A**
sulla stessa voce passa al brano successivo. Si ferma quando parte un gioco, e dopo il Lettore
musicale resta in silenzio fino allo spegnimento.

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
| Riprendi, Salva stato, Carica stato | Stato nello slot in uso: `Saves/<console o core>/<gioco>.state` (`.2.state`, `.3.state`) |
| Salvataggi › | I tre slot, il salvataggio automatico e la copia precedente, con anteprima e data |
| Disco | Solo per i giochi su più dischi (PlayStation con `.m3u`): cambia disco aprendo e richiudendo il vassoio; grigio con un disco solo |
| Proporzioni, Filtro video, Cornice | Originale / Pixel interi / Schermo intero; i filtri di `Filters/`; cornice sì/no |
| Rotazione schermo | Normale, oppure 90° a sinistra o a destra per giocare i verticali con la console di lato (TATE); la croce direzionale ruota di conseguenza |
| Opzioni del gioco › | Opzioni del core: prima i **DIP switch** (vite, difficoltà, monete…), poi le altre. Con FBNeo "Diagnostic Input" dà accesso al **menu di servizio** del gioco |
| Comandi › | Per ogni tasto del gioco, con il nome dato dal core ("Weak Punch"…), il tasto della console da usare |
| Ricomincia, Esci al menu, Spegni | |

Proporzioni, filtro, rotazione e comandi valgono per il singolo gioco
(`Saves/<console o core>/<gioco>.cfg`); senza file valgono quelli delle impostazioni generali.
Le opzioni del core cambiate dal menu finiscono in `.opt` accanto; quelle comuni a tutti i
giochi si possono scrivere in `System/core-options.txt`.

**Dove finiscono i file di un gioco.** Per le console la cartella è quella della console
(`Saves/md`, `Saves/gb`, `Saves/ps`…: l'id di `Emulators/<id>.json`), per gli arcade quella del
core (`Saves/fbneo`). Il nome è l'identità del gioco: il nome del file, oppure
`nome [sottocartella]` / `nome [zip]` per due ROM con lo stesso nome nello stesso emulatore. Il
salvataggio a batteria (`.srm`, `.rtc`; per il Neo Geo Pocket anche la flash, con la patch del
core) si scrive ogni 10 s se cambia e a ogni uscita, sospensione o batteria scarica. I file
delle versioni fino alla 0.9.17 in `Saves/<core>/` si copiano nelle cartelle nuove al primo
avvio (`launcher/savesplit.c`, registro `.spostati-per-console.txt`).

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
con 0 per tornare al menu e con 20 per spegnere. Sulla console lo stesso ciclo lo fa
`m2/system-init`.

### Cartelle della SD

| Cartella | Contenuto |
|---|---|
| `Emulators/<id>.json` | Un file per emulatore (vedi sotto) |
| `Roms/<cartella>` | ROM per emulatore (`roms` del suo JSON), anche in sottocartelle (`neogeo/`, `cps2/`…): `FBNEO`, `MAME2003P`, `NES`, `SNES`, `MD`, `SMS`, `GG`, `PCE`, `GB`, `GBC`, `GBA`, `NGP`. Il launcher crea all'avvio quelle che mancano |
| `Bios` | Cartella di sistema dei core: `neogeo.zip` (può stare anche accanto alle ROM), `gba_bios.bin` facoltativo |
| `Saves/<console>` | Per le console l'id dell'emulatore (`md`, `gb`, `ps`…), per gli arcade quello del core (`fbneo`): stati salvati, NVRAM, impostazioni del gioco e salvataggi a batteria (`<rom>.srm`, `<rom>.rtc` per l'orologio della cartuccia; Beetle NeoPop scrive il suo `<rom>.flash` all'uscita) |
| `Media/<tipo>/<rom>.png` | Immagini scaricate dallo scraper: `snap`, `title`, `marquee`, `flyer`, `box` (scatole, solo console). Si possono mettere anche a mano in `Media/<core>/<rom>.png`; senza immagini il launcher genera una copertina |
| `Dats` | Indici dei titoli: `<id>.tsv` arcade (con `<id>.roms`) dai DAT; `<id>.tsv` delle console, `arcadedb.tsv` (genere, giocatori), `history/` e `libretro/` scritti dallo scraper sulla console |
| `Linux/<gioco>/launch.sh` | Giochi Linux: `game.json` (titolo, anno, autore, `env`, `keys`, `cpu`) e `cover.png` facoltativi |
| `Music/` | `Music/Menu`: musica del menu (spenta di partenza); il resto è del Lettore musicale |
| `Filters/<id>/filter.json` | Filtri video (vedi sotto) |
| `Bezels/` | Cornici: `<core>/<rom>.png`, `<rom>.png`, `_default.png`, `_default_vertical.png` |
| `Themes/<tema>/theme.json` | Temi; `Themes/_shared/fonts` contiene i font condivisi (licenza OFL) |
| `Lang/<codice>.json` | Testi dell'interfaccia: `it`, `en`, `fr`, `es`, `de`, `pt` |
| `System` | `settings.json`, preferiti, recenti, collezioni, nascosti, `core-options.txt` per le opzioni dei core |
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

Una console ha qualche campo in più:

```json
{
  "name": "Game Boy",
  "core": "gambatte_libretro",
  "roms": "Roms/GB",
  "extensions": ["gb", "dmg", "zip"],
  "kind": "console",
  "art": "handheld",
  "short": "Game Boy",
  "thumbnails": "Nintendo - Game Boy",
  "order": 50
}
```

| Campo | Significato |
|---|---|
| `name` | Unico obbligatorio. Gli altri hanno un valore predefinito dall'id (`<id>_libretro`, `Roms/<ID>`, `Dats/<id>.tsv`, zip e 7z) |
| `index` | Indice dei titoli; senza, il titolo è il nome del file |
| `kind` | `"console"`: niente ArcadeDB, doppioni né cabinato; lo scraper usa il CRC della ROM |
| `thumbnails` | Nome della raccolta di miniature libretro e del sistema in libretro-database |
| `short` | Sigla sulla cartuccia nella schermata Piattaforme (predefinita: l'id in maiuscolo) |
| `art` | Disegno in Piattaforme: `handheld`, `handheld-wide`, `disc`, `disc-white`, `nes`, `snes`, `md`, `sms`, `pce`; altrimenti una console da casa generica |
| `maker`, `year` | Produttore e anno della console, sulle schede del layout galleria |
| `bios` | Nomi dei BIOS in `Bios/`, ne basta uno (Sega CD: uno per regione): se mancano tutti, la diagnosi del gioco che non parte lo dice |
| `cpu` | Frequenza della CPU mentre si gioca: 1 = 816, 2 = 1200, 3 = 1608, 4 = 2000 MHz (il massimo); senza, quella del profilo scelto nelle impostazioni. Sulla Smart Pro l'interruttore FN (prestazioni massime) vale di più; la Brick Pro non ce l'ha |
| `guide` | Guida, dal sistema 0.9.105: `{"it": ["riga", ...], "en": [...]}`, poche righe con le cose da sapere (come si apre il menu dell'emulatore, come si esce, BIOS, cartelle). SELECT in Piattaforme e nella scheda dello Store (make-store.py la copia nel catalogo); per gli emulatori con `launch` compare anche da sola al primo avvio, una volta (`System/guide-viste.txt`). Lingua della console, poi inglese |
| `guide_pad` | Disegno dei comandi nella guida, dal sistema 0.9.106: `[[etichetta, tasti], ...]`; l'etichetta è un testo, `"@chiave"` della sezione `controls` delle lingue o `{"it": ..., "en": ...}`; i tasti sono `A B X Y L1 R1 L2 R2 SELECT START MENU @dpad @lstick @rstick`, più d'uno separati da ` · `. Senza, la guida disegna i `controls` (N64, Flycast); le levette non si disegnano sulla Brick Pro |
| `planned` | `true`: console non ancora supportata, solo in grigio in Piattaforme («In arrivo»), senza core né cartella (oggi `dc`) |

Giochi su più dischi: un `.m3u` (estensione dell'emulatore) elenca i dischi, uno per riga,
nella stessa cartella; in elenco va solo il `.m3u`, non i dischi che elenca.

Per aggiungere un emulatore servono il file di configurazione, il core libretro in `Cores/`
(`<core>.dylib` sul Mac, `.so` sulla console) e la cartella delle ROM. Le schede della
libreria, la voce "Giochi doppi" e lo scraper si adattano da soli agli emulatori presenti.
Se il core non legge gli zip (non c'è `zip` tra le sue estensioni), il runner estrae in `/tmp`
la prima voce con un'estensione giusta: un gioco per zip. I core si compilano con
`toolchain/build-cores.sh`; i sorgenti dei core GPL/MPL sono allegati alle release.

**Giochi senza ROM**: con Impostazioni → *Giochi senza ROM: Mostrati* la libreria mostra
anche il catalogo degli emulatori. Sono solo i giochi principali dell'indice, non cloni né
BIOS: circa 2.600 per FBNeo e 3.100 per MAME 2003+. I giochi mancanti compaiono scuriti
con l'etichetta "Senza ROM", e la scheda indica quale file copiare e dove.

## Impostazioni

Stanno in `System/settings.json`, riscritto per intero a ogni cambio. Tra le chiavi:
`resume_last` (riprendi l'ultimo gioco all'accensione), `autosave_exit` (salvataggio
automatico anche uscendo al menu, spento di base), `box_covers` (per le console la scatola come
copertina, se scaricata; acceso di base), `speaker_rest` (amplificatore a riposo, prova per il
fruscio della Brick Pro: 0 sempre acceso, 1 spento con `/sys/class/speaker/mute`, 2 con
`HpSpeaker Switch`), `timezone` (anche in `/run/arcade-tz` e `System/fuso-orario.txt` per gli
script), `update_channel`, `wifi`, `debug_level`. Una chiave mancante prende il valore
predefinito.

## Salvaschermo e batteria

- **Salvaschermo** (Impostazioni): dopo 2 o 5 minuti senza toccare i tasti, presentazione
  delle schermate dei giochi con titolo e orologio, oppure schermo spento. Il primo tasto
  lo chiude senza fare altro.
- **Batteria**: sotto il 15% un avviso nel launcher, sotto il 5% un'etichetta rossa fissa.
  In gioco: avviso al 10%; al 4% la partita viene salvata da sola e riparte alla
  riaccensione. Per le prove: `TRIMUI_FAKE_BATTERY=4 ./build/launcher …`.

## Temi

Un tema è una cartella con `theme.json` (commenti `//` ammessi). Campi principali:

- `layout`: `carousel` (home con riquadri, libreria a griglia), `list` (elenco con anteprima),
  `bar` (categorie in orizzontale e giochi in verticale, come il tema Onda) o `vetrina` (dalla
  0.9.30: home con l'illustrazione grande della voce scelta su una fascia obliqua, il nome a
  lettere piene e la striscia delle piattaforme in basso, come il tema Copertina dello Store; la
  libreria resta a griglia) o `galleria` (dalla 0.9.32: schede Preferiti, Recenti e Piattaforme
  in alto con L/R, il nome grande a sinistra e un carosello di schede con il disegno della
  piattaforma, produttore e anno, come il tema Stellato). Un sistema che non conosce il layout
  usa `carousel`;
- `fonts.regular`, `fonts.title`: percorsi relativi al tema. Con un font variabile si sceglie
  lo spessore con `:N` (per Nunito 3 = Regular, 6 = Bold);
- `metrics`: misure in frazioni dell'altezza dello schermo, quindi lo stesso tema si adatta
  a 1280×720 e a 1024×768;
- `variants`: una o più combinazioni di colori (`background`, `background2` per il
  gradiente, `surface`, `text`, `text_dim`, `accent`, `accent_text`, `shadow`, `line`,
  `highlight` per il titolo della vetrina, se no l'accento) e `label`, il nome della variante
  (testo o `{ "it": ..., "en": ... }`: per i temi dello Store, che non sono nei file delle lingue);
- temi dello Store: una cartella in `frontend/package/themes/<id>/` diventa il pacchetto
  `tema-<id>` (cartella Temi), con `version` e `texts` (titolo e descrizione per lingua) presi dal
  `theme.json`;
- `metrics.tile_aspect`: proporzione dei riquadri (1 quadrati, 1.333 per mostrare intere le
  schermate 4:3 dei giochi arcade);
- `media.cover`: tipi di immagine per i riquadri, in ordine di preferenza (es. `["title", "snap"]`);
  `media.cover_style`: `fill` (ritaglia per riempire) o `fit` (immagine intera su fondo scuro);
  `media.preview`: immagini per l'anteprima del layout a elenco; `media.marquee`: mostra il marquee
  sopra l'anteprima;
- `animation.speed`, `animation.pulse`, `effects.scanlines`, `effects.waves` (onde animate di
  sfondo), `effects.stars` (cielo stellato sul colore `background`, con la fascia di
  `background2` in basso), `sound_style` (`soft` o `retro`),
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

Nel sistema (`frontend/filters`): Nitido, Morbido, Scanline, Griglia LCD, CRT, generati da
`tools/make_assets.py`.

Nello Store, cartella Filtri (dalla 0.9.51): una serie è un pacchetto `filtri-<serie>`, con i
filtri in `frontend/package/filters/<serie>/Filters/<id>` e la descrizione in `pack.json`
accanto (`version`, `color`, `texts` per lingua, come le cornici). La serie `shader` ha **Pixel
nitidi**, **CRT curvo** (5 parametri) e **CRT con bagliore** (preset a 3 passaggi). Sulle schede
che li hanno già dal sistema lo Store li mostra da aggiornare; togliendo il filtro in uso le
impostazioni tornano a Nitido.

Le **cornici** sono PNG con una finestra trasparente dove va il gioco, cercate in
quest'ordine:
1. `Bezels/<core>/<rom>.png`;
2. `Bezels/<rom>.png`;
3. `Bezels/<console>/_default.png` (la console è quella di `Saves/<console>`, per gli arcade l'id dell'emulatore);
4. `Bezels/<console>/_mosaik.png`, dei pacchetti Cornici dello Store (`frontend/package/bezels`, disegnati da `tools/make_bezels.py`): lo Store scrive solo questi, quelli dell'utente vengono prima;
5. `Bezels/_default.png` per i giochi orizzontali o `_default_vertical.png` per i verticali (tutti con `_vertical` prima di `.png` per i giochi verticali).

Per ogni nome si prova prima `<nome>-<L>x<A>.png` con la misura dello schermo (`_mosaik-1024x768.png` sulla Brick Pro), poi `<nome>.png`.

La finestra viene trovata da sola. In alternativa si indica con `"viewport": [x, y, w, h]` in
un `.json` con lo stesso nome della PNG. Le cornici predefinite sono originali e pensate per
lo schermo 16:9 della Smart Pro; sul 4:3 della Brick Pro conviene lasciarle spente.

## Giochi Linux

Ogni gioco è una cartella `Linux/<gioco>/` con:
- `launch.sh`;
- `game.json`, facoltativo: `title`, `year`, `author`, `env` (variabili d'ambiente, per
  esempio `SDL_GAMECONTROLLERCONFIG`), `keys` (vedi sotto) e `cpu` (frequenza, vedi sotto;
  dal sistema 0.9.104, quelli prima la ignorano);
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

### Frequenza della CPU

`"cpu"` in `game.json` vale come per gli emulatori: 1 = 816, 2 = 1200, 3 = 1608, 4 = 2000 MHz.
Il launcher la imposta prima di avviare il gioco e la passa come `ARCADE_CPU_KHZ`, così anche il
menu di Mosaik sopra il gioco la tiene; tornando al launcher si torna al profilo delle
impostazioni.

Un gioco che vuole cambiarla da sé, per esempio da una sua voce «Prestazioni», scrive le due
voci di cpufreq (il gioco gira come root; frequenze accettate in `scaling_available_frequencies`:
816000, 1200000, 1608000, 2000000) e la stessa frequenza in kHz in `/run/cpu-gioco`:

```sh
echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 2000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
echo 2000000 > /run/cpu-gioco
```

Senza `/run/cpu-gioco` il menu e la barra del volume di Mosaik, quando si aprono, rimettono
`"cpu"` o il profilo. Il file vale solo per il gioco in corso: il sistema lo toglie prima e dopo
ogni gioco. Sulla Smart Pro l'interruttore FN (prestazioni massime) vale sempre di più; la Brick
Pro non ce l'ha. Dal sistema 0.9.104.

## Immagini dei giochi

```bash
python3 tools/scrape.py sdcard
```

Oppure dal launcher: **Impostazioni → ROM e immagini → Scarica immagini**, con barra di
avanzamento e possibilità di interrompere. Serve l'app «Scarica immagini» dello Store
(`src/apps/scraper.c`, in C con libcurl, via Wi-Fi sulla console), che il launcher avvia con
`launcher/scrape_client.c`. Lo script Python fa solo gli arcade.

**Arcade.** Lo scraper cerca su **ArcadeDB** (adb.arcadeitalia.net) per nome ROM, e usa le
**miniature libretro** come riserva per titolo. Scarica schermata di gioco, schermata titolo e
marquee (`--types snap,title,marquee,flyer` per scegliere), più genere e numero di giocatori in
`Dats/arcadedb.tsv` e la storia in `Dats/history/`.

**Console.** Calcola il CRC della ROM (anche dentro lo zip; senza l'intestazione iNES e quella
da 512 byte dei copiatori SNES e delle HuCard) e lo cerca nei file `metadat` di
**libretro-database** (CC BY-SA 4.0): anno, editore, sviluppatore, genere, giocatori. I file
si tengono in `Dats/libretro/<campo>/<sistema>.dat` e si riscaricano dopo 30 giorni. Il
risultato va nell'indice della console `Dats/<id>.tsv` (stesso formato degli indici arcade; il
nome No-Intro diventa il titolo) e genere e giocatori in `Dats/arcadedb.tsv`. Le immagini
vengono dalle miniature libretro del sistema (`thumbnails`): `Named_Snaps`, `Named_Titles` e
`Named_Boxarts` in `Media/box`. Pacchetto e aggiornamenti non portano mai questi file.

In entrambi i casi:
- una richiesta al secondo, e si scarica solo ciò che manca;
- le immagini non trovate sono ricordate in `Media/non-trovate.txt` (`--retry` per riprovarle);
- le immagini più grandi si riducono a 640 px.

Non serve nessun account: sono servizi gratuiti della comunità, conviene non abbassare
`--delay`. Le fonti e le licenze dei dati scaricati sono in `licenses/THIRD-PARTY.md`.

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
è x86-64 e sul Mac gira emulato: i core richiedono parecchi minuti. Il firmware v1.1.1 ha
glibc 2.33: `make check` (`toolchain/check-glibc.sh`) mostra la versione più alta richiesta da
programmi, app e core (oggi 2.17 per i programmi) e fallisce se supera la 2.33. Si compila con
l'SDL2 2.26 del kit; sulla console gira quella del firmware (2.30.8, con il driver della GPU).
Dal firmware si usano anche OpenSSL e zlib; SDL2_ttf,
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

I core delle console stanno accanto, come cloni dei repository libretro: `libretro-fceumm`,
`snes9x`, `Genesis-Plus-GX`, `beetle-pce-fast-libretro`, `gambatte-libretro`, `mgba`,
`beetle-ngp-libretro`, `pcsx_rearmed` (con il sottomodulo `frontend/libpicofe`). Per la console
li compila tutti `toolchain/build-cores.sh`, nel container del kit, applicando le patch di
`toolchain/patches/` (flash del Neo Geo Pocket come memoria di salvataggio; nome di registro
`lr` per le binutils del kit in PCSX ReARMed); sul Mac si compilano con il `Makefile` libretro
di ciascuno. I sorgenti con le patch vanno allegati alle release (`dist/release/sources`).

## Prove automatiche

Launcher e runner accettano `--auto` con una sequenza di tasti, attese e screenshot, e
girano senza finestra visibile:

```bash
./build/launcher --root sdcard --auto "wait:30,right,right,shot:/tmp/home.png,quit"
./build/runner --root sdcard --core sdcard/Cores/fbneo_libretro.dylib --rom sdcard/Roms/FBNEO/neogeo/mslug.zip --auto "wait:700,menu,shot:/tmp/menu.png,quit"
```

`down:<tasto>` e `up:<tasto>` tengono premuto e rilasciano un tasto, per provare le
combinazioni (es. `down:menu,up,up,up:menu`). `toast:<testo>` mostra un avviso (senza virgole),
`route` fa come un cambio d'uscita audio (cuffie Bluetooth collegate o scollegate).

`m2/test-ui.sh` compila il launcher in Alpine e lo fa girare col video finto di SDL
(`SDL_VIDEODRIVER=dummy`): schermate confrontate pixel per pixel, nessun Mac necessario.

## Limiti noti

- Sul Mac luminosità, Wi-Fi e profilo CPU sono simulati: sulla console agiscono su
  retroilluminazione, rete e frequenza della CPU.
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

Il codice del progetto è sotto LGPL v3 (`COPYING`, `COPYING.LESSER`, `LICENSE-NOTICE`); le
licenze dei componenti esterni sono in `licenses/THIRD-PARTY.md`.

Le ROM e i BIOS non fanno parte del progetto: ognuno usa i propri.
