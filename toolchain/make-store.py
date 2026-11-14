#!/usr/bin/env python3
"""Pacchetti dello Store e catalogo firmato.

    python3 toolchain/make-store.py [--out dist/site/store]

Prende dist/store-src/<pacchetto>/ (preparati da toolchain/package.sh: i file con i percorsi
della scheda sotto Arcade/) e scrive in --out:
  store-<pacchetto>-<versione>.zip   i file del pacchetto più pacchetto.json
  store.json                         il catalogo: per ogni pacchetto nome, console, versione,
                                     misura, impronta SHA-256 e file installati
  store.sig                          la firma del catalogo (stessa chiave degli aggiornamenti)
  store.cat                          firma e catalogo in un file solo: quello che legge la console
La versione è l'impronta del contenuto: cambia solo se cambia un file. L'etichetta mostrata è il
commit del core (o la versione di Deadcast). La console scarica catalogo e firma dallo stesso
indirizzo degli aggiornamenti (canale Stabile o Sviluppo)."""
import argparse
import hashlib
import json, re
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

# pacchetto -> (nome, cartella dei sorgenti in /build/src per il commit, categoria dello Store:
# arcade, console o apps; in futuro linux). I temi non sono qui: una cartella con il solo
# Themes/<tema>/theme.json (e i suoi file) è un pacchetto della cartella Temi (theme_info)
PACKAGES = {
    "fbneo": ("FinalBurn Neo", "fbneo", "arcade"),
    "mame2003p": ("MAME 2003-Plus", "mame2003-plus", "arcade"),
    "mame2010": ("MAME 2010", "mame2010", "arcade"),
    "fceumm": ("FCEUmm", "fceumm", "console"),
    "genesis_plus_gx": ("Genesis Plus GX", "genesis_plus_gx", "console"),
    "snes9x": ("Snes9x", "snes9x", "console"),
    "gambatte": ("Gambatte", "gambatte", "console"),
    "mgba": ("mGBA", "mgba", "console"),
    "pce_fast": ("Beetle PCE Fast", "pce_fast", "console"),
    "ngp": ("Beetle NeoPop", "ngp", "console"),
    "pcsx": ("PCSX ReARMed", "pcsx", "console"),
    "deadcast": ("Deadcast", None, "console"),
    "stella2014": ("Stella 2014", "stella2014", "console"),
    "prosystem": ("ProSystem", "prosystem", "console"),
    "gearcoleco": ("Gearcoleco", "gearcoleco", "console"),
    "freeintv": ("FreeIntv", "freeintv", "console"),
    "handy": ("Handy", "handy", "console"),
    "wswan": ("Beetle WonderSwan", "wswan", "console"),
    "pokemini": ("PokeMini", "pokemini", "console"),
    "vb": ("Beetle VB", "vb", "console"),
    "scummvm": ("ScummVM", "scummvm", "computer"),
    "fuse": ("Fuse", "fuse", "computer"),
    "bluemsx": ("blueMSX", "bluemsx", "computer"),
    "vice": ("VICE", "vice", "computer"),
    "dosbox_pure": ("DOSBox Pure", "dosbox_pure", "computer"),
    "puae": ("PUAE", "puae", "computer"),   # cartella Computer (i sistemi fino alla 0.9.36 la mettono in Console)
    "ppsspp": ("PPSSPP", "ppsspp", "console"),
    "mupen64plus": ("Mupen64Plus", None, "console"),   # etichetta: commit del core in deps/mupen64plus
    "files": ("Files", None, "apps"),   # app: nome, versione e traduzioni da Apps/<id>/app.json
    "viewer": ("Images and comics", None, "apps"),
    "music": ("Music player", None, "apps"),
    "sdtest": ("SD card test", None, "apps"),
    "scraper": ("Download images", None, "apps"),
    "sdgrow": ("SD card space", None, "apps"),
    "diag": ("Diagnostics", None, "apps"),
    "video": ("Video", None, "apps"),   # porta in Apps/video/lib le librerie LGPL di FFmpeg
    # giochi Linux (0.9.58): nome, versione, colore e testi da frontend/package/linux/<id>/pack.json
    "freedoom": ("Freedoom", None, "linux"),
    "mrboom": ("Mr.Boom", None, "linux"),
    "devilutionx": ("Diablo", None, "linux"),
}
# giochi Linux tenuti fuori dal repository (frontend/package/linux-diapolon, solo locale: vedi
# toolchain/package.sh): ogni cartella con pack.json è un pacchetto della cartella Giochi Linux
EXTRA_GAMES = os.path.join(top, "frontend", "package", "linux-diapolon")
for _id in sorted(os.listdir(EXTRA_GAMES)) if os.path.isdir(EXTRA_GAMES) else []:
    if os.path.exists(os.path.join(EXTRA_GAMES, _id, "pack.json")):
        PACKAGES.setdefault(_id, (_id, None, "linux"))
