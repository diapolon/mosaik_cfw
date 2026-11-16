#!/bin/sh
# Scarica file (app dello Store): parte come un gioco Linux; ROOT è la cartella Arcade
cd "$(dirname "$0")" || exit 1
exec ./urlget --root "${ROOT:-/mnt/SDCARD/Arcade}" --app "$PWD"
