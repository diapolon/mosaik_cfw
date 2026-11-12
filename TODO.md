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

- [x] Games with the same file name on two platforms that share a core (MD/SMS/GG, GB/GBC) shared saves, states, statistics and pictures: each platform kept apart, existing files moved (`launcher/savesplit.c`, [#5](https://github.com/diapolon/mosaik_cfw/issues/5), 0.9.18)
- [ ] Neo Geo Pocket: flash save now goes through the frontend as `.srm` (`toolchain/patches/beetle-ngp-saveram.patch`, [#6](https://github.com/diapolon/mosaik_cfw/issues/6)); to test on console with a real game that saves to flash: forced power-off, sleep, low battery, 0.9.17 `.flash` migration
- [ ] Survive a power loss while files are being replaced (journal or A/B slots); the rollback after three failed boots restores only the system ([#3](https://github.com/diapolon/mosaik_cfw/issues/3))
- [x] Full ROM check that decompresses the data, as an option next to the quick check (Verify ROMs → Full, `romcheck_zip_verify`, `m2/test-romcheck.sh`; also in the diagnosis of a game that doesn't start): works on a Brick Pro: 29 FBNeo zips (362 MB) checked in about 10 s, all complete (11 Nov)
- [ ] SD not closed cleanly on restart (9 and 10 Nov, Smart Pro): the logged open file was the listing itself (its output went to `passi.txt`), so most likely the stopped programs had not closed their files yet. `finish` now retries the read-only remount for 2 s and, if it still fails, logs the real open files and the stopped programs: to confirm on a console (no "SD non chiusa" in `passi.txt` and no card check after restarts)
- [x] Undo the files already replaced when an update stops with an error
- [x] Replace `volatile` status fields with atomics or a mutex in the worker threads
- [x] Do not grow the card or mount it for writing after a failed disk check, and recover the partition table from its backup copy (new images since 0.9.15)

## Hardware

- [ ] RAM use, for the whole system and above all the base system (measured by `m2/consumi` into `Logs/consumi.tsv` with the Performance diagnostic log, analysed by `toolchain/consumi.py`; `m2/test-consumi.sh`): after boot at the home screen (kernel, `system-init` and its services: `trimui_inputd`, `udevd`, `wpa_supplicant`, the launcher with its themes, images and music), then in each kind of game (runner with every core, Linux games, Deadcast) and back in the menu after many game starts, to find leaks in the launcher and runner over a long session; both models, with the numbers in `Logs` so each version can be compared
- [ ] Battery drain, for the whole system and above all the base system (same log; a sample just before and after each sleep): the launcher idle at the home screen (screen on and dimmed), the services running in the background, Wi-Fi on and off (scans, reconnects), sleep (overnight: battery percentage and the AXP2202 readings before and after), and then each kind of game; look for anything that keeps the CPU awake or the clock high (launcher redraws when nothing changes, polling loops, background threads, CPU governor and profiles), on both models
- [ ] Brick Pro: speaker hiss at idle ([#4](https://github.com/diapolon/mosaik_cfw/issues/4), trimui/firmware_brickpro#5). In test: Settings → Display and sound → Speaker at rest (`speaker_rest`), amplifier off after 4 s without sound in the launcher, method A `/sys/class/speaker/mute` or B `HpSpeaker Switch`. To decide by ear which pops less (A seemed fine at first, the hiss showed mostly during downloads), then keep one and choose the default
- [ ] PMIC interrupt enables after a direct power-off: `axpctl poweroff` (charger connected, issue #2) copies NextUI's `poweroff_next` and sets 0x40-0x44 to 0; on NextUI devices those stay 0 across power cycles, while a Smart Pro S that never ran NextUI reads `10 f3 01 0f 60` (LoveRetro/NextUI#837, #835: a Brick Pro whose power button stopped responding after a power-off). `axpctl restore` at boot only fixes bit 4 of 0x22. Now the registers are logged at every boot (diagnostic log on): read them on a Brick Pro and a Smart Pro, after a normal and after a charger power-off, then decide whether `restore` puts 0x40-0x44 back
- [ ] SD not answering after waking from sleep (Smart Pro, seen once, not reproduced): `arcade-suspend` now checks the card after waking and the launcher offers to restart (`/run/sd-guasta`); if it happens again, look at the kernel messages it saves
- [ ] Bluetooth controllers
- [ ] Support for the original TrimUI Brick (TG3040)
- [x] Settings page for the Brick Pro's extra buttons (side HOME, F1, F2): list them on the console, choose what each one does, show the page only on models that have them
- [x] Analog stick calibration from the settings
- [x] LED indication while charging

## Emulators

- [ ] PlayStation with PCSX ReARMed (`Roms/PS`: .chd, .cue, .pbp, .m3u for several discs, disc change in the game menu): to test on console with real games; analog sticks passed to the cores (DualShock chosen from the core's controller list, left stick stops acting as D-pad when the game reads the sticks), to test on console with an analog game; release sources must include the libpicofe submodule and `pcsx-rearmed-lr.patch`
- [ ] Dreamcast with Deadcast 1.2.5 (author's package pinned by `toolchain/fetch-deadcast.sh`, in `Cores/deadcast` with Mosaik's `frontend/package/deadcast/launch.sh`; Vulkan driver and libcurl copied from the firmware into the system): tested on a Brick Pro with Daytona USA: runs very well, VMU and save states written, clean exit. Mosaik's menu did not show over it, and not because of the overlay memory: fb0 is 64 MB (1024x16384), the menu sat at row 15616 and Deadcast's frames at rows 12288–14687, so most likely Deadcast drives the display layer itself (the overlay now also avoids the game's rows; the menu still shows over Linux games, checked on a Brick Pro) and closing it from outside may lose the last VMU writes (no SIGTERM handling, buffered `fwrite`): `"menu": false` in `dc.json` → `GAMEWATCH_NOMENU=1`, so Menu and HOME are Deadcast's and the power button sleeps without asking. Asked in the draft to the author to quit cleanly on SIGTERM. Still to test on a Smart Pro (1280x720 screen). Release: attach the Deadcast source (with its `ui` submodule) to the release, it is GPL-2.0. Fallback if needed: Flycast libretro in the runner (needs hardware rendering in the runner; Flycast cloned in `deps/flycast`)
- [ ] MAME 2010 core (`Roms/MAME2010`, romset 0.139, `Emulators/mame2010.json`, index from `metadata/mame2010.xml`), for games missing from FinalBurn Neo and MAME 2003-Plus: runs on the Mac and on ARM64 (Bubble Bobble, Snow Bros.); to test on a console for speed. The Neo Geo BIOS must sit next to the games (the core looks only in the game's folder)
- [ ] Flycast core for Naomi and Atomiswave (check performance first)
- [x] NES (FCEUmm), SNES (Snes9x), Mega Drive, Master System and Game Gear (Genesis Plus GX), PC Engine (Beetle PCE Fast), Game Boy and Game Boy Color (Gambatte, with the cartridge clock), GBA (mGBA), Neo Geo Pocket (Beetle NeoPop): zipped ROMs and battery saves (0.9.17)

## Launcher

- [ ] Backup and restore over Wi-Fi (Transfer files page: **Download backup** with saves, settings, favorites, collections, play time and BIOS, optionally Wi-Fi networks and images; **Restore backup** uploads the zip, the console asks for confirmation, writes each file as `.part` and restarts; `m2/test-fileserver.sh`): to test on a console, also between the two models. A backup zip copied to the card (main folder or `Arcade/`) is offered for import at the first start, after the language on a new card, once per file (`System/backup-visti.txt`): to test on a console with a rewritten card. Optional **Also the games** (Roms and Linux, shown with its size, off above 3.5 GB); a restore without games says to copy the ROMs again
- [ ] File transfer over Wi-Fi (Settings → Tools → Transfer files, 0.9.19, `launcher/fileserver.c`, `m2/test-fileserver.sh`): tested on a console from a Mac and an Android phone (fast, mosaik.local opens). Still to test: power-off during an upload (the `.part` file is removed at the next opening), a PS disc, `mosaik.local` from Windows, "Remember this browser"
- [ ] Console scraper, phase 2: ScreenScraper as a second source (needs developer credentials, requested on the screenscraper.fr forum). It is also the only source for the data of disc games: images of PlayStation and Dreamcast games are already found by name in the libretro thumbnails (Daytona USA: box, snap, title), but libretro-database (at `LR_REF`) has no release year, genre, publisher or players for them, only `developer` (22 Dreamcast games, matched by the CRC of track 1), and `.chd` files are not read for a CRC
- [ ] Reset settings and Erase everything (Settings → Tools, `rst_*` in launcher.c): tested on the Mac; to test on a console
- [ ] Game list cache, for very large libraries: measured on a Brick Pro with 33 games, the library reads in 0.01 s (first screen at about 4.9 s from kernel start), so not needed for now
- [ ] Apps section in the launcher (`Arcade/Apps/<app>/launch.sh`, `app.json`, `icon.png`; home entry only when there are apps): tested on the Mac in the three themes; to test on a console with a real app, and document the format in `frontend/README.md`
- [ ] Packages and Store: every emulator is a package (`toolchain/make-store.py`, `launcher/store.c`, Settings → Card and data → Store, `m2/test-store.sh`); offline install by copying `store-*.zip` to the card; a new card offers the Store. Tested on the Mac with the real signed catalogue. Next: test on a console (new card and updated card), publish the Store files on the stable Release too, apps and themes as packages, show not-installed platforms in Platforms
- [ ] Built-in apps, shipped with the system (not in `Arcade/Apps`, so Erase everything keeps them; the Apps entry is then always there), built from `frontend/src/apps/` with the launcher's UI library and started by `system-init` like the runner (a new `app` kind in `next.txt`, `run-mac.sh` too):
  - [ ] File explorer: browse the card, copy, move, rename and delete files and folders, free space
  - [ ] Comic and image viewer: images, folders of images and `.cbz` comics, zoom and next page (game manuals too)
  - [ ] SD card test: read and write speed, and write-then-verify to find fake or failing cards
  - [ ] Music player: the `Music` folder with track list, pause, next, repeat, also with the screen off
- [x] Console scraper: game data from libretro-database by the ROM's CRC, box art from the libretro thumbnails (0.9.17)
- [x] Scraper checks (audit R08): database at a fixed commit, DAT files validated before replacing the copy on the card, multi-ROM zips get no data, network loss stops the run without marking images as missing, write errors retried; `m2/test-scraper.sh`
- [x] Boot area generation visible (audit R07): the system carries its images' generation in `/etc/area-avvio`, Settings → Updates → Information shows the card's and warns when it is older; wiki: writing the card again without losing anything (pushed with the next stable)
- [x] Update as one transaction (audit R03, [#3](https://github.com/diapolon/mosaik_cfw/issues/3)): journal `System/update-journal.txt`, originals kept as `.annulla` until the first menu with loadable cores, `system-init` puts the old files back after an interrupted install or a fallback; tested on console with a forced power-off mid-install (0.9.18). Only works when falling back to a system that has this code

## Project

- [ ] Website on mosaikcfw.org
- [ ] English version of the build documentation (`m2/README.md`)
