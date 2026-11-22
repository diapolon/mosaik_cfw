<p align="center">
  <img src="docs/images/logo.png" width="444" alt="Mosaik cfw">
</p>

<h3 align="center">Tutti i tuoi giochi, in tasca.</h3>

<p align="center">
  Un sistema di gioco per <b>TrimUI Smart Pro</b> e <b>Brick Pro</b>: arcade, console e giochi Linux.<br>
  Parte dalla microSD e non tocca la memoria interna della console.
</p>

<p align="center">
  <a href="https://www.mosaikcfw.org/it/">Sito</a> ·
  <a href="https://github.com/diapolon/mosaik_cfw/releases">Scarica</a> ·
  <a href="#installazione">Installazione</a> ·
  <a href="#caricare-i-giochi">Caricare i giochi</a> ·
  <a href="#comandi">Comandi</a> ·
  <a href="#problemi-frequenti">Problemi frequenti</a> ·
  <a href="https://github.com/diapolon/mosaik_cfw/wiki/Italiano">Wiki</a> ·
  <a href="#sostieni-mosaik">Sostieni</a> ·
  <a href="README.md">English</a>
</p>

<p align="center">
  <img src="docs/images/it/home.png" width="640" alt="La schermata iniziale di Mosaik">
</p>

> **Stato:** versione 0.9. **Smart Pro e Brick Pro: provate.**
>
> **In arrivo:** R36S. Una prima versione è in preparazione e non si può ancora installare.

## Cosa c'è dentro

