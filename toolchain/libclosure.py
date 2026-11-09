#!/usr/bin/env python3
"""Librerie necessarie (NEEDED, ricorsivo) a un insieme di programmi, cercate in un rootfs.
  libclosure.py ROOT prog1 [prog2 ...] [+libextra.so ...]   -> elenco di percorsi nel ROOT
Esce con errore se manca una libreria, se readelf fallisce o se un programma fuori dal rootfs
(launcher, runner, core) chiede una versione di simboli che le librerie del firmware non hanno:
un'immagine così non partirebbe sulla console."""
import os, subprocess, sys
root = sys.argv[1]
dirs = ["lib", "usr/lib", "usr/trimui/lib", "lib64"]
def find(name):
    for d in dirs:
        p = os.path.join(root, d, name)
        if os.path.exists(p): return p
    return None
errors = []
def readelf(opt, path):
    r = subprocess.run(["readelf", opt, "-W", path], capture_output=True, text=True)
    if r.returncode != 0:
        errors.append("readelf non riuscito su %s: %s" % (path, r.stderr.strip()))
    return r.stdout
def needed(path):
    return [l.split("[")[1].split("]")[0] for l in readelf("-d", path).splitlines() if "(NEEDED)" in l]
def versions(path):
    """(richieste [(libreria, versione)], versioni definite)"""
    wanted, defined, mode, lib = [], set(), None, None
    for l in readelf("-V", path).splitlines():
        if l.startswith("Version needs section"): mode = "need"
        elif l.startswith("Version definition section"): mode = "def"
        elif l.startswith("Version symbols section"): mode = None
        elif mode == "need" and "File: " in l: lib = l.split("File: ")[1].split()[0]
        elif mode == "need" and "Name: " in l: wanted.append((lib, l.split("Name: ")[1].split()[0]))
        elif mode == "def" and "Name: " in l and "Rev:" in l: defined.add(l.split("Name: ")[1].split()[0])
    return wanted, defined
todo, seen, missing = [], {}, set()
ours = []
for a in sys.argv[2:]:
    if a.startswith("+"):
        p = find(a[1:]); (todo.append(p) if p else missing.add(a[1:]))
    else:
        todo.append(a)
        if not a.startswith(root): ours.append(a)
while todo:
    p = todo.pop()
    if p in seen.values(): continue
    seen[os.path.basename(p)] = p
    for n in needed(p):
        if n in seen: continue
        q = find(n)
        if q: todo.append(q)
        else: missing.add(n)
for prog in ours:
    for lib, ver in versions(prog)[0]:
        if lib in seen and ver not in versions(seen[lib])[1]:
            errors.append("%s chiede %s da %s, che non ce l'ha" % (os.path.basename(prog), ver, lib))
for n, p in sorted(seen.items()):
    if p.startswith(root): print(os.path.relpath(p, root))
for m in sorted(missing): print("MANCA", m, file=sys.stderr)
for e in errors: print("ERRORE", e, file=sys.stderr)
if missing or errors: sys.exit(1)
