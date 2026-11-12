<p align="center">
  <img src="docs/images/logo.png" width="444" alt="Mosaik cfw">
</p>

<h3 align="center">All your games, in your pocket.</h3>

<p align="center">
  A gaming system for <b>TrimUI Smart Pro</b> and <b>Brick Pro</b>: arcade, consoles and Linux games.<br>
  It runs from the microSD and leaves the console's internal storage alone.
</p>

<p align="center">
  <a href="https://github.com/diapolon/mosaik_cfw/releases">Download</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#loading-games">Loading games</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#troubleshooting">Troubleshooting</a> ·
  <a href="https://github.com/diapolon/mosaik_cfw/wiki">Wiki</a> ·
  <a href="#support-mosaik">Support</a> ·
  <a href="README.it.md">Italiano</a>
</p>

<p align="center">
  <img src="docs/images/home.png" width="640" alt="The Mosaik home screen">
</p>

> **Status:** version 0.9. **Smart Pro and Brick Pro: tested.**
>
> **Coming:** R36S. A first version is being prepared and is not ready to install yet.

## What's inside

- **Emulators from the Store**: the system stays small and you install only what you play, from the **Store** on the home screen. Every package is signed; **Archive** removes an emulator and keeps its games and saves, **Delete** removes everything after showing what goes.
- **FinalBurn Neo, MAME 2003-Plus and MAME 2010**: Neo Geo, CPS1, CPS2 and most of the arcade classics.
- **NES, Super Nintendo, Mega Drive, Master System, Game Gear, PC Engine, Game Boy, Game Boy Color, Game Boy Advance and Neo Geo Pocket**, with battery saves written every 10 seconds (also the clock of the Pokémon games), separate saves for each console and zipped ROMs.
- **PlayStation** (PCSX ReARMed) with the DualShock sticks: `.chd`, `.cue` and `.pbp` discs, games on several discs with a `.m3u` file and the disc change in the game menu.
- **Dreamcast** with [Deadcast](https://github.com/tyrannotorus/emulator-dreamcast-deadcast), first on the Brick Pro.
- **Covers and game information for consoles too**: box art, title, year, publisher, genre and players, found by the ROM itself.
- **Linux games**, with an in-game menu and sleep.
- **Boots in about 5 seconds**, sleep with the power button.
- **Built-in Wi-Fi** for covers, game history, time and updates.
- **Transfer files over Wi-Fi** from a computer or phone: scan the QR code or open `mosaik.local`, then upload ROMs and download saves without taking the card out.
- **Backup and restore**: one zip with saves, settings, BIOS files and, if you want, the games; restore it all or only the parts you choose, also onto a new card or the other model.
- **Apps**: programs that aren't games get their own section on the home screen.
- **Three themes with color variants**, languages, LEDs and video filters you can configure.
- **Signed updates** over Wi-Fi, all or nothing: if one is interrupted or the new version fails to start, the console goes back to all the previous files.
- **CPU profile for each game**, and a notice on the home screen when an update is available.
- **Game diagnosis**: when a game can't start, it tells you what's missing and where to copy it.
- **Three save slots per game** with preview and date, plus an automatic save with a previous copy.

## The interface

<table>
  <tr>
    <td><img src="docs/images/platforms.png" alt="Platforms"></td>
    <td><img src="docs/images/library.png" alt="Library"></td>
  </tr>
  <tr>
    <td align="center">Platforms, to pick a category before the games</td>
    <td align="center">The library, as a grid of pictures in Tessera</td>
  </tr>
  <tr>
    <td><img src="docs/images/details.png" alt="Game details"></td>
    <td><img src="docs/images/filters.png" alt="Filters"></td>
  </tr>
  <tr>
    <td align="center">Game details, with history and information</td>
    <td align="center">Search, genres, collections and sorting</td>
  </tr>
  <tr>
    <td><img src="docs/images/settings.png" alt="Settings"></td>
    <td><img src="docs/images/appearance.png" alt="Appearance settings"></td>
  </tr>
  <tr>
    <td align="center">Settings, grouped in sections</td>
    <td align="center">Theme, color variant and language</td>
  </tr>
  <tr>
    <td><img src="docs/images/theme-cabinato.png" alt="Cabinato theme"></td>
    <td><img src="docs/images/theme-onda.png" alt="Onda theme"></td>
  </tr>
  <tr>
    <td align="center">Cabinato theme: the numbered arcade menu</td>
    <td align="center">Onda theme: the library as a vertical list</td>
  </tr>
  <tr>
    <td><img src="docs/images/transfer.png" alt="Transfer files window"></td>
    <td><img src="docs/images/transfer-page.png" alt="Transfer page"></td>
  </tr>
  <tr>
    <td align="center">Transfer files over Wi-Fi: scan the QR code or open mosaik.local</td>
    <td align="center">The transfer page, in any browser</td>
  </tr>
  <tr>
    <td><img src="docs/images/store-folders.png" alt="Store folders"></td>
    <td><img src="docs/images/store.png" alt="Consoles in the Store"></td>
  </tr>
  <tr>
    <td align="center">The Store: arcade, consoles, computers, apps, themes and bezels</td>
    <td align="center">Installed emulators in colour, the others in grey</td>
  </tr>
  <tr>
    <td><img src="docs/images/store-themes.png" alt="Themes in the Store"></td>
    <td><img src="docs/images/store-delete.png" alt="Delete preview in the Store"></td>
  </tr>
  <tr>
    <td align="center">Themes to download from the Store</td>
    <td align="center">Before deleting, what is removed and what is kept</td>
  </tr>
  <tr>
    <td><img src="docs/images/theme-copertina.png" alt="Cover theme"></td>
    <td><img src="docs/images/theme-stellato.png" alt="Starry theme"></td>
  </tr>
  <tr>
    <td align="center">Cover theme: the big drawing of the platform and the strip below</td>
    <td align="center">Starry theme: tabs, and cards with maker and year</td>
  </tr>
</table>

<sub>The games and covers in these pictures are made up, just to show the interface.</sub>

## Installation

You need a good-quality microSD card (4 GB to 256 GB) and [balenaEtcher](https://etcher.balena.io/).

1. **Download the image** for your model from the [latest release](https://github.com/diapolon/mosaik_cfw/releases/latest), under **Assets**: `mosaik-<version>-smartpro.img.gz` for the Smart Pro, `mosaik-<version>-brickpro.img.gz` for the Brick Pro. There's no need to decompress it. **Check the file name: with the image of the other model the console won't start.** Nothing gets damaged: just write the right one. **Ignore all the other files** (`file-0`, `file-1`…, `update.txt`, `update.sig`, source archives): the console's Wi-Fi updater uses them by itself. The `store-….zip` files are the emulators, with `store.json` and `store.sig`, for a console without Wi-Fi (step 5).
2. **Write it to the microSD** with balenaEtcher. Everything on the card is erased. If your computer then asks to format part of the card, say no.
3. **Put the card in the console and turn it on.** On first boot the partition grows to fill the card, so startup takes a little longer.
4. **Open the settings with Start** to set the language (Appearance), the date and time and the Wi-Fi (System).
5. **Install the emulators**: open the **Store** from the home screen (the console offers it on a new card) and choose the ones you need. Without Wi-Fi, copy the `store-….zip` files of the emulators you want from the release to the main folder of the card, together with `store.json` and `store.sig` from the same release (they prove the packages are genuine): at the next start the console offers to install them.

To go back to the original system, turn the console off and remove the card.

## Loading games

> **ROMs and BIOS files are not included.** Only use copies of games you have the right to use.

Turn the console off and connect the card to your computer. Everything is in the `Arcade` folder of the `MOSAIK` drive (`ARCADE` on cards installed before 0.9.6):

| Folder | What goes in it |
|---|---|
| `Roms/FBNEO` | ROMs for FinalBurn Neo (current FBNeo set), subfolders allowed |
| `Roms/MAME2003P` | ROMs for MAME 2003-Plus (0.78-plus set) |
| `Roms/MAME2010` | ROMs for MAME 2010 (0.139 set), for the games the other two don't run |
| `Roms/NES`, `Roms/SNES`, `Roms/MD` | NES, Super Nintendo and Mega Drive games |
| `Roms/SMS`, `Roms/GG`, `Roms/PCE` | Master System, Game Gear and PC Engine (HuCard) games |
| `Roms/GB`, `Roms/GBC`, `Roms/GBA`, `Roms/NGP` | Game Boy, Game Boy Color, Game Boy Advance and Neo Geo Pocket games |
| `Roms/PS` | PlayStation games: `.chd` (recommended), `.cue` with its `.bin` files, or `.pbp` |
| `Roms/DC` | Dreamcast games: `.chd` (recommended), `.gdi` with its tracks, `.cdi` or `.m3u` |
| `Bios` | `neogeo.zip` and other BIOS files; for the Dreamcast, `dc/dc_boot.bin` |
| `Linux` | Linux games, one per folder |
| `Apps` | Programs that aren't games, one per folder with `launch.sh` |
| `Media` | Game images |
| `Music` | Background music (MP3, OGG, FLAC, WAV) |
| `Saves` | Saves |

Keep arcade ROMs as `.zip` and don't rename them. Console games can be zipped or not, one game per zip; the GBA doesn't need a BIOS. For a PlayStation game on several discs, list the discs in a `.m3u` file in the same folder: only the `.m3u` shows in the list, and **Disc** in the game menu changes disc. A PlayStation BIOS (`scph5501.bin`…) in `Bios` is optional. **Settings → ROMs and images → Verify ROMs** checks that every zip contains the right files (**Full** also reads all the data and finds damaged files), and the game's details page (X) shows what's missing, including a BIOS. The Dreamcast menu is Deadcast's own: open it with Menu and always quit the game from there, so the memory-card saves are written.

For covers and information, connect to Wi-Fi and use **Settings → ROMs and images → Download images**.

You can also copy files without taking the card out: open **Settings → Card and data → Transfer files**, then scan the QR code with your phone, or open **http://mosaik.local** in a browser on a computer connected to the same Wi-Fi network and type the PIN (tick **Remember this browser** to skip it next time). From the page you can upload ROMs, BIOS files, images, music and Linux games (files or whole folders, also by dragging them onto the page), download saves and logs (a folder becomes a zip), create, rename and delete. System files can only be downloaded. The transfer stays on only while the window is open on the console, and the PIN changes every time. The **Backup** box on the page downloads one zip with your saves, settings, favorites, collections, play time and BIOS files (optionally Wi-Fi networks, images and games) and restores it, all or only the parts you tick; a backup zip copied to the main folder of the card is offered at the next start.

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
| Power, short press | Asks whether to sleep, even during a game: confirm with A or by pressing power again. Press it once more to wake |
| Power, held | Asks whether to power off. During a game it saves and powers off |
| Volume + / − | Volume, with an on-screen bar even in games |
| A / B | Confirm / back |
| X | Game details |
| Y | Add to favorites |
| Start | Settings |
| Select | Filters and sorting (in the library) |
| L / R | Switch tabs in the library |
| Menu (arcade and console games) | Game menu: save and load state, aspect ratio, filter, controls, exit |
| Menu held 1 s (Linux games) | Menu over the paused game |

**Shortcuts in arcade and console games (hold Menu):**

| Combination | What it does |
|---|---|
| Menu + R / L | Save / load state |
| Menu + R2 | Fast forward |
| Menu + X | Screenshot |
| Menu + Up / Down | Volume |
| Menu + Left / Right | Brightness |

On the Brick Pro, HOME goes back to the launcher, the two bars below the screen save and load the game, and the FN switch turns on maximum performance. **Settings → Controls** has a button and stick test and lets you choose what HOME and the two bars do.

## Wi-Fi and updates

Open **Settings → System**: **Wi-Fi** turns it on and off, **Wi-Fi networks** shows the networks to connect to. On the on-screen keyboard, X switches between uppercase, lowercase and symbols.

To update, open **Settings → Updates → Update system** and choose **Download**. When the download has been checked, choose **Install**. ROMs, saves and settings are left untouched. Every update is signed and checked before it is installed. The replaced files are kept until the new system reaches the menu: if the power goes off while installing, or the new system fails to start three times, the console goes back to the previous version with all its files. This does not cover every failure: in the worst case, [write the image to the card again](https://github.com/diapolon/mosaik_cfw/wiki/Installation#writing-the-card-again-without-losing-anything).

**Settings → Updates → Update channel** switches between Stable and Development (test builds that may have problems). Going back to Stable applies from the next stable version.

**Settings → Card and data** also has **Reset settings** (settings, lists and Wi-Fi networks back to new; games and saves stay) and **Erase everything** (also games, BIOS files and saves, like a freshly written card). Both ask more than once.

## Troubleshooting

- **The console doesn't turn on with the card:** check that you wrote the image for your model (`smartpro` or `brickpro` in the file name). Without the card the original system starts as usual.
- **Black or red screen at startup:** hold the power button for 10-20 seconds, remove the card and look in `Arcade/Logs`.
- **A system has no games or the emulator is missing:** install it from the **Store** on the home screen. An archived emulator comes back with **Download again**, with its games and saves.
- **A game won't start:** the console shows what's missing and where to copy it. You can also open the check yourself: on the game details page press X and choose **Game diagnosis**.
- **A game misbehaves after changing its options:** on its details page press X and choose **Reset game**. Your ROM stays, and save states are kept unless you choose otherwise.
- **Neo Geo games won't start:** they need `neogeo.zip` in `Arcade/Bios`.
- **A game is slow or the audio crackles:** try the Performance profile in Settings → Games. Some 3D arcade games are too heavy or aren't supported.
- **Damaged files after copying:** cheap cards and readers can damage files without reporting errors. Try another reader or card.
- **Need more detail:** logs are off by default. Turn them on in **Settings → Updates → Diagnostic log**, restart the console, reproduce the problem and check `Arcade/Logs`.

## Support Mosaik

Mosaik is free and will stay free. If you enjoy it and want to help its development, you can make a donation with [PayPal](https://www.paypal.me/diapolonRS): thank you! On the console, the **Support Mosaik** button at the top of the settings (press **Y**) shows a QR code to open the same page from your phone.

## Building

The build uses Docker and needs components extracted from the official TrimUI firmware, which are not in this repository. See [`m2/README.md`](m2/README.md) (in Italian).

## License

The code written for Mosaik is under the [GNU LGPL v3](COPYING.LESSER). It does not cover third-party components, which keep their own licenses: see [`licenses/THIRD-PARTY.md`](licenses/THIRD-PARTY.md), which also links the emulator sources.

FinalBurn Neo, MAME 2003-Plus, MAME 2010, Genesis Plus GX and Snes9x are for non-commercial use only, so **Mosaik must not be sold**, alone or preinstalled. The system images also contain components taken from the official TrimUI firmware, which belong to their respective owners. The source code of the GPL programs built for the images (BusyBox, dosfstools) is attached to each [release](https://github.com/diapolon/mosaik_cfw/releases), together with the source of the GPL and MPL emulators (FCEUmm, Gambatte, mGBA, Beetle PCE Fast, Beetle NeoPop, PCSX ReARMed, Deadcast) in the releases that include them.

Independent project, not affiliated with TrimUI.
