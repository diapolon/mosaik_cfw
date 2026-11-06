#!/usr/bin/env python3
"""Scarica immagini e informazioni dei giochi arcade presenti sulla SD.

Fonti:
  1. ArcadeDB (adb.arcadeitalia.net): cerca per nome ROM; immagini di gioco, titolo,
     marquee, flyer; genere (in inglese, lo traduce il launcher) e numero di giocatori.
  2. Miniature libretro (thumbnails.libretro.com): cerca per titolo; usata quando
     ArcadeDB non ha l'immagine.

Scarica solo ciò che manca, una richiesta al secondo: sono servizi gratuiti della
comunità. Le immagini più grandi di 640 px vengono ridotte (sips su macOS).

Uso: scrape.py <radice SD> [--types snap,title,marquee] [--limit N] [--delay 1.0]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request

UA = "TrimUI-Arcade-Prototype/0.1"
ADB = "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&use_parent=1&game_name="
ADB_FIELDS = {"snap": "url_image_ingame", "title": "url_image_title",
              "marquee": "url_image_marquee", "flyer": "url_image_flyer"}
LIBRETRO = "https://thumbnails.libretro.com/{repo}/{kind}/{name}.png"
LIBRETRO_KINDS = {"snap": "Named_Snaps", "title": "Named_Titles"}
LIBRETRO_REPOS = {"fbneo": ["FBNeo - Arcade Games", "MAME"], "mame2003p": ["MAME", "FBNeo - Arcade Games"]}
SOURCES = {"fbneo": "FBNEO", "mame2003p": "MAME2003P"}
MAX_SIDE = 640

# versione del formato di Dats/arcadedb.tsv: la v1 conteneva generi già tradotti in italiano,
# la v2 il genere originale di ArcadeDB; con la v3 si salva anche la storia del gioco
# in Dats/history/<rom>.txt. Un formato più vecchio fa riscaricare le informazioni.
META_HEADER = "# arcadedb v3"


class Fetcher:
    def __init__(self, delay):
        self.delay = delay
        self.last = 0.0

    def get(self, url):
        wait = self.last + self.delay - time.time()
        if wait > 0:
            time.sleep(wait)
        self.last = time.time()
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.headers.get("Content-Type", ""), r.read()
        except urllib.error.HTTPError as e:
            return e.code, "", b""
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            print(f"  rete: {e}", file=sys.stderr)
            return 0, "", b""


def load_index(root, core):
    """nome ROM -> titolo, dall'indice creato da build_index.py"""
    titles = {}
    path = os.path.join(root, "Dats", f"{core}.tsv")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                cols = line.rstrip("\n").split("\t")
                if len(cols) >= 7 and cols[6] != "1":
                    titles[cols[0]] = cols[1]
    return titles


def scan_roms(root):
    """[(core, nome ROM, titolo)] per le ROM presenti, come le trova il launcher"""
    games = []
    for core, folder in SOURCES.items():
        titles = load_index(root, core)
        base = os.path.join(root, "Roms", folder)
        for dirpath, _, files in os.walk(base, followlinks=True):
            if dirpath[len(base):].count(os.sep) > 3:
                continue
            for fn in files:
                name, ext = os.path.splitext(fn)
                if ext.lower() not in (".zip", ".7z"):
                    continue
                if titles and name not in titles:
                    continue   # BIOS o file estranei al set
                games.append((core, name, titles.get(name, name)))
    return games


def save_image(data, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    if shutil.which("sips"):
        out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", tmp],
                             capture_output=True, text=True).stdout
        dims = [int(n) for n in re.findall(r"pixel(?:Width|Height): (\d+)", out)]
        if dims and max(dims) > MAX_SIDE:
            subprocess.run(["sips", "-Z", str(MAX_SIDE), "-s", "format", "png", tmp, "--out", tmp],
                           capture_output=True)
    os.replace(tmp, dest)


