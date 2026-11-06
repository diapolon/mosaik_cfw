#!/usr/bin/env python3
"""Genera i filtri video e le cornici (bezel) predefinite, tutto originale.

Uso: make_assets.py <cartella frontend>
Crea filters/<id>/filter.json (+ mask.png) e bezels/_default.png, bezels/_default_vertical.png.
Scrive i PNG senza librerie esterne (zlib + struct).
"""
import json
import math
import os
import struct
import sys
import zlib


def write_png(path, w, h, rows):
    """rows: lista di bytes RGBA, una per riga"""
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def mask(path, pixels):
    """pixels: matrice di (r, g, b); la maschera moltiplica l'immagine (bianco = invariato)"""
    rows = [bytes(c for p in row for c in (*p, 255)) for row in pixels]
    write_png(path, len(pixels[0]), len(pixels), rows)


FILTERS = {
    "nitido": {"name": {"it": "Nitido", "en": "Sharp", "fr": "Net", "es": "Nítido", "de": "Scharf", "pt": "Nítido"},
               "order": 10, "smooth": False},
    "morbido": {"name": {"it": "Morbido", "en": "Smooth", "fr": "Doux", "es": "Suave", "de": "Weich", "pt": "Suave"},
                "order": 20, "smooth": True},
    "scanline": {"name": {"it": "Scanline", "en": "Scanlines", "fr": "Lignes", "es": "Scanlines", "de": "Scanlines", "pt": "Scanlines"},
                 "order": 30, "smooth": False, "mask": "mask.png", "mask_scale": "pixel", "mask_strength": 0.55,
                 "_mask": [[(255, 255, 255)], [(90, 90, 90)]]},
    "lcd": {"name": {"it": "Griglia LCD", "en": "LCD grid", "fr": "Grille LCD", "es": "Rejilla LCD", "de": "LCD-Raster", "pt": "Grelha LCD"},
            "order": 40, "smooth": False, "mask": "mask.png", "mask_scale": "pixel", "mask_strength": 0.6,
            "_mask": [[(255, 255, 255), (255, 255, 255), (150, 150, 150)],
                      [(255, 255, 255), (255, 255, 255), (150, 150, 150)],
                      [(150, 150, 150), (150, 150, 150), (130, 130, 130)]]},
    "crt": {"name": {"it": "CRT", "en": "CRT", "fr": "CRT", "es": "CRT", "de": "CRT", "pt": "CRT"},
            "order": 50, "smooth": True, "mask": "mask.png", "mask_scale": "pixel", "mask_strength": 0.75, "vignette": 0.3,
            "_mask": [[(255, 175, 175), (175, 255, 175), (175, 175, 255)],
                      [(140, 96, 96), (96, 140, 96), (96, 96, 140)]]},
}


def make_filters(base):
    for fid, spec in FILTERS.items():
        d = os.path.join(base, "filters", fid)
        os.makedirs(d, exist_ok=True)
        data = {k: v for k, v in spec.items() if not k.startswith("_")}
        if "_mask" in spec:
            mask(os.path.join(d, "mask.png"), spec["_mask"])
        with open(os.path.join(d, "filter.json"), "w", encoding="utf-8") as f:
            f.write(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def bezel(path, win_w, win_h, w=1280, h=720):
    """Cornice scura con righe diagonali leggere, griglia di altoparlante e filo luminoso
       attorno alla finestra trasparente (dove va il gioco)."""
    x0, y0 = (w - win_w) // 2, (h - win_h) // 2
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if x0 <= x < x0 + win_w and y0 <= y < y0 + win_h:
                row += b"\x00\x00\x00\x00"
                continue
            t = y / h
            r, g, b = 20 - 12 * t, 18 - 11 * t, 30 - 18 * t
            if (x + y) % 18 < 2:                      # righe diagonali
                r, g, b = r + 6, g + 6, b + 9
            side = x < x0
            cx = x0 / 2 if side else x0 + win_w + (w - x0 - win_w) / 2
            gx, gy = (x - cx) % 14, (y - h * 0.62) % 14
            if abs(x - cx) < (w - win_w) / 2 * 0.55 and abs(y - h * 0.62) < h * 0.16 and gx < 6 and gy < 6 \
                    and (gx - 3) ** 2 + (gy - 3) ** 2 < 8:   # griglia dell'altoparlante
                r, g, b = r + 22, g + 22, b + 28
            dist = min(abs(x - (x0 - 1)), abs(x - (x0 + win_w))) if y0 <= y < y0 + win_h else 999
            if dist < 7:                              # filo luminoso accanto alla finestra
                glow = math.exp(-dist / 2.2)
                r, g, b = r + 230 * glow, g + 46 * glow, b + 136 * glow
            row += bytes((max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b))), 255))
        rows.append(bytes(row))
    write_png(path, w, h, rows)


def main():
    base = sys.argv[1]
    make_filters(base)
    d = os.path.join(base, "bezels")
    os.makedirs(d, exist_ok=True)
    bezel(os.path.join(d, "_default.png"), 960, 720)            # giochi orizzontali 4:3
    bezel(os.path.join(d, "_default_vertical.png"), 540, 720)   # giochi verticali 3:4
    print("filtri:", ", ".join(FILTERS), "· cornici: _default, _default_vertical")


if __name__ == "__main__":
    main()
