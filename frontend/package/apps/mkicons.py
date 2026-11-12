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


def viewer_icon(path):
    S = 1024
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([96, 180, 928, 844], radius=90, fill=ACCENT)               # cornice
    d.rounded_rectangle([156, 240, 868, 784], radius=50, fill=(245, 248, 250))     # foto
    d.polygon([(190, 750), (420, 470), (560, 640), (660, 540), (834, 750)], fill=LIGHT)   # monti
    d.ellipse([640, 290, 780, 430], fill=(255, 196, 60))                            # sole
    im.resize((256, 256), Image.LANCZOS).save(path)


def music_icon(path):
    S = 1024
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([96, 96, 928, 928], radius=200, fill=ACCENT)
    d.ellipse([250, 600, 470, 790], fill=(245, 248, 250))          # note
    d.ellipse([560, 530, 780, 720], fill=(245, 248, 250))
    d.rectangle([420, 260, 470, 700], fill=(245, 248, 250))         # gambi
    d.rectangle([730, 190, 780, 630], fill=(245, 248, 250))
    d.polygon([(420, 260), (780, 190), (780, 300), (420, 370)], fill=(245, 248, 250))   # trave
    im.resize((256, 256), Image.LANCZOS).save(path)


def sdtest_icon(path):
    S = 1024
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.polygon([(250, 96), (720, 96), (840, 216), (840, 928), (184, 928), (184, 162)], fill=ACCENT)   # scheda
    for i in range(4):
        d.rounded_rectangle([280 + i * 130, 150, 360 + i * 130, 330], radius=20, fill=LIGHT)      # contatti
    d.line([(300, 640), (460, 800), (740, 470)], fill=(245, 248, 250), width=90, joint="curve")    # spunta
    im.resize((256, 256), Image.LANCZOS).save(path)


files_icon(os.path.join(D, "files", "icon.png"))
sdtest_icon(os.path.join(D, "sdtest", "icon.png"))
music_icon(os.path.join(D, "music", "icon.png"))
viewer_icon(os.path.join(D, "viewer", "icon.png"))
print("icone fatte")
