# To do

## Testing

- [x] Full test on the Brick Pro
- [x] Automatic return to the previous system on a Smart Pro (broken system, three failed boots)
- [ ] Power loss during an update, on a real console
- [ ] Restart with the charger connected: U-Boot enters its charging-only mode and the system never starts (Smart Pro, confirmed; [#2](https://github.com/diapolon/mosaik_cfw/issues/2)). Mosaik now powers off instead: check that power off with the charger connected, then the power button, starts normally; check the Brick Pro
- [ ] Wi-Fi password containing quotes, on a real console
- [ ] First run on a brand new card
- [x] Brick Pro freeze when Wi-Fi starts (I2C bus 3 race, trimui/firmware_brickpro#4, [#1](https://github.com/diapolon/mosaik_cfw/issues/1)): interrupts on separate CPUs and `trimui_inputd` paused while Wi-Fi starts, 0 freezes in 700 starts (200 + 500)
- [x] Brick Pro: long session with Wi-Fi on and traffic: 60 minutes with ping and 20 minutes with 217 MB of downloads, no freeze ([#1](https://github.com/diapolon/mosaik_cfw/issues/1))

## Reliability

- [x] Undo the files already replaced when an update stops with an error
- [ ] Survive a power loss while files are being replaced (journal or A/B slots); the rollback after three failed boots restores only the system ([#3](https://github.com/diapolon/mosaik_cfw/issues/3))
- [x] Replace `volatile` status fields with atomics or a mutex in the worker threads
- [x] Do not grow the card or mount it for writing after a failed disk check, and recover the partition table from its backup copy (new images since 0.9.15)
- [ ] Full ROM check that decompresses the data, as an option next to the quick check
- [ ] Test matrix per console and feature, with version and date

## Features

- [ ] Backup and restore: Tools → Backup makes one zip on the card (saves, settings, favorites, collections, play time, Wi-Fi networks; optionally images and ROMs). Copy it to a computer, write the card again, put the zip back: at the first start the console finds it and offers to import it. Pairs with the reinstall notice (`m2/area-avvio.conf`)
- [ ] Bluetooth controllers
- [x] Settings page for the Brick Pro's extra buttons (side HOME, F1, F2): list them on the console, choose what each one does, show the page only on models that have them
- [x] Analog stick calibration from the settings
- [x] LED indication while charging
- [ ] Game list cache, for very large libraries
- [x] NES (FCEUmm) and Mega Drive (Genesis Plus GX), with zipped ROMs and battery saves
- [x] Game Boy / Game Boy Color (Gambatte, with the cartridge clock), GBA (mGBA), SNES (Snes9x): to be checked on the console
- [ ] More consoles: PlayStation, Master System, Game Gear, PC Engine, Neo Geo Pocket, Dreamcast (shown greyed as "coming soon" on the Platforms screen: `"planned": true` in `Emulators/<id>.json`)
- [ ] MAME 2010 core, for games missing from FinalBurn Neo and MAME 2003-Plus
- [ ] Flycast core for Naomi and Atomiswave (check performance first)
- [ ] Apps section in the launcher
- [ ] Support for the original TrimUI Brick (TG3040)

## Project

- [ ] Website on mosaikcfw.org
- [ ] English version of the build documentation (`m2/README.md`)
