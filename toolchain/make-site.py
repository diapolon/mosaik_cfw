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


values = {"VERSION": version, "DATE": f"{stamp[6:8]}/{stamp[4:6]}/{stamp[:4]}", "DATE_ISO": f"{stamp[:4]}-{stamp[4:6]}-{stamp[6:8]}"}
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


def fill(text, extra):
    for k, v in {**values, **extra}.items():
        text = text.replace("{{" + k + "}}", v)
    return text


conf = json.load(open(os.path.join(src, "site.json"), encoding="utf-8"))
layout = open(os.path.join(src, "layout.html"), encoding="utf-8").read()
for lang, c in conf.items():
    up = "../" if c["prefix"] else ""
    guides = list(c["guides"])
    paths = {"LANG": lang, "ASSETS": up + "assets/", "SHOTS": up + c["shots"], "DL": up + "download/", "DESCRIPTION": c["description"]}


    def read(slug):
        return open(os.path.join(src, lang, slug + ".html"), encoding="utf-8").read()


    def write(slug, title, body, section):
        other = conf[c["other"]]["prefix"]
        other_href = ("../" if c["prefix"] else other) + slug + ".html"
        links = "".join(f'<a href="{k}.html"' + (' aria-current="page"' if k == section else "") + f">{v}</a>" for k, v in c["nav"].items())
        links += f'<a class="lang" href="{other_href}" lang="{c["other"]}" hreflang="{c["other"]}">{c["other_label"]}</a>'
        full_title = c["site"] if slug == "index" else f"{title} · Mosaik"
        page = layout.replace("{{BODY}}", body).replace("{{FOOTER}}", c["footer"]).replace("{{TOPLINKS}}", links)
        page = fill(page.replace("{{PAGE_TITLE}}", full_title), paths)
        assert "{{" not in page, f"segnaposto non sostituito in {lang}/{slug}"
        path = os.path.join(out, c["prefix"], slug + ".html")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8").write(page)


    write("index", c["nav"]["index"], read("index"), "index")
    write("download", c["nav"]["download"], f'<main><h1>{c["nav"]["download"]}</h1>' + read("download") + "</main>", "download")
    write("licenses", c["licenses_title"], f'<main><h1>{c["licenses_title"]}</h1>' + read("licenses") + "</main>", "")
    cards = "".join(f'<a class="card link" href="{g}.html"><h3>{c["guides"][g][0]}</h3><p>{c["guides"][g][1]}</p></a>' for g in guides)
    write("guide", c["nav"]["guide"], f'<main><h1>{c["nav"]["guide"]}</h1><p class="lead">{c["guide_lead"]}</p><div class="cards">{cards}</div></main>', "guide")
    for n, g in enumerate(guides):
        side = "".join(f'<li><a href="{x}.html"' + (' aria-current="page"' if x == g else "") + f'>{c["guides"][x][0]}</a></li>' for x in guides)
        pager = ""
        if n > 0:
            pager += f'<a class="prev" href="{guides[n - 1]}.html"><small>{c["prev"]}</small>{c["guides"][guides[n - 1]][0]}</a>'
        if n + 1 < len(guides):
            pager += f'<a class="next" href="{guides[n + 1]}.html"><small>{c["next"]}</small>{c["guides"][guides[n + 1]][0]}</a>'
        body = f'<div class="doc"><aside class="side"><ul>{side}</ul></aside><main class="article"><h1>{c["guides"][g][0]}</h1>' + read(g) + f'<nav class="pager">{pager}</nav></main></div>'
        write(g, c["guides"][g][0], body, "guide")
total = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
biggest = max(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
print(f"sito pronto in {out}: {human(total)}, file più grande {human(biggest)}")
