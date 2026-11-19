#!/usr/bin/env python3
"""Prepara il sito in dist/site: pagina, immagini SD compresse e aggiornamento per la console.

    python3 toolchain/make-site.py

Va lanciato dopo m2/build-m2.sh. Il risultato è fatto solo di file statici, da caricare dove si
vuole: l'indirizzo della cartella update/ va poi scritto nel file UPDATE_URL e il sistema
ricostruito, così le console sanno dove cercare gli aggiornamenti (vedi site/README.md).
"""
import gzip
import hashlib
import json
import glob, os
import shutil
import subprocess
import sys

top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dist, src, out = os.path.join(top, "dist"), os.path.join(top, "site"), os.path.join(top, "dist", "site")
if not os.path.exists(os.path.join(dist, "arcade-version.txt")):
    sys.exit("manca dist/arcade-version.txt: prima sh m2/build-m2.sh")
version, stamp = open(os.path.join(dist, "arcade-version.txt")).read().split()
# site/ (le pagine HTML del sito) non è nel repository: senza, si preparano solo immagini,
# aggiornamento, Store e build-info.txt, cioè tutto quello che serve a una Release su GitHub
have_site = os.path.isdir(src)
# --solo-sito: rifà solo le pagine di www.mosaikcfw.org (dist/site/web) dalle immagini già pronte in
# dist/site/download, senza toccare aggiornamento, Store e firme già pubblicati
solo = "--solo-sito" in sys.argv[1:]
if solo and not have_site:
    sys.exit("--solo-sito: manca la cartella site/")
if not have_site:
    print("niente cartella site/: salto le pagine del sito (immagini, aggiornamento e Store si fanno lo stesso)")
if not solo:
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "download"))
    subprocess.run([sys.executable, os.path.join(top, "toolchain", "update-server.py"), "--out", os.path.join(out, "update")], check=True)
    # emulatori: pacchetti dello Store e catalogo firmato, accanto ai file dell'aggiornamento
    subprocess.run([sys.executable, os.path.join(top, "toolchain", "make-store.py"), "--out", os.path.join(out, "store")], check=True)


def human(n):
    return f"{n / (1 << 30):.1f} GB".replace(".", ",") if n >= 1 << 30 else f"{n // (1 << 20)} MB"


values = {"VERSION": version, "DATE": f"{stamp[6:8]}/{stamp[4:6]}/{stamp[:4]}", "DATE_ISO": f"{stamp[:4]}-{stamp[4:6]}-{stamp[6:8]}"}
for key, model in (("SMART", "smartpro"), ("BRICK", "brickpro")):
    name = f"mosaik-{version}-{model}.img.gz"
    path = os.path.join(out, "download", name)
    # mtime=0: stesso contenuto, stesso file compresso
    if solo:
        if not os.path.exists(path): sys.exit(f"--solo-sito: manca {path}")
    else:
        with open(os.path.join(dist, f"m2-{model}.img"), "rb") as f, open(path, "wb") as raw, \
                gzip.GzipFile(filename="", fileobj=raw, mode="wb", compresslevel=6, mtime=0) as z:
            shutil.copyfileobj(f, z, 1 << 20)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    values |= {f"{key}_FILE": name, f"{key}_SIZE": human(os.path.getsize(path)), f"{key}_SHA": h.hexdigest()}
    print(name, human(os.path.getsize(path)))


def fill(text, extra):
    for k, v in {**values, **extra}.items():
        text = text.replace("{{" + k + "}}", v)
    return text


