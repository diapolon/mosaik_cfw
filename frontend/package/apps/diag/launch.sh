#!/bin/sh
# Diagnostica (app dello Store): parte come un gioco Linux; ROOT è la cartella Arcade
cd "$(dirname "$0")" || exit 1
exec ./diag --root "${ROOT:-/mnt/SDCARD/Arcade}" --app "$PWD"
