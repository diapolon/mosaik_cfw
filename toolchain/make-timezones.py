#!/usr/bin/env python3
"""Scrive frontend/src/common/timezones.h: tutti i fusi orari del database tz (zone.tab, uno o più
per paese, con il nome della città che la gente cerca) e la regola POSIX di ognuno, presa dalla
riga finale dei file TZif. La console non ha /usr/share/zoneinfo: il launcher passa la regola
POSIX in TZ (settings_apply_tz) e la copia in System/fuso-orario.txt per l'avvio.

Uso: python3 toolchain/make-timezones.py [cartella zoneinfo, di solito /usr/share/zoneinfo]
Da rilanciare quando cambiano le regole (ora legale abolita o spostata in qualche paese)."""
import os
import re
import sys

top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
zi = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/zoneinfo"
out = os.path.join(top, "frontend", "src", "common", "timezones.h")


def footer(name):
    """la regola POSIX in fondo a un file TZif di versione 2 o più"""
    data = open(os.path.join(zi, name), "rb").read()
    if data[:4] != b"TZif" or data[4:5] < b"2":
        sys.exit(f"{name}: file TZif senza la regola finale (serve la versione 2)")
    tail = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode("ascii")
    if not tail:
        sys.exit(f"{name}: regola finale vuota")
    return tail


def std_minutes(posix):
    """scostamento da UTC dell'ora solare, in minuti (CET-1 -> +60, <-03>3 -> -180)"""
    m = re.match(r"^(?:<[^>]+>|[A-Za-z]+)([+-]?)(\d+)(?::(\d+))?", posix)
    if not m:
        sys.exit(f"regola non capita: {posix}")
    mins = int(m.group(2)) * 60 + int(m.group(3) or 0)
    return -mins if m.group(1) != "-" else mins


ids = set()
for line in open(os.path.join(zi, "zone.tab"), encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    ids.add(line.split("\t")[2].strip())
rows = [("UTC", "UTC0", 0)]
for i in sorted(ids):
    p = footer(i)
    rows.append((i, p, std_minutes(p)))
version = open(os.path.join(zi, "+VERSION")).read().strip() if os.path.exists(os.path.join(zi, "+VERSION")) else "?"
with open(out, "w", encoding="utf-8") as f:
    f.write(f"/* Generato da toolchain/make-timezones.py (database tz {version}): non modificare a mano.\n"
            "   nome, regola POSIX per TZ, scostamento dell'ora solare da UTC in minuti */\n")
    f.write("const timezone_t TIMEZONES[] = {\n")
    for i, p, m in rows:
        f.write(f'    {{ "{i}", "{p}", {m} }},\n')
    f.write("};\n")
print(f"{len(rows)} fusi orari (tz {version}) in {os.path.relpath(out, top)}")