# cartelle in cui un pacchetto può scrivere (la console controlla le stesse)
ALLOWED = ("Cores/", "Emulators/", "Dats/", "Apps/", "Themes/", "Filters/", "Bezels/", "Linux/")
# Linux/ solo per i giochi Linux, che vanno nell'elenco "packages2" del catalogo: i sistemi fino
# alla 0.9.57 leggono solo "packages" e un percorso che non conoscono renderebbe falso tutto il
# catalogo; "packages2" lo leggono dalla 0.9.58
NEW_PATHS = ("Linux/",)


def theme_info(root):
    """(nome, versione, colori, testi) di un pacchetto con un solo tema, altrimenti None"""
    td = os.path.join(root, "Themes")
    if sorted(os.listdir(root)) != ["Themes"] or not os.path.isdir(td):
        return None
    ids = [d for d in os.listdir(td) if not d.startswith((".", "_"))]
    if len(ids) != 1 or not os.path.exists(os.path.join(td, ids[0], "theme.json")):
        return None
    text = re.sub(r"^\s*//.*$", "", open(os.path.join(td, ids[0], "theme.json"), encoding="utf-8").read(), flags=re.M)
    tj = json.loads(text)
    accents = [v.get("accent") for v in (tj.get("variants") or {}).values() if isinstance(v, dict) and v.get("accent")]
    if not accents and tj.get("colors", {}).get("accent"):
        accents = [tj["colors"]["accent"]]
    return tj.get("name", ids[0]), str(tj.get("version", "1.0")), accents[:3], tj.get("texts")


def bezel_info(pid, root):
    """(nome, versione, colori, testi, console) di un pacchetto Cornici, altrimenti None. Le
    cornici sono in frontend/package/bezels/<serie>/Bezels (tools/make_bezels.py), la descrizione
    in pack.json accanto; nel pacchetto ci sono solo i file Bezels/<console>/_mosaik*.png, che
    il runner usa dopo quelli dell'utente. Una console sta in una serie sola: due serie con lo
    stesso file si toglierebbero a vicenda la cornice."""
    if not pid.startswith("cornici-") or sorted(os.listdir(root)) != ["Bezels"]:
        return None
    pj = os.path.join(top, "frontend", "package", "bezels", pid[8:], "pack.json")
    if not os.path.exists(pj):
        return None
    meta = json.load(open(pj, encoding="utf-8"))
    texts = meta.get("texts") or {}
    name = (texts.get("en") or {}).get("title", pid)
    consoles = []
    for sid in sorted(os.listdir(os.path.join(root, "Bezels"))):
        ej = os.path.join(top, "frontend", "emulators", sid + ".json")
        em = json.load(open(ej)) if os.path.exists(ej) else {}
        consoles.append((em.get("order", 100), em.get("short") or em.get("name") or sid))
    return name, str(meta.get("version", "1.0")), [meta["color"]] if meta.get("color") else [], texts, [c[1] for c in sorted(consoles)]


