#!/bin/sh
# Esplora file (app dello Store): parte come un gioco Linux; ROOT è la cartella Arcade
cd "$(dirname "$0")" || exit 1
exec ./files --root "${ROOT:-/mnt/SDCARD/Arcade}" --app "$PWD"
