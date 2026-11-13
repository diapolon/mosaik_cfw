#!/usr/bin/env python3
"""Copertina di un gioco Doom: la schermata del titolo (TITLEPIC) di un file WAD, in PNG.

    python3 toolchain/wad-cover.py freedoom1.wad cover.png

TITLEPIC è un'immagine nel formato "patch" di Doom (colonne di tratti di pixel) con i colori della
tavolozza PLAYPAL. Usato da toolchain/package.sh per i pacchetti Freedoom dello Store."""
import struct
import sys
import zlib


def lumps(data):
    n, off = struct.unpack_from("<4xii", data, 0)
    out = {}
    for i in range(n):
        pos, size, name = struct.unpack_from("<ii8s", data, off + 16 * i)
        out.setdefault(name.rstrip(b"\0").decode("ascii", "replace").upper(), data[pos:pos + size])
    return out


def patch(p, pal):
    w, h = struct.unpack_from("<HH", p, 0)
    px = bytearray(b"\0\0\0" * w * h)
    for x in range(w):
        o = struct.unpack_from("<I", p, 8 + 4 * x)[0]
        top = -1
        while p[o] != 0xFF:
            start, n = p[o], p[o + 1]
            top = start if start > top else top + start   # tratti alti (DeePsea): la riga si somma
            for k in range(n):
                y = top + k
                if 0 <= y < h:
                    c = p[o + 3 + k]
                    px[3 * (y * w + x):3 * (y * w + x) + 3] = pal[3 * c:3 * c + 3]
            o += n + 4
    return w, h, px


def png(w, h, px):
    raw = b"".join(b"\0" + bytes(px[3 * w * y:3 * w * (y + 1)]) for y in range(h))
    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b"")


data = open(sys.argv[1], "rb").read()
L = lumps(data)
w, h, px = patch(L["TITLEPIC"], L["PLAYPAL"][:768])
# Doom disegna 320x200 su uno schermo 4:3: la copertina a 640x480, come appare nel gioco
W, H = 640, 480
big = bytearray(3 * W * H)
for y in range(H):
    sy = y * h // H
    for x in range(W):
        sx = x * w // W
        big[3 * (y * W + x):3 * (y * W + x) + 3] = px[3 * (sy * w + sx):3 * (sy * w + sx) + 3]
open(sys.argv[2], "wb").write(png(W, H, big))
