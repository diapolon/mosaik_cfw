# To do

## Testing

- [x] Power loss during an update, on a real console (Smart Pro, forced power-off during the install with the test pause: the previous files came back, 0.9.18)
- [ ] "Update downloaded" question at start (power off after downloading, before installing), on a real console
- [ ] Wi-Fi password containing quotes, on a real console
- [ ] First run on a brand new card
- [ ] Test matrix per console and feature, with version and date (the ten console cores included)
- [x] Full test on the Brick Pro
- [x] Automatic return to the previous system on a Smart Pro (broken system, three failed boots)
- [x] Brick Pro freeze when Wi-Fi starts (I2C bus 3 race, trimui/firmware_brickpro#4, [#1](https://github.com/diapolon/mosaik_cfw/issues/1)): interrupts on separate CPUs and `trimui_inputd` paused while Wi-Fi starts, 0 freezes in 700 starts (200 + 500); fixed in 0.9.17
- [x] Brick Pro: long session with Wi-Fi on and traffic: 60 minutes with ping and 20 minutes with 217 MB of downloads, no freeze ([#1](https://github.com/diapolon/mosaik_cfw/issues/1))
- [x] Power off and restart with the charger connected ([#2](https://github.com/diapolon/mosaik_cfw/issues/2)): the AXP2202 switched the console back on and U-Boot stopped on a black screen. Mosaik powers off through the chip (`m2/axpctl.c`), a restart with the charger becomes a power off; the power button then starts normally. Tested on Smart Pro and Brick Pro; fixed in 0.9.17

## Reliability

- [ ] Games with the same file name on two platforms that share a core (MD/SMS/GG, GB/GBC) share saves, states, statistics and pictures: keep each platform apart and move existing files ([#5](https://github.com/diapolon/mosaik_cfw/issues/5))
- [ ] Neo Geo Pocket: flash save now goes through the frontend as `.srm` (`toolchain/patches/beetle-ngp-saveram.patch`, [#6](https://github.com/diapolon/mosaik_cfw/issues/6)); to test on console with a real game that saves to flash: forced power-off, sleep, low battery, 0.9.17 `.flash` migration
- [ ] Survive a power loss while files are being replaced (journal or A/B slots); the rollback after three failed boots restores only the system ([#3](https://github.com/diapolon/mosaik_cfw/issues/3))
- [ ] Full ROM check that decompresses the data, as an option next to the quick check
- [ ] On restart the card was once not closed cleanly: a process still had `Logs/passi.txt` open as its output ("SD non chiusa" in `passi.txt`, Smart Pro, 10 Nov 22:02), so the next start ran the card check. Find which process and close it in `finish`
- [x] Undo the files already replaced when an update stops with an error
- [x] Replace `volatile` status fields with atomics or a mutex in the worker threads
- [x] Do not grow the card or mount it for writing after a failed disk check, and recover the partition table from its backup copy (new images since 0.9.15)

## Hardware

- [ ] Brick Pro: speaker hiss at idle ([#4](https://github.com/diapolon/mosaik_cfw/issues/4), trimui/firmware_brickpro#5). In test: Settings → Display and sound → Speaker at rest (`speaker_rest`), amplifier off after 4 s without sound in the launcher, method A `/sys/class/speaker/mute` or B `HpSpeaker Switch`. To decide by ear which pops less (A seemed fine at first, the hiss showed mostly during downloads), then keep one and choose the default
- [ ] SD not answering after waking from sleep (Smart Pro, seen once, not reproduced): `arcade-suspend` now checks the card after waking and the launcher offers to restart (`/run/sd-guasta`); if it happens again, look at the kernel messages it saves
- [ ] Bluetooth controllers
- [ ] Support for the original TrimUI Brick (TG3040)
- [x] Settings page for the Brick Pro's extra buttons (side HOME, F1, F2): list them on the console, choose what each one does, show the page only on models that have them
- [x] Analog stick calibration from the settings
- [x] LED indication while charging

## Emulators

- [ ] PlayStation with PCSX ReARMed (`Roms/PS`: .chd, .cue, .pbp, .m3u for several discs, disc change in the game menu): to test on console with real games; analog sticks passed to the cores (DualShock chosen from the core's controller list, left stick stops acting as D-pad when the game reads the sticks), to test on console with an analog game; release sources must include the libpicofe submodule and `pcsx-rearmed-lr.patch`
- [ ] Dreamcast (shown greyed as "coming soon" on the Platforms screen: `"planned": true` in `Emulators/dc.json`)
- [ ] MAME 2010 core, for games missing from FinalBurn Neo and MAME 2003-Plus
- [ ] Flycast core for Naomi and Atomiswave (check performance first)
- [x] NES (FCEUmm), SNES (Snes9x), Mega Drive, Master System and Game Gear (Genesis Plus GX), PC Engine (Beetle PCE Fast), Game Boy and Game Boy Color (Gambatte, with the cartridge clock), GBA (mGBA), Neo Geo Pocket (Beetle NeoPop): zipped ROMs and battery saves (0.9.17)

## Launcher

- [ ] Backup and restore: Tools → Backup makes one zip on the card (saves, settings, favorites, collections, play time, Wi-Fi networks; optionally images and ROMs). Copy it to a computer, write the card again, put the zip back: at the first start the console finds it and offers to import it. Pairs with the reinstall notice (`m2/area-avvio.conf`)
- [ ] File transfer over Wi-Fi (Settings → Tools → Transfer files, 0.9.19, `launcher/fileserver.c`, `m2/test-fileserver.sh`): tested on a console from a Mac and an Android phone (fast, mosaik.local opens). Still to test: power-off during an upload (the `.part` file is removed at the next opening), a PS disc, `mosaik.local` from Windows, "Remember this browser"
- [ ] Console scraper, phase 2: ScreenScraper as a second source (needs developer credentials, requested on the screenscraper.fr forum)
- [ ] Game list cache, for very large libraries
- [ ] Apps section in the launcher
- [x] Console scraper: game data from libretro-database by the ROM's CRC, box art from the libretro thumbnails (0.9.17)
- [x] Scraper checks (audit R08): database at a fixed commit, DAT files validated before replacing the copy on the card, multi-ROM zips get no data, network loss stops the run without marking images as missing, write errors retried; `m2/test-scraper.sh`
- [x] Boot area generation visible (audit R07): the system carries its images' generation in `/etc/area-avvio`, Settings → Updates → Information shows the card's and warns when it is older; wiki: writing the card again without losing anything (pushed with the next stable)
- [x] Update as one transaction (audit R03, [#3](https://github.com/diapolon/mosaik_cfw/issues/3)): journal `System/update-journal.txt`, originals kept as `.annulla` until the first menu with loadable cores, `system-init` puts the old files back after an interrupted install or a fallback; tested on console with a forced power-off mid-install (0.9.18). Only works when falling back to a system that has this code

## Project

- [ ] Website on mosaikcfw.org
- [ ] English version of the build documentation (`m2/README.md`)
