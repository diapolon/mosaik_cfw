#!/usr/bin/env python3
"""Partite di un gioco Linux dello Store in Arcade/Saves/<cartella>, invece che in save/ accanto
al programma: così entrano nel backup e restano togliendo il pacchetto.

    python3 toolchain/linux-game-saves.py launch.sh CARTELLA

Riscrive il launch.sh di un gioco che usa "save" e "$PWD/save": dopo la
riga `cd "$(dirname "$0")"` definisce SAVE, copia una volta la cartella save/ di un gioco messo a
mano prima dello Store, poi sostituisce i riferimenti. Un launch.sh che usa già Arcade/Saves
(USERDIR) resta com'è. Usato da toolchain/package.sh."""
import re
import sys

path, folder = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
if re.search(r'^[^#\n]*\$\{?ROOT\}?/Saves', text, re.M):
    sys.exit(0)   # usa già Arcade/Saves
anchor = re.search(r'^cd "\$\(dirname "\$0"\)"[^\n]*\n', text, re.M)
if not anchor:
    sys.exit(f"{path}: manca la riga cd \"$(dirname \"$0\")\"")
head = (
    '# Store di Mosaik: partite e registro in Arcade/Saves/%s (backup, e restano togliendo il gioco);\n'
    '# la cartella save/ di un gioco copiato a mano prima si porta dietro una volta\n'
    'SAVE="${ROOT:-/mnt/SDCARD/Arcade}/Saves/%s"\n'
    '[ -d save ] && [ ! -d "$SAVE" ] && mkdir -p "$SAVE" && cp -R save/. "$SAVE"/\n' % (folder, folder))
body = text[anchor.end():]
body = body.replace('"$PWD/save"', '"$SAVE"')
body = re.sub(r'(?<![\w/.$"])save/', '"$SAVE"/', body)
body = re.sub(r'mkdir -p save\b', 'mkdir -p "$SAVE"', body)
if re.search(r'(?<![\w/.$"])save(?![\w"])', body):
    sys.exit(f"{path}: resta un riferimento a save che non so cambiare")
open(path, "w", encoding="utf-8").write(text[:anchor.end()] + head + body)
