#!/bin/sh
# Immagini e fumetti (app dello Store): parte come un gioco Linux; ROOT è la cartella Arcade
cd "$(dirname "$0")" || exit 1
exec ./viewer --root "${ROOT:-/mnt/SDCARD/Arcade}" --app "$PWD"
