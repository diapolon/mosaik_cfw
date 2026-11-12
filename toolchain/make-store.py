#!/usr/bin/env python3
"""Pacchetti dello Store e catalogo firmato.

    python3 toolchain/make-store.py [--out dist/site/store]

Prende dist/store-src/<pacchetto>/ (preparati da toolchain/package.sh: i file con i percorsi
della scheda sotto Arcade/) e scrive in --out:
  store-<pacchetto>-<versione>.zip   i file del pacchetto più pacchetto.json
  store.json                         il catalogo: per ogni pacchetto nome, console, versione,
                                     misura, impronta SHA-256 e file installati
  store.sig                          la firma del catalogo (stessa chiave degli aggiornamenti)
La versione è l'impronta del contenuto: cambia solo se cambia un file. L'etichetta mostrata è il
commit del core (o la versione di Deadcast). La console scarica catalogo e firma dallo stesso
indirizzo degli aggiornamenti (canale Stabile o Sviluppo)."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import zipfile

top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--src", default=os.path.join(top, "dist", "store-src"))
ap.add_argument("--out", default=os.path.join(top, "dist", "site", "store"))
ap.add_argument("--key", default=os.path.expanduser("~/.config/arcade-update/update-key.pem"))
ap.add_argument("--labels", help="file 'cartella_sorgente commit' (altrimenti dal volume Docker)")
args = ap.parse_args()

# pacchetto -> (nome, cartella dei sorgenti in /build/src per il commit)
PACKAGES = {
    "fbneo": ("FinalBurn Neo", "fbneo"),
    "mame2003p": ("MAME 2003-Plus", "mame2003-plus"),
    "mame2010": ("MAME 2010", "mame2010"),
    "fceumm": ("FCEUmm", "fceumm"),
    "genesis_plus_gx": ("Genesis Plus GX", "genesis_plus_gx"),
    "snes9x": ("Snes9x", "snes9x"),
    "gambatte": ("Gambatte", "gambatte"),
    "mgba": ("mGBA", "mgba"),
    "pce_fast": ("Beetle PCE Fast", "pce_fast"),
    "ngp": ("Beetle NeoPop", "ngp"),
    "pcsx": ("PCSX ReARMed", "pcsx"),
    "deadcast": ("Deadcast", None),
}
# cartelle in cui un pacchetto può scrivere (la console controlla le stesse)
ALLOWED = ("Cores/", "Emulators/", "Dats/", "Apps/", "Themes/", "Filters/", "Bezels/")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def commits():
    if args.labels:
        text = open(args.labels).read()
    else:
        text = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", "trimui-build:/build", "trimui-tg5040",
                               "sh", "-c", 'for d in /build/src/*/; do [ -d "$d/.git" ] && echo "$(basename $d) $(git -C $d rev-parse --short=7 HEAD)"; done'],
                              capture_output=True, text=True).stdout
    return dict(l.split() for l in text.splitlines() if len(l.split()) == 2)


if not os.path.isdir(args.src):
    sys.exit(f"manca {args.src}: prima sh toolchain/package.sh (o m2/build-m2.sh)")
if not os.path.exists(args.key):
    sys.exit(f"manca la chiave privata {args.key}")
os.makedirs(args.out, exist_ok=True)
for n in os.listdir(args.out):   # i pacchetti di prima: restano solo quelli di questo catalogo
    if n.startswith("store"):
        os.remove(os.path.join(args.out, n))
labels = commits()
catalog = {"format": 1, "packages": []}
for pid in sorted(os.listdir(args.src)):
    root = os.path.join(args.src, pid)
    if pid not in PACKAGES or not os.path.isdir(root):
        sys.exit(f"pacchetto sconosciuto: {pid}")
    name, srcdir = PACKAGES[pid]
    files = []
    for d, _, names in os.walk(root):
        for n in names:
            rel = os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/")
            if not rel.startswith(ALLOWED) or ".." in rel:
                sys.exit(f"{pid}: {rel} fuori dalle cartelle permesse")
            files.append(rel)
    files.sort()
    h = hashlib.sha256()
    for rel in files:
        h.update(rel.encode() + b"\0" + sha256(os.path.join(root, rel)).encode() + b"\n")
    version = h.hexdigest()[:12]
    emulators, systems = [], []
    for rel in files:
        if rel.startswith("Emulators/") and rel.endswith(".json"):
            emulators.append(rel[10:-5])
            systems.append(json.load(open(os.path.join(root, rel))).get("name", rel[10:-5]))
    if pid == "deadcast":
        dz = sorted(n for n in os.listdir(os.path.join(top, "deps", "deadcast")) if n.endswith(".pak.zip"))
        label = dz[-1][12:-8] if dz else "?"
    else:
        label = labels.get(srcdir, "?")
    installed = sum(os.path.getsize(os.path.join(root, r)) for r in files)
    meta = {"id": pid, "name": name, "version": version, "label": label, "files": files}
    zpath = os.path.join(args.out, f"store-{pid}-{version}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        mi = zipfile.ZipInfo("pacchetto.json", (2026, 1, 1, 0, 0, 0))   # data fissa: zip uguale a contenuto uguale
        mi.compress_type = zipfile.ZIP_DEFLATED
        mi.external_attr = 0o644 << 16
        z.writestr(mi, json.dumps(meta, ensure_ascii=False, indent=1) + "\n", compresslevel=9)
        for rel in files:
            info = zipfile.ZipInfo.from_file(os.path.join(root, rel), rel)
            info.date_time = (2026, 1, 1, 0, 0, 0)   # zip uguale a contenuto uguale
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(os.path.join(root, rel), "rb") as f:
                z.writestr(info, f.read(), compresslevel=9)
    catalog["packages"].append({
        "id": pid, "name": name, "label": label, "version": version, "systems": systems, "emulators": emulators,
        "file": os.path.basename(zpath), "size": os.path.getsize(zpath), "installed": installed,
        "sha256": sha256(zpath), "files": files,
    })
    print(f"  {pid:16} {label:10} {os.path.getsize(zpath) / 1e6:6.1f} MB  {', '.join(systems)}")

# ordine delle piattaforme (campo "order" degli emulatori): prima gli arcade, poi le console
def order(pk):
    return min((json.load(open(os.path.join(args.src, pk["id"], "Emulators", e + ".json"))).get("order", 100) for e in pk["emulators"]), default=100)
catalog["packages"].sort(key=lambda pk: (order(pk), pk["name"].lower()))
text = (json.dumps(catalog, ensure_ascii=False, indent=1) + "\n").encode()
with tempfile.NamedTemporaryFile() as t:
    t.write(text)
    t.flush()
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", args.key, t.name], check=True, capture_output=True).stdout
open(os.path.join(args.out, "store.json"), "wb").write(text)
open(os.path.join(args.out, "store.sig"), "wb").write(sig)
print(f"Store: {len(catalog['packages'])} pacchetti in {args.out}")
