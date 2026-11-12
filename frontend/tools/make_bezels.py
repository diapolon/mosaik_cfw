#!/usr/bin/env python3
"""Disegna le cornici dei pacchetti Cornici dello Store, tutte originali (nessun marchio, logo
o disegno di altri). Uso: python3 tools/make_bezels.py frontend/package/bezels

Per ogni pacchetto scrive <cartella>/<id>/Bezels/<console>/_mosaik.png (1280x720, Smart Pro) e
_mosaik-1024x768.png (Brick Pro); il runner sceglie quella dello schermo (bezel_try). Le cornici
arcade hanno anche _mosaik_vertical. Il gioco va nella finestra trasparente: il runner la trova
da solo (pixel con alfa < 32). I PNG si tengono nel repository: rigenerarli con un'altra
versione di Pillow cambierebbe l'impronta dei pacchetti, e lo Store proporrebbe un
aggiornamento a vuoto (le cornici si confrontano per "version" in pack.json).
"""
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

SS = 3   # si disegna più grande e si riduce: bordi morbidi
SCREENS = [((1280, 720), "_mosaik.png"), ((1024, 768), "_mosaik-1024x768.png")]


def hexc(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def mix(c1, c2, t):
    return tuple(int(round(c1[i] + (c2[i] - c1[i]) * t)) for i in range(3)) + (255,)


def lighter(c, t):
    return mix(c, (255, 255, 255), t)


def darker(c, t):
    return mix(c, (0, 0, 0), t)


class Canvas:
    """Disegno in coordinate dello schermo finale, moltiplicate per SS."""

    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.im = Image.new("RGBA", (w * SS, h * SS), bg)
        self.d = ImageDraw.Draw(self.im, "RGBA")   # i colori trasparenti si fondono con quello che c'è sotto

    def s(self, v):
        return int(round(v * SS))

    def box(self, x, y, w, h):
        return [self.s(x), self.s(y), self.s(x + w) - 1, self.s(y + h) - 1]

    def rrect(self, x, y, w, h, r, fill, outline=None, width=0):
        self.d.rounded_rectangle(self.box(x, y, w, h), radius=self.s(r), fill=fill, outline=outline, width=self.s(width) if width else 0)

    def ellipse(self, cx, cy, rx, ry, fill, outline=None, width=0):
        self.d.ellipse([self.s(cx - rx), self.s(cy - ry), self.s(cx + rx), self.s(cy + ry)], fill=fill, outline=outline,
                       width=self.s(width) if width else 0)

    def circle(self, cx, cy, r, fill, outline=None, width=0):
        self.ellipse(cx, cy, r, r, fill, outline, width)

    def line(self, pts, fill, width):
        self.d.line([(self.s(x), self.s(y)) for x, y in pts], fill=fill, width=max(1, self.s(width)))

    def poly(self, pts, fill):
        self.d.polygon([(self.s(x), self.s(y)) for x, y in pts], fill=fill)

    def vgrad(self, x, y, w, h, c1, c2, r=0):
        """sfumatura verticale dentro un rettangolo arrotondato"""
        g = Image.new("RGBA", (1, 256))
        for i in range(256):
            g.putpixel((0, i), mix(c1, c2, i / 255))
        g = g.resize((self.s(w), self.s(h)), Image.BILINEAR)
        m = Image.new("L", g.size, 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, g.size[0] - 1, g.size[1] - 1], radius=self.s(r), fill=255)
        self.im.paste(g, (self.s(x), self.s(y)), m)

    def shadow(self, x, y, w, h, r, blur, alpha=140, dy=6):
        sh = Image.new("RGBA", self.im.size, (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle(self.box(x, y + dy, w, h), radius=self.s(r), fill=(0, 0, 0, alpha))
        sh = sh.filter(ImageFilter.GaussianBlur(self.s(blur)))
        self.im.alpha_composite(sh)

    def glow(self, draw_fn, blur, strength=1):
        """disegna una forma luminosa sfocata (neon) e poi la forma nitida sopra"""
        layer = Image.new("RGBA", self.im.size, (0, 0, 0, 0))
        saved = self.d
        self.d = ImageDraw.Draw(layer, "RGBA")
        draw_fn()
        self.d = saved
        blurred = layer.filter(ImageFilter.GaussianBlur(self.s(blur)))
        for _ in range(strength):
            self.im.alpha_composite(blurred)
        self.im.alpha_composite(layer)

    def save(self, path, win, corner=0):
        """riduce, apre la finestra trasparente (con gli angoli arrotondati coperti) e salva"""
        out = self.im.resize((self.w, self.h), Image.BOX)   # media dei pixel: niente aloni ai bordi
        x, y, w, h = win
        # alfa: opaco fuori dalla finestra, trasparente dentro tranne gli angoli arrotondati
        m = Image.new("L", (self.w * SS, self.h * SS), 255)
        ImageDraw.Draw(m).rounded_rectangle(self.box(x, y, w, h), radius=self.s(corner), fill=0)
        m = m.resize((self.w, self.h), Image.BOX)
        out.putalpha(m)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        out.save(path, optimize=True)
        # controllo: la finestra che troverà il runner è proprio quella voluta
        a = out.getchannel("A").point(lambda v: 255 if v < 32 else 0)
        bb = a.getbbox()
        assert bb and abs(bb[0] - x) <= 1 and abs(bb[1] - y) <= 1 and abs(bb[2] - (x + w)) <= 1 and abs(bb[3] - (y + h)) <= 1, (path, bb, win)


def background(c, W, H, top, bottom):
    c.vgrad(0, 0, W, H, top, bottom)
    # vignettatura leggera
    v = Image.new("L", (W * SS // 8, H * SS // 8), 0)
    dv = ImageDraw.Draw(v)
    dv.ellipse([-v.size[0] * 0.15, -v.size[1] * 0.3, v.size[0] * 1.15, v.size[1] * 1.3], fill=255)
    v = v.filter(ImageFilter.GaussianBlur(v.size[0] / 10)).resize(c.im.size, Image.BILINEAR)
    c.im = Image.composite(c.im, Image.alpha_composite(c.im, Image.new("RGBA", c.im.size, (0, 0, 0, 110))), v)
    c.d = ImageDraw.Draw(c.im, "RGBA")


# ------------------------------------------------------------------ portatili

HANDHELDS = {
    # id: (risoluzione del gioco, colore del guscio, colore dei tasti, colore dello sfondo)
    "gb": ((160, 144), "#C9C6BF", "#7C3152", "#26303A"),
    "gbc": ((160, 144), "#6E57A6", "#2D2B36", "#1F1E2C"),
    "gba": ((240, 160), "#4D53AE", "#D9DAE4", "#1A1D33"),
    "gg": ((160, 144), "#2E2F35", "#3E78C0", "#15171C"),
    "ngp": ((160, 152), "#B9BEC6", "#303036", "#22252B"),
    "lynx": ((160, 102), "#3B3B41", "#C9CBD3", "#17171A"),
    "ws": ((224, 144), "#E4E0D4", "#56627A", "#2A2C33"),
    "wsc": ((224, 144), "#3D80B6", "#EBEBF0", "#14263A"),
    "pokemini": ((96, 64), "#4E9159", "#EEEEEE", "#15291A"),
}


def fit(res, aw, ah):
    """misura della finestra: la scala intera più grande se è vicina a quella massima"""
    gw, gh = res
    k = min(aw / gw, ah / gh)
    ki = math.floor(k)
    if ki >= 1 and ki >= k * 0.9:
        k = ki
    return int(round(gw * k)), int(round(gh * k))


def dpad(c, cx, cy, u, col):
    arm = u * 0.95
    sh = darker(col, 0.5)
    c.rrect(cx - arm * 1.5 + 2, cy - arm / 2 + 3, arm * 3, arm, arm * 0.18, sh)
    c.rrect(cx - arm / 2 + 2, cy - arm * 1.5 + 3, arm, arm * 3, arm * 0.18, sh)
    c.rrect(cx - arm * 1.5, cy - arm / 2, arm * 3, arm, arm * 0.18, col)
    c.rrect(cx - arm / 2, cy - arm * 1.5, arm, arm * 3, arm * 0.18, col)
    c.circle(cx, cy, arm * 0.28, darker(col, 0.25))
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):   # frecce in rilievo
        px, py = cx + dx * arm * 1.05, cy + dy * arm * 1.05
        c.circle(px, py, arm * 0.09, lighter(col, 0.18))


def button(c, cx, cy, r, col):
    c.circle(cx + r * 0.08, cy + r * 0.18, r, darker(col, 0.55))
    c.circle(cx, cy, r, col)
    c.ellipse(cx - r * 0.25, cy - r * 0.35, r * 0.45, r * 0.3, lighter(col, 0.28))


def pill(c, cx, cy, w, h, col):
    c.rrect(cx - w / 2 + 1, cy - h / 2 + 2, w, h, h / 2, darker(col, 0.5))
    c.rrect(cx - w / 2, cy - h / 2, w, h, h / 2, col)


def handheld(path, W, H, res, body, btn, bg):
    """guscio di una portatile: comandi ai lati dello schermo, oppure sotto (schermi larghi
       sulla Brick Pro), scegliendo la disposizione che dà al gioco più spazio"""
    body, btn, bg = hexc(body), hexc(btn), hexc(bg)
    c = Canvas(W, H, bg)
    background(c, W, H, lighter(bg, 0.12), darker(bg, 0.25))
    lip = 28
    bx, by = round(W * 0.035), round(H * 0.035)
    bw, bh = W - 2 * bx, H - 2 * by
    room = 215 if W > H * 1.5 else 150     # spazio dei comandi ai lati
    band = 150                             # spazio dei comandi sotto
    side = fit(res, W - 2 * (bx + room + lip), bh - 2 * lip - 24)
    below = fit(res, bw - 2 * lip - 120, bh - band - 2 * lip - 30)
    beside = side[0] * side[1] >= below[0] * below[1]
    gw, gh = side if beside else below
    if beside:
        gx, gy = (W - gw) // 2, (H - gh) // 2
        lcx = (bx + gx - lip) / 2 + 4
        rcx = W - lcx
        ccy, pcy = H * 0.47, H * 0.72
        u = room * 0.26
    else:
        gx, gy = (W - gw) // 2, by + lip + 26
        bandy = gy + gh + lip
        ccy = (bandy + by + bh) / 2 - 4
        lcx, rcx = bx + bw * 0.19, bx + bw * 0.81
        u = band * 0.27
        pcy = ccy
    # guscio
    r = min(bh * 0.22, 150)
    c.shadow(bx, by, bw, bh, r, 18, 160, 10)
    c.vgrad(bx, by, bw, bh, lighter(body, 0.12), darker(body, 0.12), r)
    c.rrect(bx + 6, by + 6, bw - 12, bh - 12, r - 6, None, lighter(body, 0.22), 2)
    # cornice dello schermo
    sx, sy, sw, sh = gx - lip, gy - lip, gw + 2 * lip, gh + 2 * lip
    scr = darker(body, 0.72) if sum(body[:3]) > 300 else darker(body, 0.55)
    c.rrect(sx - 3, sy - 3, sw + 6, sh + 6, lip * 0.9, darker(body, 0.25))
    c.vgrad(sx, sy, sw, sh, lighter(scr, 0.08), darker(scr, 0.15), lip * 0.8)
    c.rrect(gx - 3, gy - 3, gw + 6, gh + 6, 4, darker(scr, 0.45))
    # spia accesa
    lx, ly = sx + lip * 0.5, sy + sh * 0.25
    c.glow(lambda: c.circle(lx, ly, 5, (255, 70, 60, 255)), 6)
    # comandi: croce a sinistra, due tasti a destra, start e select
    dpad(c, lcx, ccy, u, (34, 34, 40, 255))
    br = u * 0.62
    button(c, rcx - br * 1.15, ccy + br * 0.75, br, btn)
    button(c, rcx + br * 1.15, ccy - br * 0.75, br, btn)
    pw, ph = u * 1.15, u * 0.34
    if beside:
        pill(c, lcx, pcy, pw, ph, darker(body, 0.45))
        pill(c, rcx, pcy, pw, ph, darker(body, 0.45))
        gx0, gy0 = rcx - u * 0.9, H * 0.8
    else:
        pill(c, W / 2 - pw * 0.75, pcy, pw, ph, darker(body, 0.45))
        pill(c, W / 2 + pw * 0.75, pcy, pw, ph, darker(body, 0.45))
        gx0, gy0 = (lcx + W / 2) / 2 - u * 0.6, ccy - u * 0.25
    # griglia dell'altoparlante
    for k in range(5):
        x = gx0 + k * u * 0.42
        c.line([(x, gy0 + u * 0.55), (x + u * 0.38, gy0)], darker(body, 0.35), max(3, u * 0.1))
    c.save(path, (gx, gy, gw, gh))


# ------------------------------------------------------------------ televisore

def television(path, W, H):
    bg = hexc("#1C1A1F")
    c = Canvas(W, H, bg)
    background(c, W, H, hexc("#2A2630"), hexc("#141217"))
    wide = W > H * 1.5
    if wide:
        gh = H - 120
        gw = gh * 4 // 3
        gx, gy = 100, 60
        bx, by, bw, bh = 40, 12, W - 80, H - 24
    else:
        gw, gh = 768, 576
        gx, gy = (W - gw) // 2, 56
        bx, by, bw, bh = 52, 10, W - 104, H - 20
    body = hexc("#3A3236")
    r = 46
    c.shadow(bx, by, bw, bh, r, 16, 170, 8)
    c.vgrad(bx, by, bw, bh, hexc("#4A4045"), hexc("#2A2427"), r)
    # finitura legno scuro: venature sottili
    for k in range(0, int(bh), 7):
        y = by + k + 3 * math.sin(k * 0.13)
        c.line([(bx + 30, y), (bx + bw - 30, y + 2 * math.sin(k * 0.07))], hexc("#000000", 18), 1.2)
    c.rrect(bx + 5, by + 5, bw - 10, bh - 10, r - 5, None, hexc("#6A5C62", 120), 2)
    # incavo dello schermo e filo cromato
    m = 26
    c.rrect(gx - m - 4, gy - m - 4, gw + 2 * m + 8, gh + 2 * m + 8, 50, hexc("#8C8C94"))
    c.vgrad(gx - m, gy - m, gw + 2 * m, gh + 2 * m, hexc("#202024"), hexc("#0E0E10"), 44)
    # pannello dei comandi
    if wide:
        px0, px1 = gx + gw + m + 30, bx + bw - 34
        pcx = (px0 + px1) / 2
        c.rrect(px0, by + 30, px1 - px0, bh - 60, 20, hexc("#2B2528"))
        knob(c, pcx, by + bh * 0.22, (px1 - px0) * 0.3, True)
        knob(c, pcx, by + bh * 0.44, (px1 - px0) * 0.22, False)
        for k in range(8):
            y = by + bh * 0.6 + k * 16
            c.rrect(px0 + 22, y, px1 - px0 - 44, 7, 3, hexc("#151214"))
        c.glow(lambda: c.circle(pcx, by + bh - 52, 6, (90, 255, 140, 255)), 6)
    else:
        py0, py1 = gy + gh + m + 14, by + bh - 18
        pcy = (py0 + py1) / 2
        c.rrect(bx + 40, py0, bw - 80, py1 - py0, 18, hexc("#2B2528"))
        for k in range(10):
            x = bx + 70 + k * 22
            c.rrect(x, pcy - 26, 9, 52, 4, hexc("#151214"))
        knob(c, bx + bw - 210, pcy, (py1 - py0) * 0.36, True)
        knob(c, bx + bw - 110, pcy, (py1 - py0) * 0.28, False)
        c.glow(lambda: c.circle(bx + bw / 2, pcy, 6, (90, 255, 140, 255)), 6)
    c.save(path, (gx, gy, gw, gh), corner=34)


def knob(c, cx, cy, r, ticks):
    if ticks:   # selettore dei canali con le tacche
        for k in range(12):
            a = k / 12 * 2 * math.pi
            c.line([(cx + math.cos(a) * r * 1.18, cy + math.sin(a) * r * 1.18), (cx + math.cos(a) * r * 1.32, cy + math.sin(a) * r * 1.32)],
                   hexc("#9A9AA2"), 2.2)
    c.circle(cx + 2, cy + 4, r, hexc("#000000", 150))
    c.circle(cx, cy, r, hexc("#B9B9C0"))
    c.circle(cx, cy, r * 0.82, hexc("#8E8E96"))
    c.ellipse(cx - r * 0.25, cy - r * 0.32, r * 0.42, r * 0.25, hexc("#E4E4EA", 170))
    c.line([(cx, cy - r * 0.78), (cx, cy - r * 0.3)], hexc("#2A2A30"), max(2, r * 0.1))


# ------------------------------------------------------------------ sala giochi

def cabinet(path, W, H, vertical):
    bg = hexc("#0B0A14")
    c = Canvas(W, H, bg)
    background(c, W, H, hexc("#1B1530"), hexc("#07060C"))
    if vertical:
        gh = H
        gw = gh * 3 // 4
    elif W > H * 1.5:
        gw, gh = H * 4 // 3, H
    else:
        gw, gh = W - 64, H - 48
    gx, gy = (W - gw) // 2, (H - gh) // 2
    side = gx
    if side >= 120:
        for s in (0, 1):
            x0 = 0 if s == 0 else gx + gw
            cx = x0 + side / 2
            # altoparlante in alto
            spk = min(side * 0.32, 70)
            c.circle(cx, H * 0.2, spk + 6, hexc("#2A2440"))
            c.circle(cx, H * 0.2, spk, hexc("#120F1E"))
            for yy in range(-int(spk) + 8, int(spk) - 4, 12):
                for xx in range(-int(spk) + 8, int(spk) - 4, 12):
                    if xx * xx + yy * yy < (spk - 10) ** 2:
                        c.circle(cx + xx, H * 0.2 + yy, 3.2, hexc("#3A3258"))
            # porta delle monete in basso
            dw, dh = min(side * 0.62, 150), min(side * 0.62, 150) * 1.05
            dx, dy = cx - dw / 2, H * 0.84 - dh / 2
            c.rrect(dx, dy, dw, dh, 10, hexc("#2C2838"))
            c.rrect(dx + 6, dy + 6, dw - 12, dh - 12, 8, hexc("#3E3950"))
            for k, ox in enumerate((-0.22, 0.22)):
                sx = cx + ox * dw
                c.glow(lambda sx=sx: c.rrect(sx - dw * 0.08, dy + dh * 0.2, dw * 0.16, dh * 0.34, 5, (255, 60, 70, 255)), 8)
                c.rrect(sx - 2.5, dy + dh * 0.25, 5, dh * 0.24, 2, hexc("#200608"))
            c.rrect(cx - dw * 0.3, dy + dh * 0.68, dw * 0.6, dh * 0.12, 4, hexc("#1A1724"))
            # tre frecce al neon, dal magenta al ciano
            aw_ = min(side * 0.42, 110)
            for k in range(3):
                col = mix((255, 64, 170), (60, 220, 255), k / 2)
                yk = H * 0.40 + k * aw_ * 0.42
                pts = [(cx - aw_ / 2, yk + aw_ * 0.3), (cx, yk), (cx + aw_ / 2, yk + aw_ * 0.3)]
                c.glow(lambda pts=pts, col=col: c.line(pts, col, 4), 7, 2)
    # bordino luminoso (T-molding) intorno al monitor
    tm = (255, 64, 170, 255)
    if gy > 0:
        c.glow(lambda: c.rrect(gx - 6, gy - 6, gw + 12, gh + 12, 8, None, tm, 3), 8, 2)
    else:   # monitor alto quanto lo schermo: solo i due lati
        c.glow(lambda: (c.line([(gx - 4, 0), (gx - 4, H)], tm, 3), c.line([(gx + gw + 4, 0), (gx + gw + 4, H)], tm, 3)), 8, 2)
    c.save(path, (gx, gy, gw, gh))


# ------------------------------------------------------------------ pacchetti

TV = ["nes", "snes", "md", "sms", "pce", "pcecd", "segacd", "ps", "a2600", "a7800", "coleco", "intv"]
ARCADE = ["fbneo", "mame2003p", "mame2010"]


def main():
    out = sys.argv[1]
    for sys_id, (res, body, btn, bg) in HANDHELDS.items():
        for (W, H), name in SCREENS:
            handheld(os.path.join(out, "portatili", "Bezels", sys_id, name), W, H, res, body, btn, bg)
    for (W, H), name in SCREENS:
        tmp = os.path.join(out, ".tv-" + name)
        television(tmp, W, H)
        data = open(tmp, "rb").read()
        os.remove(tmp)
        for sys_id in TV:
            p = os.path.join(out, "tv", "Bezels", sys_id, name)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(data)
    for vertical in (False, True):
        for (W, H), name in SCREENS:
            if vertical:
                name = name.replace("_mosaik", "_mosaik_vertical")
            tmp = os.path.join(out, ".arc-" + name)
            cabinet(tmp, W, H, vertical)
            data = open(tmp, "rb").read()
            os.remove(tmp)
            for sys_id in ARCADE:
                p = os.path.join(out, "arcade", "Bezels", sys_id, name)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "wb").write(data)
    print("cornici in", out)


if __name__ == "__main__":
    main()
