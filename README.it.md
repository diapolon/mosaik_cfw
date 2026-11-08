<p align="center">
  <img src="docs/images/logo.png" width="396" alt="Mosaik">
</p>

<h3 align="center">Tutti i tuoi giochi, in tasca.</h3>

<p align="center">
  Un sistema di gioco per <b>TrimUI Smart Pro</b> e <b>Brick Pro</b>: arcade e giochi Linux.<br>
  Parte dalla microSD e non tocca la memoria interna della console.
</p>

<p align="center">
  <a href="https://github.com/diapolon/mosaik_cfw/releases">Scarica</a> ·
  <a href="#installazione">Installazione</a> ·
  <a href="#caricare-i-giochi">Caricare i giochi</a> ·
  <a href="#comandi">Comandi</a> ·
  <a href="#problemi-frequenti">Problemi frequenti</a> ·
  <a href="https://github.com/diapolon/mosaik_cfw/wiki/Italiano">Wiki</a> ·
  <a href="README.md">English</a>
</p>

<p align="center">
  <img src="docs/images/it/home.png" width="640" alt="La schermata iniziale di Mosaik">
</p>

> **Stato:** versione 0.9. **Smart Pro e Brick Pro: provate.**

## Cosa c'è dentro

- **FinalBurn Neo e MAME 2003-Plus**: Neo Geo, CPS1, CPS2 e gran parte dei classici arcade.
- **Giochi Linux**, con menu sopra al gioco e sospensione.
- **Avvio in circa 5 secondi**, sospensione con il tasto di accensione.
- **Wi-Fi integrato** per copertine, storia dei giochi, ora e aggiornamenti.
- **Temi, lingue, LED e filtri video** configurabili.
- **Aggiornamenti firmati** via Wi-Fi, con un tentativo di ritorno alla versione precedente se uno fallisce.
- **Profilo CPU per ogni gioco** e avviso nella home quando c'è un aggiornamento.
- **Diagnosi dei giochi**: quando un gioco non parte, ti dice cosa manca e dove copiarlo.

## L'interfaccia

<table>
  <tr>
    <td><img src="docs/images/it/library.png" alt="Libreria"></td>
    <td><img src="docs/images/it/details.png" alt="Scheda del gioco"></td>
  </tr>
  <tr>
    <td align="center">La libreria, con quante miniature per riga vuoi</td>
    <td align="center">La scheda del gioco, con storia e informazioni</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/filters.png" alt="Filtri"></td>
    <td><img src="docs/images/it/appearance.png" alt="Impostazioni di aspetto"></td>
  </tr>
  <tr>
    <td align="center">Ricerca, generi, collezioni e ordinamento</td>
    <td align="center">Tema, lingua e miniature</td>
  </tr>
  <tr>
    <td><img src="docs/images/it/theme-cabinato.png" alt="Tema Cabinato"></td>
    <td><img src="docs/images/it/theme-onda.png" alt="Tema Onda"></td>
  </tr>
  <tr>
    <td align="center">Tema Cabinato</td>
    <td align="center">Tema Onda</td>
  </tr>
</table>

<sub>I giochi e le copertine in queste immagini sono inventati, solo per mostrare l'interfaccia.</sub>

## Installazione

