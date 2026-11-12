#!/usr/bin/env python3
"""Icone delle app dello Store (icon.png, 256x256), disegnate qui: python3 package/apps/mkicons.py"""
import os
from PIL import Image, ImageDraw

D = os.path.dirname(os.path.abspath(__file__))
ACCENT, LIGHT = (15, 177, 212), (120, 214, 235)


def files_icon(path):
    S = 1024   # disegno grande, poi ridotto: bordi morbidi
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([96, 210, 470, 380], radius=60, fill=LIGHT)          # linguetta
    d.rounded_rectangle([96, 290, 928, 830], radius=80, fill=LIGHT)          # retro della cartella
    d.rounded_rectangle([150, 250, 860, 640], radius=40, fill=(245, 248, 250))  # fogli
    d.rounded_rectangle([96, 400, 928, 830], radius=80, fill=ACCENT)         # fronte
    im.resize((256, 256), Image.LANCZOS).save(path)


files_icon(os.path.join(D, "files", "icon.png"))
print("icone fatte")
