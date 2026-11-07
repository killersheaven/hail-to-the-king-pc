#!/usr/bin/env python3
"""Button-prompt variants of the font sheet (9B28D9CBB4D57B31_256x256).
The sheet's icon row (y 144-168) holds four 18x24 cells in the order Y B A X,
i.e. the Dreamcast's top, right, bottom and left face buttons. Each variant
redraws those cells for another controller, by position, as a white disc with
the symbol cut out (like the original icons; the game tints them, and the
runtime recolours them per controller):
  xbox:        Y  B  A  X
  playstation: triangle  circle  cross  square
  switch:      X  A  B  Y
Writes <out>/<style>/hd/<sheet> (from the HD sheet) and <out>/<style>/sd/<sheet>
(from the original sheet).
usage: promptpack.py <original sheet> <HD sheet> <bold sans .ttf> <out dir>"""
import sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont
NAME = "9B28D9CBB4D57B31_256x256.png"
STYLES = {"xbox": ["Y", "B", "A", "X"], "playstation": ["tri", "circ", "cross", "sq"], "switch": ["X", "A", "B", "Y"]}
SS = 16                                    # supersampling per source texel

def icon(sym, font):
    W, H = 18 * SS, 24 * SS
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    cx, cy, r = W / 2, 12 * SS, 8.1 * SS     # disc 16.2 texels wide, centred in the 18-texel window
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    lw = int(1.5 * SS)
    if len(sym) == 1:                        # a letter, cut out
        f = font
        b = d.textbbox((0, 0), sym, font=f, anchor="lt")
        tw, th = b[2] - b[0], b[3] - b[1]
        d.text((cx - tw / 2 - b[0], cy - th / 2 - b[1]), sym, font=f, fill=0, anchor="lt")
    elif sym == "cross":
        s = 3.9 * SS
        d.line((cx - s, cy - s, cx + s, cy + s), fill=0, width=lw); d.line((cx - s, cy + s, cx + s, cy - s), fill=0, width=lw)
    elif sym == "circ":
        s = 4.3 * SS
        d.ellipse((cx - s, cy - s, cx + s, cy + s), outline=0, width=lw)
    elif sym == "sq":
        s = 3.8 * SS
        d.rectangle((cx - s, cy - s, cx + s, cy + s), outline=0, width=lw)
    elif sym == "tri":
        s = 4.8 * SS
        pts = [(cx, cy - s), (cx + s * 0.95, cy + s * 0.62), (cx - s * 0.95, cy + s * 0.62)]
        gx, gy = cx, cy + (-s + 0.62 * s * 2) / 3          # centroid
        k = 1 - 2.2 * lw / (s * 1.62)                      # inset by about one stroke width
        d.polygon(pts, fill=0)
        d.polygon([(gx + (x - gx) * k, gy + (y - gy) * k) for x, y in pts], fill=255)
    return im

def put(sheet, scale, cells):
    a = np.array(sheet.convert("RGBA"))
    for k, im in enumerate(cells):
        t = np.array(im.resize((18 * scale, 24 * scale), Image.LANCZOS))
        y0, x0 = 144 * scale, 18 * k * scale
        a[y0:y0 + 24 * scale, x0:x0 + 18 * scale] = np.stack([t, t, t, t], axis=-1)   # white, brightness = alpha
    # the 4 texels right of the X cell (72-76) belonged to the old X icon's overhang: clear them
    a[144 * scale:168 * scale, 72 * scale:76 * scale] = 0
    return Image.fromarray(a, "RGBA")

orig, hd = Image.open(sys.argv[1]), Image.open(sys.argv[2])
font = ImageFont.truetype(sys.argv[3], int(11.5 * SS))
for style, syms in STYLES.items():
    cells = [icon(s, font) for s in syms]
    for sub, sheet, scale in (("hd", hd, hd.size[0] // 256), ("sd", orig, 1)):
        d = os.path.join(sys.argv[4], style, sub); os.makedirs(d, exist_ok=True)
        put(sheet, scale, cells).save(os.path.join(d, NAME))
    print(style, "done")
