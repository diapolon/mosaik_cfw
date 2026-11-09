# To do

## Testing

- [x] Full test on the Brick Pro
- [x] Automatic return to the previous system on a Smart Pro (broken system, three failed boots)
- [ ] Power loss during an update, on a real console
- [ ] Wi-Fi password containing quotes, on a real console
- [ ] First run on a brand new card
- [ ] Brick Pro freeze a few seconds after power-on, after a long time off: confirm that starting Wi-Fi later avoids it

## Reliability

- [ ] Make updates a complete transaction (journal with backups, or A/B slots): today an error or a rollback can leave files from two versions on the card
- [x] Replace `volatile` status fields with atomics or a mutex in the worker threads
- [ ] Do not grow the card or mount it for writing after a failed disk check, and recover the partition table from its backup copy (needs a new image)
- [ ] Full ROM check that decompresses the data, as an option next to the quick check
- [ ] Test matrix per console and feature, with version and date

## Features

- [ ] Bluetooth controllers
- [x] Settings page for the Brick Pro's extra buttons (side HOME, F1, F2): list them on the console, choose what each one does, show the page only on models that have them
- [x] Analog stick calibration from the settings
- [x] LED indication while charging
- [ ] Game list cache, for very large libraries
- [ ] More consoles (NES, SNES, Mega Drive, Game Boy, GBA, PS1)
- [ ] MAME 2010 core, for games missing from FinalBurn Neo and MAME 2003-Plus
- [ ] Flycast core for Naomi and Atomiswave (check performance first)
- [ ] Apps section in the launcher
- [ ] Support for the original TrimUI Brick (TG3040)

## Project

- [ ] Website on mosaikcfw.org
- [ ] English version of the build documentation (`m2/README.md`)
