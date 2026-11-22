#!/usr/bin/env python3
"""Schermate del README, della Wiki e del sito (docs/images e docs/images/it), sul Mac.

    python3 toolchain/schermate.py             tutte, in inglese e in italiano
    python3 toolchain/schermate.py home store  solo quelle

Usa build/launcher e build/runner di frontend (make sul Mac) e una SD dimostrativa creata da capo in
una cartella temporanea: giochi inventati con copertine in pixel art disegnate qui (cielo, sole a
cerchi, profilo di città; il colore viene dal titolo), indici, genere e storia inventati, emulatori,
temi e catalogo dello Store presi dalla SD simulata (frontend/sdcard, setup-mac.sh). Il gioco
dietro il menu è un core libretro dimostrativo (toolchain/schermate-core.c), che disegna una scena
sua. Niente ROM, nomi o dati veri. Misura della Brick Pro (1024x768), tema Tessera chiaro, batteria
all'80% (TRIMUI_FAKE_BATTERY)."""
import colorsys, hashlib, json, os, random, shutil, subprocess, sys, tempfile, time
from PIL import Image, ImageDraw

W, H, S = 160, 120, 2


def cover(title):
    h = int(hashlib.sha1(title.encode()).hexdigest(), 16)
    rnd = random.Random(h)
    hue = (h % 360) / 360
    sky_top = colorsys.hsv_to_rgb((hue + 0.62) % 1, 0.65, 0.22)
    sky_low = colorsys.hsv_to_rgb((hue + 0.55) % 1, 0.45, 0.45)
    sun = colorsys.hsv_to_rgb(hue, 0.75, 1.0)
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):   # sfumatura a bande, come le palette a 16 colori
        t = round(y / H * 8) / 8
        c = tuple(int(255 * (a + (b - a) * t)) for a, b in zip(sky_top, sky_low))
        d.line([(0, y), (W, y)], fill=c)
    for _ in range(40):
        x, y = rnd.randrange(W), rnd.randrange(H * 2 // 3)
        d.point((x, y), fill=(230, 230, 255))
    cx, cy, r = rnd.randrange(40, 120), rnd.randrange(30, 55), rnd.randrange(22, 32)
    for k in range(5, 0, -1):   # sole a cerchi concentrici
        rr = r * k // 5
        f = 1 - (k - 1) / 6
        c = tuple(int(255 * (s * f + (1 - f)) ) for s in sun)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=c)
    far = tuple(int(255 * v * 0.55) for v in sky_low)
    near = tuple(int(255 * v * 0.25) for v in sky_top)
    for col, base, var in ((far, 78, 10), (near, 92, 14)):   # due file di palazzi
        x = 0
        while x < W:
            w = rnd.randrange(6, 16)
            top = base - rnd.randrange(0, var)
            d.rectangle([x, top, x + w, H], fill=col)
            x += w
    return im.resize((W * S, H * S), Image.NEAREST)



TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FE = os.path.join(TOP, "frontend")