def build_pages():
    """Sito di www.mosaikcfw.org in dist/site/web: Home e Scarica in inglese (radice) e italiano
    (it/), le schermate di docs/images, i link di scaricamento alla Release vX.Y.Z su GitHub. Le
    guide restano nella wiki. Da caricare sull'hosting solo per le versioni stabili."""
    web = os.path.join(out, "web")
    shutil.rmtree(web, ignore_errors=True)
    shutil.copytree(os.path.join(src, "assets"), os.path.join(web, "assets"))
    shots = os.path.join(top, "docs", "images")
    os.makedirs(os.path.join(web, "assets", "shots", "en"))
    for n in os.listdir(shots):
        if n.endswith(".png"):
            shutil.copy(os.path.join(shots, n), os.path.join(web, "assets", "shots", "en", n))
    shutil.copytree(os.path.join(shots, "it"), os.path.join(web, "assets", "shots", "it"))
    conf = json.load(open(os.path.join(src, "site.json"), encoding="utf-8"))
    layout = open(os.path.join(src, "layout.html"), encoding="utf-8").read()
    rel = f"https://github.com/diapolon/mosaik_cfw/releases/download/v{version}/"
    for lang, c in conf.items():
        up = "../" if c["prefix"] else ""
        oc = conf[c["other"]]
        shot_lang = lang if os.path.isdir(os.path.join(shots, lang)) else "en"
        for slug in c["pages"]:
            body = open(os.path.join(src, lang, slug + ".html"), encoding="utf-8").read()
            links = ""
            for k, v in c["nav"].items():
                href = k[1:] if k.startswith("@") else k + ".html"
                links += f'<a href="{href}"' + (' aria-current="page"' if k == slug else "") + f">{v}</a>"
            other_href = ("../" if c["prefix"] else oc["prefix"]) + slug + ".html"
            links += f'<a class="lang" href="{other_href}" lang="{c["other"]}" hreflang="{c["other"]}">{c["other_label"]}</a>'
            page_path = c["prefix"] + ("" if slug == "index" else slug + ".html")
            other_path = oc["prefix"] + ("" if slug == "index" else slug + ".html")
            extra = {"LANG": lang, "OTHER_LANG": c["other"], "ASSETS": up + "assets/", "SHOTS": up + f"assets/shots/{shot_lang}/",
                     "HOME": "index.html", "SELF": page_path, "OTHER": other_path, "DESCRIPTION": c["description"],
                     "SMART_URL": rel + values["SMART_FILE"], "BRICK_URL": rel + values["BRICK_FILE"],
                     "PAGE_TITLE": c["site"] if slug == "index" else f'{c["titles"][slug]} · Mosaik'}
            page = layout.replace("{{BODY}}", body).replace("{{FOOTER}}", c["footer"]).replace("{{TOPLINKS}}", links)
            page = fill(page, extra)
            assert "{{" not in page, f"segnaposto non sostituito in {lang}/{slug}"
            path = os.path.join(web, c["prefix"], slug + ".html")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w", encoding="utf-8").write(page)
    print("sito web in", web)


if have_site:
    build_pages()
if solo:
    sys.exit(0)
# --- provenienza della build (audit prelancio R09): commit di Mosaik e dei core, immagine del
# kit, SHA-256 di quello che si distribuisce. Va allegato alla release insieme alle immagini.
def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()
def run(cmd, strip=True):
    try:
        out_ = subprocess.run(cmd, capture_output=True, text=True, check=True, cwd=top).stdout
        return out_.strip() if strip else out_.rstrip("\n")
    except (OSError, subprocess.CalledProcessError):
        return ""
info = [f"Mosaik {version} {stamp}"]
commit = run(["git", "rev-parse", "HEAD"])
changed = [l[3:] for l in run(["git", "status", "--porcelain", "--untracked-files=no"], strip=False).splitlines() if len(l) > 3]
info.append(f"commit {commit}" + (f" (file modificati non nel commit: {', '.join(changed)})" if changed else ""))
kit = run(["docker", "image", "inspect", "trimui-tg5040", "--format", "{{.Id}}"])
info.append(f"kit di compilazione trimui-tg5040 {kit or '?'}")
cores = run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", "trimui-build:/build", "trimui-tg5040", "sh", "-c",
             'for d in /build/src/*/; do [ -d "$d/.git" ] && echo "$(basename $d) $(git -C $d rev-parse HEAD)$(git -C $d diff --quiet || echo " + toolchain/patches")"; done'])
info.append("")
info.append("core (commit compilato, da /build/src):")
info += [f"  {l}" for l in cores.splitlines()] or ["  ?"]
info.append("")
info.append("SHA-256:")
files = [os.path.join(out, "download", n) for n in sorted(os.listdir(os.path.join(out, "download"))) if n.endswith(".img.gz")]
files.append(os.path.join(dist, "arcade-system.squashfs"))
store = os.path.join(out, "store")   # gli emulatori: pacchetti dello Store (toolchain/make-store.py)
files += [os.path.join(store, n) for n in sorted(os.listdir(store)) if n.startswith("store")]
upd = os.path.join(out, "update")
files += [os.path.join(upd, n) for n in ("update.txt", "update.sig")]
for f in files:
    info.append(f"{sha256(f)}  {os.path.relpath(f, dist)}")
n_upd = len([n for n in os.listdir(upd) if n.startswith("file-")])
info.append(f"(più {n_upd} file-N, con il loro SHA-256 nell'elenco firmato update.txt)")
with open(os.path.join(out, "download", "build-info.txt"), "w") as f:
    f.write("\n".join(info) + "\n")
print("build-info.txt:", commit[:12], "e", len(cores.splitlines()), "core")
total = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
biggest = max(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
print(f"sito pronto in {out}: {human(total)}, file più grande {human(biggest)}")
