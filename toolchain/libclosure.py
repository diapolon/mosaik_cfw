#!/usr/bin/env python3
"""Librerie necessarie (NEEDED, ricorsivo) a un insieme di programmi, cercate in un rootfs.
  libclosure.py ROOT prog1 [prog2 ...] [+libextra.so ...]   -> elenco di percorsi nel ROOT"""
import os, subprocess, sys
root = sys.argv[1]
dirs = ["lib", "usr/lib", "usr/trimui/lib", "lib64"]
def find(name):
    for d in dirs:
        p = os.path.join(root, d, name)
        if os.path.exists(p): return p
    return None
def needed(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return [l.split("[")[1].split("]")[0] for l in out.splitlines() if "(NEEDED)" in l]
todo, seen, missing = [], {}, set()
for a in sys.argv[2:]:
    if a.startswith("+"):
        p = find(a[1:]); (todo.append(p) if p else missing.add(a[1:]))
    else: todo.append(a)
while todo:
    p = todo.pop()
    if p in seen.values(): continue
    seen[os.path.basename(p)] = p
    for n in needed(p):
        if n in seen: continue
        q = find(n)
        if q: todo.append(q)
        else: missing.add(n)
for n, p in sorted(seen.items()):
    if p.startswith(root): print(os.path.relpath(p, root))
for m in sorted(missing): print("MANCA", m, file=sys.stderr)
