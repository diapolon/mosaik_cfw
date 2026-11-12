#!/usr/bin/env python3
"""Analisi del registro dei consumi della console (Arcade/Logs/consumi.tsv, scritto da m2/consumi
con il registro diagnostico "Prestazioni" o "Completo").

  python3 toolchain/consumi.py /Volumes/MOSAIK/Arcade/Logs/consumi.tsv [consumi.tsv.1 ...]

Per ogni stato (menu, gioco con il suo core, gioco Linux, emulatore a sé): tempo misurato,
corrente media dalla batteria, batteria consumata all'ora, processore, frequenza, temperatura,
memoria disponibile. Poi lo standby (i salti nell'ora fra due misure: la console dormiva) e la
memoria di launcher e runner nel tempo, per vedere se cresce (perdite in una sessione lunga)."""
import statistics
import sys
from collections import defaultdict
from datetime import datetime


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def load(paths):
    rows = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            head = None
            for line in f:
                cols = line.rstrip("\n").split("\t")
                if cols[0] == "ora":
                    head = cols
                    continue
                if not head or len(cols) != len(head):
                    continue
                r = dict(zip(head, cols))
                try:
                    r["t"] = datetime.fromisoformat(r["ora"])
                except ValueError:
                    continue
                rows.append(r)
    rows.sort(key=lambda r: r["t"])
    return rows


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rows = load(sys.argv[1:])
    if len(rows) < 2:
        print("troppo poche misure")
        return 1
    gaps = [(b["t"] - a["t"]).total_seconds() for a, b in zip(rows, rows[1:])]
    step = statistics.median([g for g in gaps if g > 0] or [30])
    print(f"{len(rows)} misure dal {rows[0]['ora']} al {rows[-1]['ora']}, una ogni {step:.0f} s circa\n")

    # stati: il tempo fra due misure vicine conta per lo stato della prima
    by = defaultdict(lambda: {"s": 0.0, "ma": [], "cpu": [], "mhz": [], "temp": [], "mem": [], "drop": 0.0, "dt": 0.0})
    sleeps = []
    for a, b in zip(rows, rows[1:]):
        dt = (b["t"] - a["t"]).total_seconds()
        if dt <= 0:
            continue
        if dt > 3 * step:   # salto: console sospesa (o spenta)
            ca, cb = num(a["batteria_%"]), num(b["batteria_%"])
            if ca is not None and cb is not None and a["usb"] != "1" and b["usb"] != "1":
                sleeps.append((dt, ca - cb, num(a["mV"]), num(b["mV"])))
            continue
        key = (a["stato"] + " " + a["gioco"]).strip()
        st = by[key]
        st["s"] += dt
        for k, col in (("ma", "mA"), ("cpu", "cpu_%"), ("mhz", "mhz"), ("temp", "temp_C"), ("mem", "mem_disp_kB")):
            v = num(a[col])
            if v is not None:
                st[k].append(v)
        ca, cb = num(a["batteria_%"]), num(b["batteria_%"])
        if ca is not None and cb is not None and a["usb"] != "1":
            st["drop"] += ca - cb
            st["dt"] += dt

    def avg(v, fmt="{:.0f}"):
        return fmt.format(statistics.mean(v)) if v else "-"

    print(f"{'stato':28} {'minuti':>7} {'mA':>6} {'%/h':>6} {'cpu%':>5} {'MHz':>6} {'°C':>4} {'mem disp. min':>14}")
    for key, st in sorted(by.items(), key=lambda kv: -kv[1]["s"]):
        rate = f"{st['drop'] / (st['dt'] / 3600):.1f}" if st["dt"] >= 600 else "-"
        mem = f"{min(st['mem']) / 1024:.0f} MB" if st["mem"] else "-"
        print(f"{key[:28]:28} {st['s'] / 60:7.1f} {avg(st['ma']):>6} {rate:>6} {avg(st['cpu']):>5} {avg(st['mhz']):>6} {avg(st['temp']):>4} {mem:>14}")
    print("  (mA: corrente dalla batteria, negativa se si scarica; %/h solo con almeno 10 minuti senza caricatore)")

    print()
    if sleeps:
        h = sum(s[0] for s in sleeps) / 3600
        drop = sum(s[1] for s in sleeps)
        print(f"standby: {len(sleeps)} sospensioni, {h:.1f} ore, batteria -{drop:.0f}%, {drop / h:.2f} %/h" if h > 0 else "standby: -")
        for dt, d, va, vb in sleeps:
            mv = f", {va:.0f} → {vb:.0f} mV" if va and vb else ""
            print(f"  {dt / 3600:5.1f} h: -{d:.0f}%{mv}")
    else:
        print("standby: nessuna sospensione misurata")

    print()
    for prog, col in (("launcher", "launcher_kB"), ("runner", "runner_kB")):
        pts = [(r["t"], num(r[col])) for r in rows if num(r[col])]
        if len(pts) < 2:
            continue
        first, last, top = pts[0][1], pts[-1][1], max(v for _, v in pts)
        print(f"{prog}: memoria {first / 1024:.1f} → {last / 1024:.1f} MB ({(last - first) / 1024:+.1f} MB), massimo {top / 1024:.1f} MB")
    big = defaultdict(float)
    for r in rows:
        name, _, kb = r["processo_piu_grande"].partition(":")
        if num(kb):
            big[name] = max(big[name], num(kb))
    if big:
        print("processi più grandi (massimo): " + ", ".join(f"{n} {v / 1024:.0f} MB" for n, v in sorted(big.items(), key=lambda kv: -kv[1])[:5]))
    mems = [num(r["mem_disp_kB"]) for r in rows if num(r["mem_disp_kB"])]
    if mems:
        print(f"memoria disponibile: minimo {min(mems) / 1024:.0f} MB, media {statistics.mean(mems) / 1024:.0f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