Servono una microSD di buona qualità (da 4 GB a 256 GB) e [balenaEtcher](https://etcher.balena.io/).

1. **Scarica l'immagine** del tuo modello da [Releases](https://github.com/diapolon/mosaik_cfw/releases). Non serve decomprimerla. **Controlla il nome del file: con l'immagine dell'altro modello la console non parte.** Non si rovina nulla: basta scrivere quella giusta.
2. **Scrivila sulla microSD** con balenaEtcher. Tutto ciò che c'è sulla scheda viene cancellato. Se poi il computer chiede di formattare una parte della scheda, rispondi di no.
3. **Metti la scheda nella console e accendi.** Alla prima accensione la partizione si allarga a tutta la scheda, quindi l'avvio dura un po' di più.
4. **Apri le impostazioni con Start** per scegliere la lingua (Aspetto) e impostare data e ora (Sistema).

Per tornare al sistema originale, spegni la console e togli la scheda.

## Caricare i giochi

> **ROM e BIOS non sono inclusi.** Usa solo copie di giochi di cui hai diritto.

Spegni la console e collega la scheda al computer. Tutto si trova nella cartella `Arcade` dell'unità `MOSAIK` (`ARCADE` sulle schede installate prima della 0.9.6):

| Cartella | Cosa contiene |
|---|---|
| `Roms/FBNEO` | ROM per FinalBurn Neo (raccolta FBNeo attuale), anche in sottocartelle |
| `Roms/MAME2003P` | ROM per MAME 2003-Plus (raccolta 0.78-plus) |
| `Bios` | `neogeo.zip` e gli altri BIOS |
| `Linux` | Giochi Linux, uno per cartella |
| `Media` | Immagini dei giochi |
| `Music` | Musica di sottofondo (MP3, OGG, FLAC, WAV) |
| `Saves` | Salvataggi |

Tieni le ROM in `.zip` e non cambiare il nome. **Impostazioni → Strumenti → Verifica ROM** controlla che ogni zip contenga i file giusti, e la scheda del gioco (X) mostra cosa manca, compreso un BIOS.

Per copertine e informazioni, collega il Wi-Fi e usa **Impostazioni → Strumenti → Scarica immagini**.

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
  "keys": { "A": "SPACE", "START": "ENTER" }
}
```

`env` imposta variabili d'ambiente prima di avviare il gioco. `keys` serve ai giochi che non leggono i controller: ogni tasto della console preme il tasto della tastiera indicato.

## Comandi

| Tasto | Cosa fa |
|---|---|
| Accensione, breve | Chiede se sospendere, anche durante un gioco: conferma con A o premendo di nuovo. Un'altra pressione risveglia |
| Accensione, tenuto | Chiede se spegnere. Durante un gioco salva la partita e spegne |
| Volume + / − | Volume, con una barra a schermo anche in gioco |
| A / B | Conferma / indietro |
| X | Scheda del gioco |
| Y | Aggiunge ai preferiti |
| Start | Impostazioni |
| Select | Filtri e ordinamento (in libreria) |
| L / R | Cambiano scheda in libreria |
| Menu (giochi arcade) | Menu del gioco: salva e carica stato, proporzioni, filtro, comandi, esci |
| Menu tenuto 1 s (giochi Linux) | Menu sopra al gioco fermo |

**Tasti rapidi nei giochi arcade (tieni premuto Menu):**

| Combinazione | Cosa fa |
|---|---|
| Menu + R / L | Salva / carica stato |
| Menu + R2 | Avanti veloce |
| Menu + X | Screenshot |
| Menu + Su / Giù | Volume |
| Menu + Sinistra / Destra | Luminosità |

Sulla Brick Pro, HOME torna al launcher, le due barrette sotto lo schermo salvano e caricano la partita e l'interruttore FN attiva le prestazioni massime.

## Wi-Fi e aggiornamenti

Apri **Impostazioni → Sistema → Wi-Fi**: sinistra e destra accendono e spengono, A mostra le reti. Sulla tastiera a schermo, X passa tra maiuscole, minuscole e simboli.

Per aggiornare, apri **Impostazioni → Strumenti → Aggiorna sistema** e premi A per scaricare. Quando il download è stato controllato, premi di nuovo A per installare. ROM, salvataggi e impostazioni non vengono toccati. Ogni aggiornamento è firmato e controllato prima di essere installato, e se un sistema nuovo non parte la console prova a tornare da sola a quello precedente. Non copre ogni guasto: nel caso peggiore, riscrivi l'immagine sulla scheda.

**Impostazioni → Strumenti → Canale aggiornamenti** passa tra Stabile e Sviluppo (versioni di prova, che possono avere problemi). Tornando a Stabile si resta così fino alla prossima versione stabile.

## Problemi frequenti

- **La console non si accende con la scheda:** controlla di aver scritto l'immagine del tuo modello (`smartpro` o `brickpro` nel nome del file). Senza la scheda parte il sistema originale, come sempre.
- **Schermo nero o rosso all'avvio:** tieni premuto il tasto di accensione per 10-20 secondi, togli la scheda e guarda in `Arcade/Logs`.
- **Un gioco non parte:** la console mostra cosa manca e dove copiarlo. Puoi aprire il controllo anche da solo: nella scheda del gioco premi X e scegli **Diagnosi del gioco**.
- **I giochi Neo Geo non partono:** serve `neogeo.zip` in `Arcade/Bios`.
- **Un gioco è lento o l'audio gracchia:** prova il profilo Prestazioni in Impostazioni → Giochi. Alcuni arcade 3D sono troppo pesanti o non supportati.
- **File rovinati dopo la copia:** schede e lettori economici possono rovinare i file senza dare errori. Prova un altro lettore o un'altra scheda.
- **Serve più dettaglio:** il registro è spento di base. Accendilo in **Impostazioni → Strumenti → Registro diagnostico**, riavvia la console, riproduci il problema e guarda in `Arcade/Logs`.

## Compilare

La compilazione usa Docker e richiede componenti estratti dal firmware ufficiale TrimUI, che non sono in questo repository. Vedi [`m2/README.md`](m2/README.md).

## Licenza

Il codice scritto per Mosaik è sotto [GNU LGPL v3](COPYING.LESSER). Non si applica ai componenti di terzi, che mantengono le proprie licenze: vedi [`licenses/THIRD-PARTY.md`](licenses/THIRD-PARTY.md), con anche i link ai sorgenti degli emulatori.

FinalBurn Neo e MAME 2003-Plus sono solo per uso non commerciale, quindi **Mosaik non può essere venduto**, né da solo né preinstallato. Le immagini del sistema contengono anche componenti presi dal firmware ufficiale TrimUI, che appartengono ai rispettivi proprietari. I sorgenti dei programmi GPL inclusi nelle immagini (BusyBox, dosfstools) sono allegati a ogni [release](https://github.com/diapolon/mosaik_cfw/releases).

Progetto indipendente, non affiliato a TrimUI.