def filter_info(pid, root):
    """(nome, versione, colori, testi, filtri) di un pacchetto Filtri, altrimenti None. I filtri
    sono in frontend/package/filters/<serie>/Filters/<id>, la descrizione in pack.json accanto.
    Nel sistema restano i filtri semplici (frontend/filters); qui gli shader GLSL e quelli nuovi."""
    if not pid.startswith("filtri-") or sorted(os.listdir(root)) != ["Filters"]:
        return None
    pj = os.path.join(top, "frontend", "package", "filters", pid[7:], "pack.json")
    if not os.path.exists(pj):
        return None
    meta = json.load(open(pj, encoding="utf-8"))
    texts = meta.get("texts") or {}
    name = (texts.get("en") or {}).get("title", pid)
    names = []
    for fid in sorted(os.listdir(os.path.join(root, "Filters"))):
        fj = json.load(open(os.path.join(root, "Filters", fid, "filter.json"), encoding="utf-8"))
        n = fj.get("name", fid)
        names.append((fj.get("order", 100), (n.get("en") if isinstance(n, dict) else n) or fid))
    return name, str(meta.get("version", "1.0")), [meta["color"]] if meta.get("color") else [], texts, [x[1] for x in sorted(names)]


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
# stato dei pacchetti (sperimentale, provato su quali modelli, sistema minimo): un file solo, perché
# il catalogo è lo stesso per i due canali del sistema. STORE_STATO lo cambia nelle prove
stato_file = os.environ.get("STORE_STATO") or os.path.join(top, "frontend", "package", "store-stato.json")
stato = json.load(open(stato_file, encoding="utf-8")) if os.path.exists(stato_file) else {}
# lo stato dei giochi fuori dal repository sta accanto a loro
_extra = os.path.join(EXTRA_GAMES, "store-stato.json")
if os.path.exists(_extra) and not os.environ.get("STORE_STATO"):
    _x = json.load(open(_extra, encoding="utf-8"))
    stato.setdefault("experimental", []).extend(_x.get("experimental", []))
    stato.setdefault("min_system", {}).update(_x.get("min_system", {}))