- **Emulatori dallo Store**: il sistema resta piccolo e installi solo quello che usi, dallo **Store** nella home. Ogni pacchetto è firmato; **Elimina** ti fa scegliere cosa togliere: l'emulatore, i suoi salvataggi e dati, le sue ROM e immagini (dalla 0.9.68; prima **Archivia** ed **Elimina**).
- **FinalBurn Neo, MAME 2003-Plus e MAME 2010**: Neo Geo, CPS1, CPS2 e gran parte dei classici arcade.
- **NES, Super Nintendo, Mega Drive, Master System, Game Gear, PC Engine, Game Boy, Game Boy Color, Game Boy Advance e Neo Geo Pocket**, con i salvataggi a batteria scritti ogni 10 secondi (anche l'orologio dei Pokémon), salvataggi separati per ogni console e le ROM zippate.
- **PlayStation** (PCSX ReARMed) con le levette del DualShock: dischi `.chd`, `.cue` e `.pbp`, giochi su più dischi con un file `.m3u` e il cambio disco nel menu di gioco.
- **Dreamcast** con [Deadcast](https://github.com/tyrannotorus/emulator-dreamcast-deadcast) sulla Brick Pro e con [Flycast](https://github.com/flyinghead/flycast) sulla Smart Pro (sperimentale): lo Store propone a ogni console il suo.
- **Altre console nello Store**: Sega CD, PC Engine CD, SG-1000, Atari 2600 e 7800, ColecoVision, Intellivision, Lynx, WonderSwan, Pokémon mini, Virtual Boy, Nintendo 64 (Mupen64Plus, con gli stati salvati), PSP (PPSSPP).
- **Computer**: ScummVM, ZX Spectrum, MSX, Commodore 64, DOS (DOSBox Pure) e Amiga, con la tastiera a schermo dei loro emulatori.
- **Copertine e informazioni anche per le console**: scatola, titolo, anno, editore, genere e giocatori, trovati dalla ROM stessa.
- **Giochi Linux**, con menu sopra al gioco e sospensione; dallo Store (0.9.58, sperimentali): Freedoom, Mr.Boom e Diablo (con i file del gioco originale).
- **Avvio in circa 5 secondi**, sospensione con il tasto di accensione.
- **Wi-Fi integrato** per copertine, storia dei giochi, ora e aggiornamenti.
- **Cuffie e casse Bluetooth** (dalla 0.9.88): tutto il suono va a loro, con il loro volume, e si ricollegano da sole all'accensione.
- **Trasferimento file via Wi-Fi** da computer o telefono: inquadri il QR o apri `mosaik.local`, poi carichi ROM e scarichi salvataggi senza togliere la scheda.
- **Backup e ripristino**: un solo zip con salvataggi, impostazioni, BIOS e, se vuoi, i giochi; lo ripristini tutto o solo le parti che scegli, anche su una scheda nuova o sull'altro modello.
- **App dallo Store**: Esplora file, Immagini e fumetti (anche a icone), Lettore musicale, Video, Test della SD, Spazio della SD, Diagnostica, Copertine e info e Scarica file.
- **Cambia gioco** (Select): le ultime partite in fila con l'immagine del punto in cui le hai lasciate; scegli e riprendi da lì, senza passare dal menu.
- **Pannello rapido** (Menu): batteria, temperatura, tempo di gioco, Wi-Fi, Bluetooth, altoparlante, LED, musica, cattura, luminosità e volume.
- **Collezioni automatiche**: Da finire, Partite brevi, Due giocatori, Mai giocati, accanto a preferiti e collezioni tue.
- **Gioco a caso** come prima voce di ogni lista di giochi, e **un gioco che parte all'accensione**.
- **Pagina Batteria** con il grafico delle ultime ore, il tempo rimasto e quanto è costata l'ultima sospensione; l'**ora presa da internet** quando il Wi-Fi è acceso.
- **Temi con varianti di colore** (Tessera di base; Cabinato, Onda, Copertina e Stellato nello Store), sei lingue, LED, filtri video e shader, cornici.
- **Aggiornamenti firmati** via Wi-Fi, tutto o niente: se uno si interrompe o la versione nuova non parte, la console torna a tutti i file di prima.
- **Impostazioni per una console intera o per un gioco** (Piattaforme, Y; **Cambia per** nel menu di gioco), profilo CPU per ogni gioco e avviso nella home quando c'è un aggiornamento.
- **Diagnosi dei giochi**: quando un gioco non parte, ti dice cosa manca e dove copiarlo.
- **Una guida per ogni emulatore** (dalla 0.9.105): **Select** in Piattaforme mostra la tua console, Smart Pro o Brick Pro, con cosa fa ogni tasto, e la cartella dei giochi; il primo gioco di un emulatore con un menu suo (PSP, Nintendo 64, Dreamcast) la mostra da solo. Ce l'hanno anche il Lettore musicale (dalla 1.12) e Video (dalla 1.7).
- **Tre slot di salvataggio per gioco** con anteprima e data, più un salvataggio automatico con copia precedente.

## Cosa funziona

Ogni versione viene provata a mano su console vere prima di uscire. Ecco lo stato attuale (0.9.110). Nello Store i pacchetti non ancora provati hanno la fiaschetta **Sperimentale**.

**Provato su tutte e due le console (Smart Pro e Brick Pro)**
- **Sistema**: avvio dalla scheda in circa 5 secondi, sospensione e ripresa, aggiornamenti firmati via Wi-Fi (tutto o niente, con il ritorno indietro), trasferimento file via Wi-Fi, backup e ripristino, Store, temi, cornici, filtri, impostazioni per una console o per un solo gioco, sei lingue.
- **Arcade**: FinalBurn Neo e MAME 2003-Plus.
- **Console**: Super Nintendo, Mega Drive e Nintendo 64 (con i salvataggi di stato).
- **Cuffie e casse Bluetooth** (dalla 0.9.88): abbinamento, suono e volume nelle cuffie, ricollegamento all'accensione e dopo la sospensione.

**Provato su una console sola**
- **Dreamcast con Deadcast**: Brick Pro (lo Store non lo propone sulla Smart Pro).
- **PSP con PPSSPP**: Brick Pro (**Menu** premuto breve per il menu di PPSSPP, tenuto per quello di Mosaik).
- **Test della SD**: Brick Pro.
- **Neo Geo Pocket, ScummVM e Mr.Boom**: Brick Pro.

**Sperimentale: si possono installare, non sono ancora stati provati**
- **Console**: NES, Game Boy e Game Boy Color, Game Boy Advance, PC Engine, PlayStation, Atari 2600, 7800 e Lynx, ColecoVision, Intellivision, WonderSwan, Virtual Boy, Pokémon mini, Dreamcast con Flycast (solo Smart Pro).
- **Arcade**: MAME 2010.
- **Computer**: ZX Spectrum, MSX, Commodore 64, DOS, Amiga.
- **Giochi Linux**: Freedoom, Diablo (DevilutionX).
- **App**: Lettore musicale, Video, Scarica file, Spazio della SD, Diagnostica.
- **Controller Bluetooth**: si possono abbinare, nessuno ne ha ancora provato uno.

**Problemi noti**
- Brick Pro: l'altoparlante fruscia quando non suona niente ([#4](https://github.com/diapolon/mosaik_cfw/issues/4)).
- Smart Pro: la levetta sinistra può fermarsi al centro dopo un movimento veloce ([#9](https://github.com/diapolon/mosaik_cfw/issues/9)).
- Bluetooth: il suono arriva con 150-250 ms di ritardo; giochi Linux, emulatori a parte (Nintendo 64, Dreamcast, PSP), Lettore musicale e Video passano alle cuffie solo quando si riaprono.
- Bluetooth: il kernel del firmware TrimUI ha i difetti Bluetooth del 2020, che Mosaik non può correggere; spegni il Bluetooth quando non ti serve ([dettagli](https://github.com/diapolon/mosaik_cfw/wiki/Cuffie-Bluetooth#sicurezza)).

Hai provato qualcosa di sperimentale? Scrivi se funziona in una [issue](https://github.com/diapolon/mosaik_cfw/issues): con la tua prova può perdere la fiaschetta.

## L'interfaccia

<table>
  <tr>
    <td><img src="docs/images/it/platforms.png" alt="Piattaforme"></td>
    <td><img src="docs/images/it/library.png" alt="Giochi"></td>
  </tr>
  <tr>
    <td align="center">Le piattaforme, per scegliere la categoria prima dei giochi</td>
    <td align="center">Giochi, a griglia di immagini in Tessera</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/switch-game.png" alt="Cambia gioco"></td>
    <td><img src="docs/images/it/quick-panel.png" alt="Pannello rapido"></td>
  </tr>
  <tr>
    <td align="center">Cambia gioco (Select): riprendi una delle ultime partite</td>
    <td align="center">Pannello rapido (Menu): stato della console e comandi veloci</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/details.png" alt="Scheda del gioco"></td>
    <td><img src="docs/images/it/filters.png" alt="Filtri"></td>
  </tr>
  <tr>
    <td align="center">La scheda del gioco, con storia e informazioni</td>
    <td align="center">Ricerca, generi, collezioni e ordinamento</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/guide.png" alt="Guida dell'emulatore"></td>
    <td><img src="docs/images/it/ingame-menu.png" alt="Menu di gioco"></td>
  </tr>
  <tr>
    <td align="center">La guida di ogni emulatore (Select in Piattaforme): la tua console con cosa fa ogni tasto</td>
    <td align="center">Il menu di gioco (Menu): salvataggi di stato, impostazioni per questo gioco o per tutta la console</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/settings.png" alt="Impostazioni"></td>
    <td><img src="docs/images/it/appearance.png" alt="Impostazioni di aspetto"></td>
  </tr>
  <tr>
    <td align="center">Le impostazioni, divise in sezioni</td>
    <td align="center">Tema, variante di colore e lingua</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/theme-cabinato.png" alt="Tema Cabinato"></td>
    <td><img src="docs/images/it/theme-onda.png" alt="Tema Onda"></td>
  </tr>
  <tr>
    <td align="center">Tema Cabinato (dallo Store): il menu numerato da sala giochi</td>
    <td align="center">Tema Onda (dallo Store): i giochi come elenco verticale</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/transfer.png" alt="Finestra Trasferisci file"></td>
    <td><img src="docs/images/it/transfer-page.png" alt="Pagina del trasferimento"></td>
  </tr>
  <tr>
    <td align="center">Trasferisci file via Wi-Fi: inquadra il QR o apri mosaik.local</td>
    <td align="center">La pagina del trasferimento, in qualsiasi browser</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/store-folders.png" alt="Cartelle dello Store"></td>
    <td><img src="docs/images/it/store.png" alt="Console nello Store"></td>
  </tr>
  <tr>
    <td align="center">Lo Store: emulatori, giochi Linux, app, temi, cornici e filtri</td>
    <td align="center">Emulatori installati a colori, gli altri in grigio; icone per aggiornamenti e pacchetti sperimentali</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/store-page.png" alt="La pagina di un pacchetto nello Store"></td>
    <td><img src="docs/images/it/store-delete.png" alt="Anteprima di Elimina nello Store"></td>
  </tr>
  <tr>
    <td align="center">Ogni pacchetto ha la sua pagina, con le immagini (L/R)</td>
    <td align="center">Elimina: scegli tu cosa togliere, l'emulatore, i suoi salvataggi o i suoi giochi</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/theme-copertina.png" alt="Tema Copertina"></td>
    <td><img src="docs/images/it/theme-stellato.png" alt="Tema Stellato"></td>
  </tr>
  <tr>
    <td align="center">Tema Copertina: il disegno grande della piattaforma e la striscia sotto</td>
    <td align="center">Tema Stellato: schede in alto, carte con produttore e anno</td>
  </tr>
</table>

<sub>I giochi e le copertine in queste immagini sono inventati, solo per mostrare l'interfaccia.</sub>

## Installazione

Servono una microSD di buona qualità (da 4 GB a 256 GB) e [balenaEtcher](https://etcher.balena.io/).

1. **Scarica l'immagine** del tuo modello dall'[ultima versione](https://github.com/diapolon/mosaik_cfw/releases/latest), sotto **Assets**: `mosaik-<versione>-smartpro.img.gz` per la Smart Pro, `mosaik-<versione>-brickpro.img.gz` per la Brick Pro. Non serve decomprimerla. **Controlla il nome del file: con l'immagine dell'altro modello la console non parte.** Non si rovina nulla: basta scrivere quella giusta. **Ignora tutti gli altri file** (`file-0`, `file-1`…, `update.txt`, `update.sig`, `build-info.txt`, archivi dei sorgenti, `store-img-….png` e `store-icon-….png`): li usano da soli l'aggiornamento Wi-Fi e lo Store della console. I file `store-….zip` sono gli emulatori, con `store.cat`, `store.json` e `store.sig`, per una console senza Wi-Fi (passo 5).
2. **Scrivila sulla microSD** con balenaEtcher. Tutto ciò che c'è sulla scheda viene cancellato. Se poi il computer chiede di formattare una parte della scheda, rispondi di no.
3. **Metti la scheda nella console e accendi.** Alla prima accensione la partizione si allarga a tutta la scheda, quindi l'avvio dura un po' di più.
4. **Apri le impostazioni con Start** per scegliere la lingua (Aspetto) e impostare data, ora e Wi-Fi (Sistema).
5. **Installa gli emulatori**: apri lo **Store** dalla home (su una scheda nuova te lo propone la console) e scegli quelli che ti servono. Senza Wi-Fi, copia nella cartella principale della scheda i file `store-….zip` degli emulatori che vuoi, presi dalla release, insieme a `store.json` e `store.sig` della stessa release (dimostrano che i pacchetti sono originali): al prossimo avvio la console propone di installarli.

Per tornare al sistema originale, spegni la console e togli la scheda.

## Caricare i giochi

> **ROM e BIOS non sono inclusi.** Usa solo copie di giochi di cui hai diritto.

Spegni la console e collega la scheda al computer. Tutto si trova nella cartella `Arcade` dell'unità `MOSAIK` (`ARCADE` sulle schede installate prima della 0.9.6):

| Cartella | Cosa contiene |
|---|---|
| `Roms/FBNEO` | ROM per FinalBurn Neo (raccolta FBNeo attuale), anche in sottocartelle |
| `Roms/MAME2003P` | ROM per MAME 2003-Plus (raccolta 0.78-plus) |
| `Roms/MAME2010` | ROM per MAME 2010 (raccolta 0.139), per i giochi che gli altri due non fanno partire |
| `Roms/NES`, `Roms/SNES`, `Roms/MD` | Giochi NES, Super Nintendo e Mega Drive |
| `Roms/SMS`, `Roms/GG`, `Roms/PCE` | Giochi Master System, Game Gear e PC Engine (HuCard) |
| `Roms/GB`, `Roms/GBC`, `Roms/GBA`, `Roms/NGP` | Giochi Game Boy, Game Boy Color, Game Boy Advance e Neo Geo Pocket |
| `Roms/PS` | Giochi PlayStation: `.chd` (consigliato), `.cue` con i suoi `.bin`, oppure `.pbp` |
| `Roms/DC` | Giochi Dreamcast: `.chd` (consigliato), `.gdi` con le sue tracce, `.cdi` o `.m3u` |
| `Bios` | `neogeo.zip` e gli altri BIOS; per il Dreamcast `dc/dc_boot.bin` |
| `Linux` | Giochi Linux, uno per cartella |
| `Apps` | Programmi che non sono giochi, uno per cartella con `launch.sh` |
| `Media` | Immagini dei giochi |
| `Music` | Musica per il Lettore musicale (MP3, OGG, FLAC, WAV); la musica del menu in `Music/Menu` |
| `Video` | Video per l'app Video (MP4, MKV, WebM, AVI, MOV) |
| `Saves` | Salvataggi |

Tieni le ROM arcade in `.zip` e non cambiare il nome. I giochi a cartuccia di NES, Super Nintendo, Mega Drive, Master System, Game Gear, PC Engine, Game Boy, Game Boy Color, Game Boy Advance e Neo Geo Pocket possono essere zippati o no, un gioco per zip; per le altre console copia i file così come sono; il GBA non ha bisogno del BIOS. Per un gioco PlayStation su più dischi, elenca i dischi in un file `.m3u` nella stessa cartella: in elenco compare solo il `.m3u`, e **Disco** nel menu di gioco cambia disco. Un BIOS PlayStation (`scph5501.bin`…) in `Bios` è facoltativo. **Impostazioni → ROM e immagini → Verifica ROM** controlla che ogni zip contenga i file giusti (**Completa** legge anche tutti i dati e trova i file rovinati), e la scheda del gioco (X) mostra cosa manca, compreso un BIOS. Il menu del Dreamcast è quello dell'emulatore (Deadcast o Flycast): si apre con Menu, ed è da lì che si esce sempre dal gioco, così i salvataggi della memory card vengono scritti; con Flycast, tenendo premuto Menu si apre il menu di Mosaik. Su Dreamcast e Nintendo 64 A, B, X e Y sono i tasti con quelle lettere.

Per copertine e informazioni, collega il Wi-Fi, installa dallo Store l'app **Copertine e info**, poi usa **Impostazioni → ROM e immagini → Scarica copertine e info**.

Puoi anche copiare i file senza togliere la scheda: apri **Impostazioni → Scheda e dati → Trasferisci file**, poi inquadra il codice QR con il telefono, oppure apri **http://mosaik.local** nel browser di un computer collegato alla stessa rete Wi-Fi e scrivi il PIN (con **Ricorda questo browser** le volte dopo non serve). Dalla pagina carichi ROM, BIOS, immagini, musica e giochi Linux (file o cartelle intere, anche trascinandoli sulla pagina), scarichi salvataggi e registri (una cartella diventa uno zip), crei, rinomini ed elimini. I file del sistema si possono solo scaricare. Il trasferimento resta acceso solo finché la finestra è aperta sulla console, e il PIN cambia ogni volta. Il riquadro **Backup** della pagina scarica un solo zip con salvataggi, impostazioni, preferiti, collezioni, tempo di gioco e BIOS (a scelta reti Wi-Fi, immagini e giochi) e lo ripristina, tutto o solo le parti che spunti; uno zip di backup copiato nella cartella principale della scheda viene proposto al prossimo avvio.

### Giochi Linux

Crea una cartella in `Arcade/Linux` con il gioco, compilato per Linux ARM a 64 bit, e un file `launch.sh` che lo avvia:

```
Arcade/Linux/MioGioco/
    launch.sh      obbligatorio: avvia il gioco
    game.json      facoltativo: titolo e opzioni
    cover.png      facoltativo: copertina
```

```json
{
  "title": "Il mio gioco",
  "year": "2024",
  "author": "Autore",
  "env": { "NOME_VARIABILE": "valore" },
  "keys": { "A": "SPACE", "START": "ENTER" },
  "cpu": 3
}
```

`env` imposta variabili d'ambiente prima di avviare il gioco. `keys` serve ai giochi che non leggono i controller: ogni tasto della console preme il tasto della tastiera indicato. `cpu` (dalla 0.9.104) sceglie la velocità del processore mentre il gioco gira: 1 = 816, 2 = 1200, 3 = 1608, 4 = 2000 MHz; senza, il profilo scelto nelle impostazioni.

## Comandi

| Tasto | Cosa fa |
|---|---|
| Accensione, breve | Una finestra con Annulla, Sospendi, Riavvia e Spegni, anche durante un gioco; premendo di nuovo si sospende. Un'altra pressione risveglia |
| Accensione, tenuto | Chiede se spegnere. Durante un gioco salva la partita e spegne |
| Volume + / − | Volume, con una barra a schermo anche in gioco |
| A / B | Conferma / indietro |
| X | Scheda del gioco |
| Y | Aggiunge ai preferiti |
| Start | Impostazioni |
| Select | Filtri e ordinamento (in Giochi); Cambia gioco (nella schermata principale); la guida dell'emulatore (in Piattaforme) |
| Menu (nel menu di Mosaik) | Pannello rapido: batteria, Wi-Fi, luminosità, volume, LED, musica, cattura, spegni |
| L / R | Cambiano scheda in Giochi |
| Menu (giochi arcade e console) | Menu del gioco: Cambia gioco, salva e carica stato, proporzioni, filtro, comandi, esci |
| Menu tenuto 1 s (giochi Linux, Nintendo 64, Flycast) | Menu sopra al gioco fermo; per il Nintendo 64 anche salva e carica stato, e per i due emulatori una pagina **Comandi** con i tasti originali e quelli della TrimUI |

**Tasti rapidi nei giochi arcade e console (tieni premuto Menu):**

| Combinazione | Cosa fa |
|---|---|
| Menu + R / L | Salva / carica stato |
| Menu + R2 | Avanti veloce |
| Menu + X | Screenshot |
| Menu + Su / Giù | Volume |
| Menu + Sinistra / Destra | Luminosità |

Sulla Brick Pro, HOME torna al launcher, le due barrette sotto lo schermo salvano e caricano la partita e l'interruttore FN attiva le prestazioni massime. In **Impostazioni → Comandi** trovi la prova di tasti e levette e puoi scegliere cosa fanno HOME e le due barrette.

## Wi-Fi e aggiornamenti

Apri **Impostazioni → Sistema**: **Wi-Fi** accende e spegne, **Reti Wi-Fi** mostra le reti a cui collegarsi. Sulla tastiera a schermo, X passa tra maiuscole, minuscole e simboli. Nello stesso posto **Bluetooth** accende e spegne, e **Dispositivi Bluetooth** cerca cuffie e casse da abbinare.

Con il Wi-Fi acceso Mosaik si collega solo a: GitHub (aggiornamenti, lo Store e il suo catalogo; `raw.githubusercontent.com` per i dati dei giochi di libretro-database), `thumbnails.libretro.com` e `adb.arcadeitalia.net` (app Copertine e info), `pool.ntp.org` e `time.cloudflare.com` (ora automatica), e gli indirizzi che scrivi nell'app Scarica file (per esempio archive.org). Di te e dei tuoi giochi non manda niente, a parte i nomi dei giochi che Copertine e info cerca (il CRC di ogni ROM si confronta sulla console, con i dati scaricati).

Per aggiornare, apri **Impostazioni → Aggiornamenti → Aggiorna sistema** e scegli **Scarica**. Quando il download è stato controllato, scegli **Installa**. ROM, salvataggi e impostazioni non vengono toccati. Ogni aggiornamento è firmato e controllato prima di essere installato. I file sostituiti restano finché il sistema nuovo non arriva al menu: se la corrente salta durante l'installazione, o il sistema nuovo non parte per tre volte, la console torna alla versione precedente con tutti i suoi file. Non copre ogni guasto: nel caso peggiore, [riscrivi l'immagine sulla scheda](https://github.com/diapolon/mosaik_cfw/wiki/Installazione#riscrivere-la-scheda-senza-perdere-niente).

**Impostazioni → Aggiornamenti → Canale aggiornamenti** passa tra Stabile e Sviluppo (versioni di prova, che possono avere problemi). Tornando a Stabile si resta così fino alla prossima versione stabile.

In **Impostazioni → Scheda e dati** ci sono anche **Reimposta impostazioni** (impostazioni, liste e reti Wi-Fi come nuove; giochi e salvataggi restano) e **Cancella tutto** (anche giochi, BIOS e salvataggi, come una scheda appena scritta). Tutte e due chiedono conferma più volte.

## Problemi frequenti

- **La console non si accende con la scheda:** controlla di aver scritto l'immagine del tuo modello (`smartpro` o `brickpro` nel nome del file). Senza la scheda parte il sistema originale, come sempre.
- **Schermo nero o rosso all'avvio:** tieni premuto il tasto di accensione per 10-20 secondi, togli la scheda e guarda in `Arcade/Logs`.
- **Una console non ha giochi o manca l'emulatore:** installalo dallo **Store** nella home. Un emulatore archiviato torna con **Scarica di nuovo**, insieme ai suoi giochi e salvataggi.
- **Un gioco non parte:** la console mostra cosa manca e dove copiarlo. Puoi aprire il controllo anche da solo: nella scheda del gioco premi X e scegli **Diagnosi del gioco**.
- **Un gioco si comporta male dopo aver cambiato le opzioni:** nella sua scheda premi X e scegli **Reimposta il gioco**. La ROM resta, e gli stati salvati restano se non scegli diversamente.
- **I giochi Neo Geo non partono:** serve `neogeo.zip` in `Arcade/Bios`.
- **Un gioco è lento o l'audio gracchia:** prova il profilo Prestazioni in Impostazioni → Giochi. Alcuni arcade 3D sono troppo pesanti o non supportati.
- **File rovinati dopo la copia:** schede e lettori economici possono rovinare i file senza dare errori. Prova un altro lettore o un'altra scheda.
- **Serve più dettaglio:** il registro è spento di base. Accendilo in **Impostazioni → Aggiornamenti → Registro diagnostico**, riavvia la console, riproduci il problema e guarda in `Arcade/Logs`.

## Sostieni Mosaik

Mosaik è gratis e resterà gratis. Se ti piace e vuoi aiutarne lo sviluppo, puoi fare una donazione con [PayPal](https://www.paypal.me/diapolonRS): grazie! Sulla console, il pulsante **Sostieni Mosaik** in alto nelle impostazioni (tasto **Y**) mostra un codice QR per aprire la stessa pagina dal telefono.

## Compilare

La compilazione usa Docker e richiede componenti estratti dal firmware ufficiale TrimUI, che non sono in questo repository. [`CONTRIBUTING.md`](CONTRIBUTING.md) (in inglese) elenca ogni dipendenza con indirizzo e commit, i comandi, le prove, come un fork usa la propria chiave di firma e come si aggiunge un emulatore; [`m2/README.md`](m2/README.md) spiega l'immagine della SD.

## Licenza

Il codice scritto per Mosaik è sotto [GNU LGPL v3](COPYING.LESSER). Non si applica ai componenti di terzi, che mantengono le proprie licenze: vedi [`licenses/THIRD-PARTY.md`](licenses/THIRD-PARTY.md), con anche i link ai sorgenti degli emulatori.

FinalBurn Neo, MAME 2003-Plus, MAME 2010, Genesis Plus GX e Snes9x sono solo per uso non commerciale, quindi **Mosaik non può essere venduto**, né da solo né preinstallato. Le immagini del sistema contengono anche componenti presi dal firmware ufficiale TrimUI, che appartengono ai rispettivi proprietari. I sorgenti dei programmi GPL compilati per le immagini (BusyBox, dosfstools) e dei programmi del Bluetooth presi dal firmware TrimUI (BlueZ, D-Bus, GLib, readline e gli altri) sono allegati a ogni [release](https://github.com/diapolon/mosaik_cfw/releases), insieme a quelli degli emulatori GPL e MPL (FCEUmm, Gambatte, mGBA, Beetle PCE Fast, Beetle NeoPop, PCSX ReARMed, Deadcast, Flycast, PPSSPP e gli altri) nelle release che li contengono.

Progetto indipendente, non affiliato a TrimUI.