def libretro_name(title):
    # convenzione delle miniature libretro: questi caratteri diventano "_"
    return re.sub(r'[&*/:`<>?\\|"]', "_", title)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--types", default="snap,title,marquee")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--delay", type=float, default=1.0)
    ap.add_argument("--retry", action="store_true", help="riprova anche le immagini non trovate in passato")
    args = ap.parse_args()
    types = [t for t in args.types.split(",") if t in ADB_FIELDS]
    root = args.root
    fetch = Fetcher(args.delay)

    games = scan_roms(root)
    if args.limit:
        games = games[:args.limit]
    meta_path = os.path.join(root, "Dats", "arcadedb.tsv")
    meta = {}
    if os.path.exists(meta_path):
        with open(meta_path, encoding="utf-8") as f:
            lines = f.read().splitlines()
        if lines and lines[0] == META_HEADER:
            for line in lines[1:]:
                cols = line.split("\t")
                if len(cols) >= 3:
                    meta[cols[0]] = cols[1:]

    # immagini già cercate senza successo: non si richiedono di nuovo, salvo --retry
    nf_path = os.path.join(root, "Media", "non-trovate.txt")
    notfound = set()
    if os.path.exists(nf_path) and not args.retry:
        with open(nf_path, encoding="utf-8") as f:
            notfound = {line.strip() for line in f if line.strip()}

    def missing(name):
        return [t for t in types if f"{t}/{name}" not in notfound
                and not os.path.exists(os.path.join(root, "Media", t, name + ".png"))]

    todo = sorted({name for _, name, _ in games if missing(name) or name not in meta})
    print(f"{len(games)} giochi, {len(todo)} da completare")

    # 1. informazioni ArcadeDB, 20 giochi per richiesta
    adb = {}
    for i in range(0, len(todo), 20):
        chunk = todo[i:i + 20]
        status, _, body = fetch.get(ADB + urllib.parse.quote(";".join(chunk), safe=";"))
        if status != 200:
            print(f"  ArcadeDB non risponde ({status}), continuo con le miniature libretro")
            break
        for r in json.loads(body or b"{}").get("result", []):
            adb[r.get("game_name")] = r
            meta[r["game_name"]] = [(r.get("genre") or "").replace("\t", " "), str(r.get("players", "") or "")]
            history = (r.get("history") or "").replace("\r\n", "\n").strip()
            if history:
                hdir = os.path.join(root, "Dats", "history")
                os.makedirs(hdir, exist_ok=True)
                with open(os.path.join(hdir, r["game_name"] + ".txt"), "w", encoding="utf-8") as hf:
                    hf.write(history + "\n")
        print(f"  informazioni: {min(i + 20, len(todo))}/{len(todo)}")

    with open(meta_path, "w", encoding="utf-8") as f:
        f.write(META_HEADER + "\n")
        for name in sorted(meta):
            f.write("\t".join([name] + meta[name]) + "\n")

    # 2. immagini
    got = failed = 0
    by_name = {}
    for core, name, title in games:
        by_name.setdefault(name, (core, title))
    for n, name in enumerate(todo, 1):
        core, title = by_name[name]
        for t in missing(name):
            dest = os.path.join(root, "Media", t, name + ".png")
            data = None
            url = adb.get(name, {}).get(ADB_FIELDS[t])
            if url:
                status, ctype, body = fetch.get(url)
                if status == 200 and ctype.startswith("image/") and len(body) > 200:
                    data = body
            if data is None and t in LIBRETRO_KINDS:
                for repo in LIBRETRO_REPOS[core]:
                    status, ctype, body = fetch.get(LIBRETRO.format(
                        repo=urllib.parse.quote(repo), kind=LIBRETRO_KINDS[t],
                        name=urllib.parse.quote(libretro_name(title))))
                    if status == 200 and len(body) > 200:
                        data = body
                        break
            if data:
                save_image(data, dest)
                got += 1
            else:
                failed += 1
                notfound.add(f"{t}/{name}")
        if n % 10 == 0 or n == len(todo):
            os.makedirs(os.path.dirname(nf_path), exist_ok=True)
            with open(nf_path, "w", encoding="utf-8") as f:
                f.write("\n".join(sorted(notfound)) + "\n")
            print(f"  immagini: {n}/{len(todo)} giochi, {got} scaricate, {failed} non trovate")
    print("fatto")


if __name__ == "__main__":
    main()
