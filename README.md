<p align="center">
  <img src="docs/images/logo.png" width="396" alt="Mosaik">
</p>

<h3 align="center">All your games, in your pocket.</h3>

<p align="center">
  A gaming system for <b>TrimUI Smart Pro</b> and <b>Brick Pro</b>: arcade and Linux games.<br>
  It runs from the microSD and leaves the console's internal storage alone.
</p>

<p align="center">
  <a href="https://github.com/diapolon/mosaik_cfw/releases">Download</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#loading-games">Loading games</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#troubleshooting">Troubleshooting</a> ·
  <a href="README.it.md">Italiano</a>
</p>

<p align="center">
  <img src="docs/images/home.png" width="640" alt="The Mosaik home screen">
</p>

> **Status:** version 0.9, tested on the Smart Pro. Testing on the Brick Pro is still in progress.

## What's inside

- **FinalBurn Neo and MAME 2003-Plus**: Neo Geo, CPS1, CPS2 and most of the arcade classics.
- **Linux games**, with an in-game menu and sleep.
- **Boots in about 5 seconds**, sleep with the power button.
- **Built-in Wi-Fi** for covers, game history, time and updates.
- **Themes, languages, LEDs and video filters** you can configure.
- **Signed updates** over Wi-Fi, with automatic return to the previous version.

## The interface

<table>
  <tr>
    <td><img src="docs/images/library.png" alt="Library"></td>
    <td><img src="docs/images/details.png" alt="Game details"></td>
  </tr>
  <tr>
    <td align="center">Library, with as many thumbnails per row as you like</td>
    <td align="center">Game details, with history and information</td>
  </tr>
  <tr>
    <td><img src="docs/images/filters.png" alt="Filters"></td>
    <td><img src="docs/images/appearance.png" alt="Appearance settings"></td>
  </tr>
  <tr>
    <td align="center">Search, genres, collections and sorting</td>
    <td align="center">Theme, language and thumbnails</td>
  </tr>
  <tr>
    <td><img src="docs/images/theme-cabinato.png" alt="Cabinato theme"></td>
    <td><img src="docs/images/theme-onda.png" alt="Onda theme"></td>
  </tr>
  <tr>
    <td align="center">Cabinato theme</td>
    <td align="center">Onda theme</td>
  </tr>
</table>

<sub>The games and covers in these pictures are made up, just to show the interface.</sub>

## Installation

You need a good-quality microSD card (4 GB to 256 GB) and [balenaEtcher](https://etcher.balena.io/).

1. **Download the image** for your model from [Releases](https://github.com/diapolon/mosaik_cfw/releases). The two are not interchangeable, and there's no need to decompress them.
2. **Write it to the microSD** with balenaEtcher. Everything on the card is erased. If your computer then asks to format part of the card, say no.
3. **Put the card in the console and turn it on.** On first boot the partition grows to fill the card, so startup takes a little longer.
4. **Open the settings with Start** to set the language (Appearance) and the date and time (System).

To go back to the original system, turn the console off and remove the card.

## Loading games

> **ROMs and BIOS files are not included.** Only use copies of games you have the right to use.

Turn the console off and connect the card to your computer. Everything is in the `Arcade` folder of the `ARCADE` drive:

| Folder | What goes in it |
|---|---|
| `Roms/FBNEO` | ROMs for FinalBurn Neo (current FBNeo set), subfolders allowed |
| `Roms/MAME2003P` | ROMs for MAME 2003-Plus (0.78-plus set) |
| `Bios` | `neogeo.zip` and other BIOS files |
| `Linux` | Linux games, one per folder |
| `Media` | Game images |
| `Music` | Background music (MP3, OGG, FLAC, MOD) |
| `Saves` | Saves |

Keep ROMs as `.zip` and don't rename them. **Settings → Tools → Verify ROMs** checks that every zip contains the right files, and the game's details page (X) shows what's missing, including a BIOS.

For covers and information, connect to Wi-Fi and use **Settings → Tools → Download images**.

### Linux games

Create a folder in `Arcade/Linux` with the game, built for 64-bit ARM Linux, and a `launch.sh` file that starts it:

```
Arcade/Linux/MyGame/
    launch.sh      required: starts the game
    game.json      optional: title and options
    cover.png      optional: cover
```

```json
{
  "title": "My game",
  "year": "2024",
  "author": "Author",
  "env": { "VARIABLE_NAME": "value" },
  "keys": { "A": "SPACE", "START": "ENTER" }
}
```

`env` sets environment variables before the game starts. `keys` is for games that don't read controllers: each console button presses the given keyboard key.

## Controls

| Button | What it does |
|---|---|
| Power, short press | Sleep, even during a game. Press again to wake |
| Power, held | Asks whether to power off. During a game it saves and powers off |
| Volume + / − | Volume, with an on-screen bar even in games |
| A / B | Confirm / back |
| X | Game details |
| Y | Add to favorites |
| Start | Settings |
| Select | Filters and sorting (in the library) |
| L / R | Switch tabs in the library |
| Menu (arcade games) | Game menu: save and load state, aspect ratio, filter, controls, exit |
| Menu held 1 s (Linux games) | Menu over the paused game |

**Shortcuts in arcade games (hold Menu):**

| Combination | What it does |
|---|---|
| Menu + R / L | Save / load state |
| Menu + R2 | Fast forward |
| Menu + X | Screenshot |
| Menu + Up / Down | Volume |
| Menu + Left / Right | Brightness |

On the Brick Pro, HOME goes back to the launcher, the two bars below the screen save and load the game, and the FN switch turns on maximum performance.

## Wi-Fi and updates

Open **Settings → System → Wi-Fi**: left and right turn it on and off, A shows the networks. On the on-screen keyboard, X switches between uppercase, lowercase and symbols.

To update, open **Settings → Tools → Update system** and confirm with A. ROMs, saves and settings are left untouched. Every update is signed and checked before it is installed, and if the new system fails to start, the console goes back to the previous one by itself.

## Troubleshooting

- **Black or red screen at startup:** hold the power button for 10-20 seconds, remove the card and look in `Arcade/Logs`.
- **A game won't start:** it's usually missing files or the wrong ROM set. Run Verify ROMs and open the game's details page.
- **Neo Geo games won't start:** they need `neogeo.zip` in `Arcade/Bios`.
- **A game is slow or the audio crackles:** try the Performance profile in Settings → Games. Some 3D arcade games are too heavy or aren't supported.
- **Damaged files after copying:** cheap cards and readers can damage files without reporting errors. Try another reader or card.
- **Need more detail:** raise **Settings → Tools → Diagnostic log**, reproduce the problem and check `Arcade/Logs`.

## Building

The build uses Docker and needs components extracted from the official TrimUI firmware, which are not in this repository. See [`m2/README.md`](m2/README.md) (in Italian).

## License

The code written for Mosaik is under the [GNU LGPL v3](COPYING.LESSER). It does not cover third-party components, which keep their own licenses: see the [`licenses`](licenses) folder.

FinalBurn Neo and MAME 2003-Plus are for non-commercial use only, so **Mosaik must not be sold**, alone or preinstalled. The system images also contain components taken from the official TrimUI firmware, which belong to their respective owners.

Independent project, not affiliated with TrimUI.