catalog = {"format": 1, "packages": [], "packages2": []}
for pid in sorted(os.listdir(args.src)):
    root = os.path.join(args.src, pid)
    theme = theme_info(root) if pid not in PACKAGES and os.path.isdir(root) else None
    bezel = bezel_info(pid, root) if pid not in PACKAGES and not theme and os.path.isdir(root) else None
    if not bezel and pid not in PACKAGES and not theme and os.path.isdir(root):
        bezel = filter_info(pid, root)   # i filtri si trattano come le cornici: serie con pack.json
        is_filter = bezel is not None
    else:
        is_filter = False
    if (pid not in PACKAGES and not theme and not bezel) or not os.path.isdir(root):
        sys.exit(f"pacchetto sconosciuto: {pid}")
    if pid in PACKAGES:
        name, srcdir, category = PACKAGES[pid]
    else:
        name, srcdir, category = (theme or bezel)[0], None, "themes" if theme else "filters" if is_filter else "bezels"
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
    emulators, systems, ems = [], [], []
    for rel in files:
        if rel.startswith("Emulators/") and rel.endswith(".json"):
            emulators.append(rel[10:-5])
            ems.append(json.load(open(os.path.join(root, rel))))
            systems.append(ems[-1].get("name", rel[10:-5]))
    # console nell'ordine delle piattaforme; titolo breve per il riquadro dello Store: il nome
    # della console se è una sola, altrimenti le sigle
    order_ix = sorted(range(len(ems)), key=lambda k: ems[k].get("order", 100))
    systems = [systems[k] for k in order_ix]
    title = systems[0] if len(ems) == 1 else " · ".join(ems[k].get("short") or ems[k].get("name", "") for k in order_ix)
    title = {"genesis_plus_gx": "Mega Drive · SMS · GG"}.get(pid, title)   # le sigle intere non stanno nel riquadro
    # l'immagine del pacchetto nello Store: quella della sua prima console (la più vecchia)
    first = order_ix[0] if ems else None
    art = {"emu": emulators[first], "art": ems[first].get("art", ""), "short": ems[first].get("short", ""),
           "console": ems[first].get("kind") == "console"} if first is not None else {}
    texts = None
    linuxj = os.path.join(top, "frontend", "package", "linux", pid, "pack.json")
    if not os.path.exists(linuxj):
        linuxj = os.path.join(EXTRA_GAMES, pid, "pack.json")
    lmeta = json.load(open(linuxj, encoding="utf-8")) if category == "linux" and os.path.exists(linuxj) else None
    if lmeta:
        texts = lmeta.get("texts")
        name = (texts or {}).get("en", {}).get("title", name)
    appj = os.path.join(root, "Apps", pid, "app.json")
    if category == "apps" and os.path.exists(appj):
        aj = json.load(open(appj))
        name = aj.get("title", name)
        texts = aj.get("texts")
    if bezel:   # le console delle cornici, per la scheda del pacchetto (i sistemi fino alla 0.9.36 le mettono in Console)
        systems = bezel[4]
    if lmeta and lmeta.get("version") == "date":   # giochi in sviluppo: "version" del loro game.json, altrimenti la data
        gj = [r for r in files if r.count("/") == 2 and r.endswith("/game.json")]
        gv = str(json.load(open(os.path.join(root, gj[0]), encoding="utf-8")).get("version", "")) if gj else ""
        if gv:
            label = gv
        else:
            import datetime
            newest = max(os.path.getmtime(os.path.join(root, r)) for r in files)
            label = datetime.date.fromtimestamp(newest).strftime("%Y.%m.%d")
    elif lmeta:
        label = str(lmeta.get("version", "1.0"))
    elif theme or bezel:
        label, texts = (theme or bezel)[1], (theme or bezel)[3]
    elif category == "apps":
        label = json.load(open(appj)).get("version", "?") if os.path.exists(appj) else "?"
    elif pid == "mupen64plus":
        label = subprocess.run(["git", "-C", os.path.join(top, "deps", "mupen64plus", "mupen64plus-core"), "rev-parse", "--short=7", "HEAD"],
                               capture_output=True, text=True).stdout.strip() or "?"
    elif pid == "deadcast":
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
    entry = {
        "id": pid, "name": name, "label": label, "version": version, "category": category, "title": title if ems else name, "systems": systems, "emulators": emulators, **art,
        "file": os.path.basename(zpath), "size": os.path.getsize(zpath), "installed": installed,
        "sha256": sha256(zpath), "files": files,
    }
    if pid in stato.get("experimental", []):
        entry["status"] = "experimental"
    if stato.get("tested", {}).get(pid):
        entry["tested"] = stato["tested"][pid]
    # sistema di cui il pacchetto ha bisogno: app e giochi Linux lo dichiarano nei loro file
    # (app.json, game.json di ogni gioco: lo legge anche il launcher prima di avviarli), gli altri
    # pacchetti in store-stato.json; per app e giochi è obbligatorio
    def vkey(v):
        return [int(x) if x.isdigit() else 0 for x in str(v).split(".")]
    mins = []
    if category == "apps" and os.path.exists(appj):
        mins.append(json.load(open(appj, encoding="utf-8")).get("min_system", ""))
    if category == "linux":
        for r in files:
            if r.count("/") == 2 and r.endswith("/game.json"):
                mins.append(json.load(open(os.path.join(root, r), encoding="utf-8")).get("min_system", ""))
    mins = [m for m in mins if m]
    min_sys = max(mins, key=vkey) if mins else stato.get("min_system", {}).get(pid, "")
    if category in ("apps", "linux") and not min_sys:
        sys.exit(f"{pid}: manca \"min_system\" (app.json delle app, game.json dei giochi Linux)")
    if min_sys:
        entry["min_system"] = min_sys
    if texts:   # nomi e descrizioni delle app nelle lingue della console
        entry["texts"] = texts
    if theme and theme[2]:   # temi: i colori delle varianti, per il riquadro
        entry["colors"] = theme[2]
    if bezel and bezel[2]:   # cornici: il colore del riquadro
        entry["colors"] = bezel[2]
    if lmeta and lmeta.get("color"):
        entry["colors"] = [lmeta["color"]]
    new = any(rel.startswith(NEW_PATHS) for rel in files)
    if new and not min_sys:
        sys.exit(f"{pid}: i file in {NEW_PATHS} richiedono un min_system")
    catalog["packages2" if new else "packages"].append(entry)
    print(f"  {pid:16} {label:10} {os.path.getsize(zpath) / 1e6:6.1f} MB  {', '.join(systems)}")

