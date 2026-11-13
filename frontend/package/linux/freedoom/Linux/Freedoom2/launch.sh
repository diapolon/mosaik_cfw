#!/bin/sh
# Freedoom, fase 2 (come Doom II): il programma è nella cartella Freedoom, vedi doom.sh
D="$(cd "$(dirname "$0")" && pwd)"
exec sh "$D/../Freedoom/doom.sh" "$D/freedoom2.wad"
