#!/bin/sh
# Freedoom, fase 1 (gli episodi di Doom): vedi doom.sh
D="$(cd "$(dirname "$0")" && pwd)"
exec sh "$D/doom.sh" "$D/freedoom1.wad"