# giochi inventati: emulatore, cartella, estensione, [(file, titolo, anno, autore, genere, giocatori)]
GAMES = [
    ("fbneo", "FBNEO", "zip", [("castellobit", "Castello Bit", 1986, "Studio Demo", "Platform", 2),
                               ("cavblu", "Cavalieri Blu", 1989, "Pixel Forge", "Beat'em Up", 2),
                               ("cuorineon", "Cuori di Neon", 1991, "Studio Demo", "Puzzle", 1),
                               ("dragobianco", "Drago Bianco", 1987, "Lumen", "Shooter", 2),
                               ("dragovetro", "Drago di Vetro", 1993, "Lumen", "Shooter", 2),
                               ("fiamma88", "Fiamma 88", 1988, "Pixel Forge", "Racing", 1),
                               ("gattovol", "Gatto Volante", 1990, "Studio Demo", "Platform", 1),
                               ("giungla3k", "Giungla 3000", 1992, "Lumen", "Run & Gun", 2)]),
    ("mame2003p", "MAME2003P", "zip", [("orbita0", "Orbita Zero", 1982, "Astra", "Shooter", 1),
                                       ("pianrosso", "Pianeta Rosso", 1984, "Astra", "Shooter", 2),
                                       ("razzomatto", "Razzo Matto", 1983, "Lumen", "Maze", 1),
                                       ("robocorsa", "Robo Corsa", 1985, "Pixel Forge", "Racing", 2)]),
    ("nes", "NES", "nes", [("Stella Cadente", "Stella Cadente", 1987, "Lumen", "Action", 1),
                           ("Torre del Vento", "Torre del Vento", 1989, "Studio Demo", "Puzzle", 1)]),
    ("snes", "SNES", "sfc", [("Isola Pixel", "Isola Pixel", 1993, "Pixel Forge", "Adventure", 1),
                             ("Luna Park", "Luna Park", 1994, "Studio Demo", "Party", 4),
                             ("Mare di Nebbia", "Mare di Nebbia", 1995, "Lumen", "RPG", 1)]),
    ("md", "MD", "md", [("Turbo Volpe", "Turbo Volpe", 1992, "Astra", "Platform", 1),
                        ("Vela Rossa", "Vela Rossa", 1993, "Lumen", "Racing", 2),
                        ("Zona Laser", "Zona Laser", 1991, "Astra", "Shooter", 2)]),
    ("gb", "GB", "gb", [("Bosco Quieto", "Bosco Quieto", 1990, "Studio Demo", "Puzzle", 1),
                        ("Topo Postino", "Topo Postino", 1991, "Pixel Forge", "Action", 1)]),
    ("gba", "GBA", "gba", [("Ninja di Carta", "Ninja di Carta", 2002, "Lumen", "Action", 1),
                           ("Sogno d'Estate", "Sogno d'Estate", 2003, "Studio Demo", "RPG", 1),
                           ("Valle dei Draghi", "Valle dei Draghi", 2004, "Astra", "Adventure", 2)]),
]
HISTORY = {
    "it": "Questo gioco è inventato per queste schermate: sulla tua scheda, qui c'è la storia del gioco scaricata da internet.\n\n"
          "Attraversa le dodici stanze di Castello Bit, raccogli le chiavi d'oro e sveglia il re prima che finisca il tempo. Uno o due giocatori, a turno.",
    "en": "This game is made up for these screenshots: on your card, this space shows the information downloaded from the Internet.\n\n"
          "Cross the twelve rooms of Castello Bit, collect the golden keys and wake the king before time runs out. One player, or two taking turns.",
}


STORE_URL = "https://github.com/diapolon/mosaik_cfw/releases/download/store/"


def store_records(d):
    """catalogo vero dello Store e registri dei pacchetti che la SD ha già (emulatori, temi, app), come
    se li avesse installati lo Store; MAME 2003-Plus con una versione vecchia, per l'icona Aggiorna"""
    import urllib.request
    for f in ("store.json", "store.sig"):
        with urllib.request.urlopen(STORE_URL + f, timeout=30) as r:
            open(os.path.join(d, "System", f), "wb").write(r.read())
    cat = json.load(open(os.path.join(d, "System", "store.json")))
    os.makedirs(os.path.join(d, "System", "pacchetti"), exist_ok=True)
    for p in cat["packages"]:
        if p.get("models") and "brickpro" not in p["models"]:
            continue
        emus = p.get("emulators") or []
        if emus:
            have = all(os.path.exists(os.path.join(d, "Emulators", e + ".json")) for e in emus)
        elif p.get("category") == "themes":
            have = os.path.isdir(os.path.join(d, "Themes", p["id"].replace("tema-", "")))
        elif p.get("category") == "apps":
            have = os.path.isdir(os.path.join(d, "Apps", p["id"]))
        else:
            have = False
        if not have:
            continue
        ver = "0" if p["id"] == "mame2003p" else p["version"]
        rec = f"version {ver}\nlabel {p.get('label', '')}\n" + "".join(f"file {f}\n" for f in p.get("files", []))
        open(os.path.join(d, "System", "pacchetti", p["id"] + ".txt"), "w").write(rec)


