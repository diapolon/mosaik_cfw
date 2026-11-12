#!/usr/bin/env python3
"""Logo di avvio per U-Boot, scritta MOSAIK CFW (bootlogo.bmp nella partizione ARCADE, che per U-Boot è
"boot-resource"): stesso formato del logo TrimUI del firmware, 396x66 su fondo nero,
BMP non compresso dal basso verso l'alto, 32 bit (Smart Pro, alfa a 0 come l'originale) o
24 bit (Brick Pro).

    mklogo.py FONT.ttf USCITA.bmp 32|24 [ANTEPRIMA.png]
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

font_path, out, bits = sys.argv[1], sys.argv[2], int(sys.argv[3])
W, H = 396, 66
ACCENT = (15, 177, 212)   # colore principale del tema Tessera

img = Image.new("RGB", (W, H), (0, 0, 0))
d = ImageDraw.Draw(img)
# simbolo: quattro tessere arrotondate, una piena di bianco (come la selezione nel launcher)
font = ImageFont.truetype(font_path, 54)
font.set_variation_by_name("Black")
text = "MOSAIK"
bbox = d.textbbox((0, 0), text, font=font)
tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
# "cfw" piccolo e minuscolo in basso a destra della scritta, nel colore del tema, come il logo
# del README (docs/images/logo.png)
font2 = ImageFont.truetype(font_path, 20)
font2.set_variation_by_name("Black")
text2 = "cfw"
bbox2 = d.textbbox((0, 0), text2, font=font2)
tw2 = bbox2[2] - bbox2[0]
s, g, gap, gap2 = 21, 6, 15, 4
total = 2 * s + g + gap + tw + gap2 + tw2   # simbolo + scritta + cfw, centrati
x0, y0 = (W - total) // 2, (H - 2 * s - g) // 2
for i, (cx, cy) in enumerate([(0, 0), (1, 0), (0, 1), (1, 1)]):
    fill = (255, 255, 255) if i == 3 else ACCENT
    d.rounded_rectangle([x0 + cx * (s + g), y0 + cy * (s + g), x0 + cx * (s + g) + s, y0 + cy * (s + g) + s], radius=6, fill=fill)
tx = x0 + 2 * s + g + gap
ty = (H - th) // 2 - bbox[1]
d.text((tx - bbox[0], ty), text, font=font, fill=(255, 255, 255))
# stessa linea di base della scritta grande
base = ty + font.getmetrics()[0]
tx2 = tx + tw + gap2
d.text((tx2 - bbox2[0], base - font2.getmetrics()[0]), text2, font=font2, fill=ACCENT)
assert tx2 + tw2 <= W, f"scritta troppo larga: {tx2 + tw2} > {W}"

row = W * bits // 8
pad = (4 - row % 4) % 4
data = bytearray()
px = img.load()
for y in range(H - 1, -1, -1):
    for x in range(W):
        r, gg, b = px[x, y]
        data += bytes((b, gg, r, 0)) if bits == 32 else bytes((b, gg, r))
    data += b"\0" * pad
hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, bits, 0, len(data), 2835, 2835, 0, 0)
open(out, "wb").write(hdr + info + data)
if len(sys.argv) > 4:
    img.save(sys.argv[4])
print(f"{out}: {W}x{H}, {bits} bit, {54 + len(data)} byte")
