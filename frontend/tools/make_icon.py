#!/usr/bin/env python3
"""Icona dell'app per il menu TrimUI (300x300, come le altre app): una leva arcade su
fondo arrotondato. Disegnata in grande e ridotta per avere bordi morbidi."""
import sys
from PIL import Image, ImageDraw

S = 4                      # sovracampionamento
W = 300 * S
img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# fondo: quadrato arrotondato con sfumatura verticale blu notte -> viola
grad = Image.new("RGBA", (W, W))
for y in range(W):
    t = y / (W - 1)
    c = (int(28 + 60 * t), int(36 + 10 * t), int(92 + 40 * t), 255)
    ImageDraw.Draw(grad).line([(0, y), (W, y)], fill=c)
mask = Image.new("L", (W, W), 0)
ImageDraw.Draw(mask).rounded_rectangle([8 * S, 8 * S, W - 8 * S, W - 8 * S], radius=64 * S, fill=255)
img.paste(grad, (0, 0), mask)
# base della leva
d.rounded_rectangle([70 * S, 196 * S, 230 * S, 236 * S], radius=20 * S, fill=(18, 20, 40, 255))
d.rounded_rectangle([70 * S, 190 * S, 230 * S, 226 * S], radius=18 * S, fill=(235, 238, 248, 255))
# asta e pallino
d.rounded_rectangle([140 * S, 104 * S, 160 * S, 204 * S], radius=8 * S, fill=(200, 205, 220, 255))
d.ellipse([104 * S, 52 * S, 196 * S, 144 * S], fill=(232, 52, 74, 255))
d.ellipse([124 * S, 66 * S, 156 * S, 92 * S], fill=(255, 150, 160, 200))
# due pulsanti
for cx, col in ((222, (255, 196, 40, 255)), (248, (64, 200, 255, 255))):
    d.ellipse([(cx - 14) * S, 150 * S, (cx + 14) * S, 178 * S], fill=col)
img = img.resize((300, 300), Image.LANCZOS)
img.save(sys.argv[1] if len(sys.argv) > 1 else "icon.png")
