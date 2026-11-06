#!/usr/bin/env python3
"""Converte i DAT XML di FinalBurn Neo e mame2003-plus in un indice TSV compatto.

Colonne: nome, titolo, anno, produttore, sistema, verticale (0/1), bios (0/1), parent
(vuoto se il gioco non è un clone: il launcher mostra solo questi nel catalogo dei mancanti).
Il launcher legge questo file invece del DAT completo (14 MB), così l'avvio resta veloce
anche sulla console.

Scrive anche <out>.roms accanto all'indice, per la verifica delle ROM:
  gioco<TAB>romof<TAB>file|dimensione|crc|m,file|...   (m = il file arriva dal parent o dal BIOS)

Uso: build_index.py fbneo <dat.xml> <out.tsv>
     build_index.py mame2003p <mame2003-plus.xml> <out.tsv>
"""
import sys
import xml.etree.ElementTree as ET

FBNEO_SYSTEMS = {
    "neogeo": "Neo Geo", "cave": "Cave", "toaplan": "Toaplan", "konami": "Konami",
    "taito": "Taito", "sega": "Sega", "irem": "Irem", "dataeast": "Data East",
    "snk": "SNK", "psikyo": "Psikyo", "pgm": "PGM", "galaxian": "Galaxian",
    "midway": "Midway", "seta": "Seta", "kaneko": "Kaneko", "nmk16": "NMK",
    "atari": "Atari", "technos": "Technos", "tecmo": "Tecmo", "sega_system16": "Sega",
}


def fbneo_system(sourcefile, maker):
    folder, _, fname = sourcefile.partition("/")
    if folder == "capcom":
        for tag, label in (("cps1", "CPS-1"), ("cps2", "CPS-2"), ("cps3", "CPS-3")):
            if tag in fname:
                return label
        return "Capcom"
    return FBNEO_SYSTEMS.get(folder, maker.split(" ")[0] if maker else "Arcade")


def clean(s):
    return (s or "").replace("\t", " ").replace("\n", " ").strip()


def main():
    kind, src, out = sys.argv[1:4]
    rows = []
    romlists = []
    for _, el in ET.iterparse(src, events=("end",)):
        if el.tag not in ("game", "machine"):
            continue
        name = el.get("name")
        title = clean(el.findtext("description"))
        year = clean(el.findtext("year"))
        maker = clean(el.findtext("manufacturer"))
        bios = el.get("isbios") == "yes" or el.get("runnable") == "no"
        if kind == "fbneo":
            system = fbneo_system(el.get("sourcefile", ""), maker)
            vertical = 0
        else:
            system = maker.split(" ")[0] if maker else "Arcade"
            video = el.find("video")
            vertical = 1 if video is not None and video.get("orientation") == "vertical" else 0
        parent = el.get("cloneof") or ""
        files = []
        for rom in el.findall("rom"):
            if rom.get("status") == "nodump" or not rom.get("crc"):
                continue
            files.append("%s|%s|%s|%s" % (rom.get("name", "").replace(",", "_").replace("|", "_"),
                                          rom.get("size", "0"), rom.get("crc", "").lower(), "m" if rom.get("merge") else ""))
        romlists.append((name, el.get("romof") or "", ",".join(files)))
        rows.append((name, title, year, maker, system, vertical, int(bios), parent))
        el.clear()
    rows.sort()
    with open(out, "w", encoding="utf-8") as f:
        for r in rows:
            f.write("\t".join(str(c) for c in r) + "\n")
    romlists.sort()
    roms_out = out[:-4] + ".roms" if out.endswith(".tsv") else out + ".roms"
    with open(roms_out, "w", encoding="utf-8") as f:
        for r in romlists:
            f.write("\t".join(r) + "\n")
    print(f"{out}: {len(rows)} giochi (+ {roms_out})")


if __name__ == "__main__":
    main()
