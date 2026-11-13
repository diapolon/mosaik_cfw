#!/bin/sh
# Spazio della SD (app dello Store): parte come un gioco Linux; ROOT è la cartella Arcade.
# Uscita 120: system-init spegne la console (per finire l'allargamento all'accensione)
cd "$(dirname "$0")" || exit 1
exec ./sdgrow --root "${ROOT:-/mnt/SDCARD/Arcade}" --app "$PWD"