# Due pacchetti non devono scrivere lo stesso file: togliendone uno sparirebbe anche dall'altro
# (audit 0.9.38). Confronto senza maiuscole, come sul FAT della scheda
owner = {}
for pk in catalog["packages"] + catalog["packages2"]:
    for rel in pk["files"]:
        k = rel.lower()
        if k in owner and owner[k] != pk["id"]:
            sys.exit(f"errore: {rel} sta sia in {owner[k]} sia in {pk['id']}")
        owner[k] = pk["id"]

# Ordine nello Store, che segue quello del catalogo anche sui sistemi vecchi. Arcade nell'ordine
# delle piattaforme (campo "order"); console e computer per marca, in ordine alfabetico, poi per
# anno di uscita: un pacchetto con più console prende la più vecchia. Marca e anno sono quelli della
# tabella "known" di frontend/src/launcher/games.c (o "maker" e "year" nel JSON dell'emulatore);
# ScummVM, che non è una marca, va in fondo ai computer. Il resto (app, temi...) per nome.
known = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(
    r'\{ "([a-z0-9_]+)", "([^"]*)", "([0-9]*)" \}', open(os.path.join(top, "frontend", "src", "launcher", "games.c"), encoding="utf-8").read())}
def emu_json(pk, e):
    return json.load(open(os.path.join(args.src, pk["id"], "Emulators", e + ".json")))
def order(pk):
    return min((emu_json(pk, e).get("order", 100) for e in pk["emulators"]), default=100)
def maker_year(pk):
    best = None
    for e in pk["emulators"]:
        j = emu_json(pk, e)
        mk, yr = known.get(e, ("", ""))
        mk, yr = j.get("maker", mk), str(j.get("year", yr))
        k = (int(yr) if yr.isdigit() else 9999, mk)
        best = k if best is None or k < best else best
    return best or (9999, "")
def sort_key(pk):
    if pk["category"] in ("console", "computer"):
        year, mk = maker_year(pk)
        return (500 if pk["category"] == "console" else 600, mk == "ScummVM", mk.lower(), year, pk["name"].lower())
    return (order(pk), False, "", 0, pk["name"].lower())   # le altre cartelle prima, come sempre
catalog["packages"].sort(key=sort_key)
catalog["packages2"].sort(key=lambda pk: pk["name"].lower())
text = (json.dumps(catalog, ensure_ascii=False, indent=1) + "\n").encode()
with tempfile.NamedTemporaryFile() as t:
    t.write(text)
    t.flush()
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", args.key, t.name], check=True, capture_output=True).stdout
open(os.path.join(args.out, "store.json"), "wb").write(text)
open(os.path.join(args.out, "store.sig"), "wb").write(sig)
# catalogo e firma in un file solo, per i sistemi dalla 0.9.46: sostituendolo non possono mai
# arrivare spaiati (store.json e store.sig restano per i sistemi di prima e per i pacchetti copiati
# sulla scheda)
open(os.path.join(args.out, "store.cat"), "wb").write(b"MOSAIK-STORE 1 %d\n" % len(sig) + sig + text)
print(f"Store: {len(catalog['packages']) + len(catalog['packages2'])} pacchetti in {args.out}")
