# To do

## Testing

- [x] Full test on the Brick Pro
- [x] Automatic return to the previous system on a Smart Pro (broken system, three failed boots)
- [ ] Power loss during an update, on a real console
- [ ] Wi-Fi password containing quotes, on a real console

## Reliability

- [ ] Make updates a complete transaction (journal with backups, or A/B slots): today an error or a rollback can leave files from two versions on the card
- [ ] Replace `volatile` status fields with atomics or a mutex in the worker threads
- [ ] Full ROM check that decompresses the data, as an option next to the quick check
- [ ] Test matrix per console and feature, with version and date

## Features

- [ ] Bluetooth controllers
- [ ] Settings page for the Brick Pro's extra buttons (side HOME, F1, F2): list them on the console, choose what each one does, show the page only on models that have them
- [ ] Analog stick calibration from the settings (the factory data is in internal storage; today the only workaround is copying `joypad.config` to `Arcade/System` by hand)
- [ ] LED indication while charging
- [ ] Game list cache, for very large libraries
- [ ] More consoles (NES, SNES, Mega Drive, Game Boy, GBA, PS1)
- [ ] MAME 2010 core, for games missing from FinalBurn Neo and MAME 2003-Plus
- [ ] Flycast core for Naomi and Atomiswave (check performance first)
- [ ] Apps section in the launcher
- [ ] Support for the original TrimUI Brick (TG3040)

## Project

- [ ] Website on mosaikcfw.org
- [ ] English version of the build documentation (`m2/README.md`)