def build_sd(d, lang):
    shutil.rmtree(d, ignore_errors=True)
    for sub in ("System/collections", "Logs", "Roms", "Saves", "Media", "Apps", "Linux", "Music", "Video", "Dats/history"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    sd = os.path.join(FE, "sdcard")
    for sub in ("Emulators", "Cores", "Themes", "Bezels"):
        if os.path.isdir(os.path.join(sd, sub)):
            shutil.copytree(os.path.join(sd, sub), os.path.join(d, sub), dirs_exist_ok=True)
    for f in os.listdir(os.path.join(d, "Emulators")):   # i JSON del repository: quelli della SD simulata possono essere vecchi
        if os.path.exists(os.path.join(FE, "emulators", f)):
            shutil.copy(os.path.join(FE, "emulators", f), os.path.join(d, "Emulators", f))
    shutil.copytree(os.path.join(FE, "themes"), os.path.join(d, "Themes"), dirs_exist_ok=True)
    shutil.copytree(os.path.join(FE, "filters"), os.path.join(d, "Filters"), dirs_exist_ok=True)
    shutil.copytree(os.path.join(FE, "lang"), os.path.join(d, "Lang"), dirs_exist_ok=True)
    store_records(d)
    for a in ("files", "music", "video", "viewer", "scraper", "sdtest"):   # app: solo app.json, icona, launch.sh
        s = os.path.join(FE, "package", "apps", a)
        if not os.path.isdir(s):
            continue
        os.makedirs(os.path.join(d, "Apps", a))
        for f in ("app.json", "icon.png"):
            if os.path.exists(os.path.join(s, f)):
                shutil.copy(os.path.join(s, f), os.path.join(d, "Apps", a, f))
        open(os.path.join(d, "Apps", a, "launch.sh"), "w").write("#!/bin/sh\n")
    os.makedirs(os.path.join(d, "Linux", "NeonRun"))
    open(os.path.join(d, "Linux", "NeonRun", "launch.sh"), "w").write("#!/bin/sh\n")
    json.dump({"title": "Neon Run", "year": "2024", "author": "Mosaik"}, open(os.path.join(d, "Linux", "NeonRun", "game.json"), "w"))
    meta = ["# arcadedb v3"]
    for emu, folder, ext, games in GAMES:
        os.makedirs(os.path.join(d, "Roms", folder), exist_ok=True)
        os.makedirs(os.path.join(d, "Media", emu), exist_ok=True)
        idx = []
        for f, title, year, maker, genre, players in games:
            open(os.path.join(d, "Roms", folder, f"{f}.{ext}"), "w").write("demo")
            cover(title).save(os.path.join(d, "Media", emu, f + ".png"))
            idx.append(f"{f}\t{title}\t{year}\t{maker}\t{maker}\t0\t0\t")
            meta.append(f"{f if ext == 'zip' else emu + '/' + f}\t{genre}\t{players}")
        open(os.path.join(d, "Dats", emu + ".tsv"), "w").write("\n".join(idx) + "\n")
    open(os.path.join(d, "Dats", "arcadedb.tsv"), "w").write("\n".join(meta) + "\n")
    open(os.path.join(d, "Dats", "history", "castellobit.txt"), "w").write(HISTORY.get(lang, HISTORY["en"]) + "\n")
    open(os.path.join(d, "System", "favorites.txt"), "w").write("fbneo\tgattovol\nmame2003p\torbita0\nsnes\tLuna Park\n")
    open(os.path.join(d, "System", "recent.txt"), "w").write("fbneo\tcastellobit\nfbneo\tgattovol\nmame2003p\torbita0\nnes\tStella Cadente\nsnes\tLuna Park\n")
    now = int(time.time())
    stats = [("fbneo", "castellobit", 7, 5400, now - 600), ("fbneo", "gattovol", 5, 3600, now - 3600),
             ("mame2003p", "orbita0", 3, 1500, now - 7200), ("fceumm", "Stella Cadente", 2, 900, now - 86400),
             ("snes9x", "Luna Park", 7, 5400, now - 172800)]
    open(os.path.join(d, "System", "stats.tsv"), "w").write("".join(f"{c}\t{n}\t{p}\t{s}\t{t}\n" for c, n, p, s, t in stats))
    json.dump({"language": lang, "setup_done": True, "device": "brickpro", "theme": "tessera", "variant": "light", "menu_music": False},
              open(os.path.join(d, "System", "settings.json"), "w"))


STORE = "wait:60,down,wait:5,right,right,right,wait:5,a,wait:120"
LIB = "wait:60,down,wait:5,a,wait:30,a,wait:40"
MENU = "wait:120,menu,wait:30"
# nome: (programma, passi di --auto prima dello scatto, impostazioni diverse da quelle di partenza)
SHOTS = {
    "home": ("launcher", "wait:60", {}),
    "platforms": ("launcher", "wait:60,down,wait:5,a,wait:30,right,wait:5,right,wait:30", {}),
    "library": ("launcher", LIB + ",right,wait:5,right,wait:20", {}),
    "switch-game": ("launcher", "wait:60,select,wait:40", {}),
    "quick-panel": ("launcher", "wait:60,menu,wait:40", {}),
    "details": ("launcher", LIB + ",right,wait:5,right,wait:5,x,wait:40", {}),
    "game-page": ("launcher", LIB + ",right,wait:5,right,wait:5,right,wait:5,right,wait:5,right,wait:5,right,wait:5,right,wait:5,right,wait:5,x,wait:40", {}),
    "game-options": ("launcher", LIB + ",right,wait:5,right,wait:5,x,wait:40,x,wait:30", {}),
    "filters": ("launcher", LIB + ",select,wait:30", {}),
    "settings": ("launcher", "wait:60,start,wait:40", {}),
    "appearance": ("launcher", "wait:60,start,wait:30,right,wait:5,a,wait:30", {}),
    "guide": ("launcher", "wait:60,down,wait:5,a,wait:30,right,wait:5,right,wait:5,select,wait:30", {}),
    "theme-cabinato": ("launcher", "wait:60", {"theme": "cabinato", "variant": "neon"}),
    "theme-onda": ("launcher", "wait:60,right,wait:5,a,wait:40,a,wait:40,down,wait:5,down,wait:30", {"theme": "onda", "variant": "oceano"}),
    "theme-copertina": ("launcher", "wait:60", {"theme": "copertina", "variant": "fumetto"}),
    "theme-stellato": ("launcher", "wait:60", {"theme": "stellato", "variant": "notte"}),
    "transfer": ("launcher", "wait:60,start,wait:30,down,down,down,wait:5,a,wait:20,a,wait:60", {}),
    "store-folders": ("launcher", STORE, {}),
    "store": ("launcher", STORE + ",right,wait:5,a,wait:40", {}),
    "store-page": ("launcher", STORE + ",down,wait:5,right,wait:5,a,wait:30,a,wait:60,r,wait:240", {}),
    "store-delete": ("launcher", STORE + ",right,wait:5,a,wait:30,down,wait:5,right,right,right,wait:5,a,wait:60,a,wait:40", {}),
    "store-themes": ("launcher", STORE + ",down,wait:5,right,wait:5,a,wait:40", {}),
    "store-bezels": ("launcher", STORE + ",down,wait:5,right,right,wait:5,a,wait:40", {}),
    "ingame-menu": ("runner", MENU, {}),
    "ingame-saves": ("runner", MENU + ",down,down,a,wait:90,down,down,a,wait:40", {}),
    "ingame-settings": ("runner", MENU + ",up,wait:30", {}),
    "ingame-controls": ("runner", MENU + ",up,up,up,up,up,wait:10,a,wait:30", {}),
    "ingame-shortcuts": ("runner", MENU + ",up,up,up,up,wait:10,a,wait:30", {}),
}


def shoot(names):
    tmp = tempfile.mkdtemp(prefix="mosaik-schermate-")
    core = os.path.join(tmp, "demo_libretro.dylib")
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-o", core, os.path.join(TOP, "toolchain", "schermate-core.c"),
                    "-I", os.path.join(FE, "src", "runner")], check=True)
    env = dict(os.environ, TRIMUI_FAKE_BATTERY="80", ARCADE_NO_AUDIO="1", ARCADE_TEST_MODEL="brickpro",
               ARCADE_FS_IP="192.0.2.20", ARCADE_FS_SHOWPORT="80", ARCADE_FS_PIN="482915")
    for lang in ("en", "it"):
        out = os.path.join(TOP, "docs", "images", *([] if lang == "en" else ["it"]))
        for name in names:
            prog, steps, extra = SHOTS[name]
            d = os.path.join(tmp, "sd")
            build_sd(d, lang)   # da capo ogni volta: una schermata non sporca la successiva
            if extra:
                sp = os.path.join(d, "System", "settings.json")
                st = json.load(open(sp)); st.update(extra); json.dump(st, open(sp, "w"))
            png = os.path.join(out, name + ".png")
            if os.path.exists(png):
                os.remove(png)   # se lo scatto non riesce, niente immagine vecchia che sembra nuova
            auto = f"{steps},shot:{png},quit"
            if prog == "launcher":
                cmd = [os.path.join(FE, "build", "launcher"), "--root", d, "--auto", auto]
            else:
                cmd = [os.path.join(FE, "build", "runner"), "--root", d, "--core", core, "--rom",
                       os.path.join(d, "Roms", "FBNEO", "castellobit.zip"), "--title", "Castello Bit", "--key", "castellobit", "--auto", auto]
            subprocess.run(cmd, env=env, cwd=FE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
            print(("ok  " if os.path.exists(png) else "NO  ") + os.path.relpath(png, TOP))
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--sd":   # solo la SD, per guardarla: --sd CARTELLA [LINGUA]
        build_sd(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "it")
        print("SD dimostrativa in", sys.argv[2])
    else:
        bad = [n for n in sys.argv[1:] if n not in SHOTS]
        if bad:
            sys.exit("schermate sconosciute: " + " ".join(bad) + "\nquelle che ci sono: " + " ".join(SHOTS))
        shoot(sys.argv[1:] or list(SHOTS))
