#!/usr/bin/env python3
"""Prepara il sito in dist/site: pagina, immagini SD compresse e aggiornamento per la console.

    python3 toolchain/make-site.py

Va lanciato dopo m2/build-m2.sh. Il risultato è fatto solo di file statici, da caricare dove si
vuole: l'indirizzo della cartella update/ va poi scritto nel file UPDATE_URL e il sistema
ricostruito, così le console sanno dove cercare gli aggiornamenti (vedi site/README.md).
"""
import gzip
import hashlib
import os
import shutil
import subprocess
import sys

top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dist, src, out = os.path.join(top, "dist"), os.path.join(top, "site"), os.path.join(top, "dist", "site")
if not os.path.exists(os.path.join(dist, "arcade-version.txt")):
    sys.exit("manca dist/arcade-version.txt: prima sh m2/build-m2.sh")
version, stamp = open(os.path.join(dist, "arcade-version.txt")).read().split()
shutil.rmtree(out, ignore_errors=True)
os.makedirs(os.path.join(out, "download"))
shutil.copytree(os.path.join(src, "assets"), os.path.join(out, "assets"))
subprocess.run([sys.executable, os.path.join(top, "toolchain", "update-server.py"), "--out", os.path.join(out, "update")], check=True)


def human(n):
    return f"{n / (1 << 30):.1f} GB".replace(".", ",") if n >= 1 << 30 else f"{n // (1 << 20)} MB"


values = {"VERSION": version, "DATE": f"{stamp[6:8]}/{stamp[4:6]}/{stamp[:4]}"}
for key, model in (("SMART", "smartpro"), ("BRICK", "brickpro")):
    name = f"arcade-{version}-{model}.img.gz"
    path = os.path.join(out, "download", name)
    # mtime=0: stesso contenuto, stesso file compresso
    with open(os.path.join(dist, f"m2-{model}.img"), "rb") as f, open(path, "wb") as raw, \
            gzip.GzipFile(filename="", fileobj=raw, mode="wb", compresslevel=6, mtime=0) as z:
        shutil.copyfileobj(f, z, 1 << 20)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    values |= {f"{key}_FILE": name, f"{key}_SIZE": human(os.path.getsize(path)), f"{key}_SHA": h.hexdigest()}
    print(name, human(os.path.getsize(path)))

page = open(os.path.join(src, "index.html"), encoding="utf-8").read()
for k, v in values.items():
    page = page.replace("{{" + k + "}}", v)
assert "{{" not in page, "segnaposto non sostituito"
open(os.path.join(out, "index.html"), "w", encoding="utf-8").write(page)
total = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
biggest = max(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
print(f"sito pronto in {out}: {human(total)}, file più grande {human(biggest)}")
