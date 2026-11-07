#!/usr/bin/env python3
"""HD legal screen: redraws the THQ legal text (texture 33522E637940C0F6, a 320x240
picture in a 512x256 texture) at 4x with EB Garamond, the open-licence revival of
the Garamond face the original uses, and adds one credit line in the same style.
usage: legalpack.py <original.png> <EBGaramond.ttf> <out.png>"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
S = 4
LINES = ["Evil Dead: Hail to the King © 2000 THQ Inc.",
         "Evil Dead and its related characters are",
         "trademarks of Renaissance Pictures Ltd.,",
         "and licensed exclusively to THQ Inc.",
         "Evil Dead 2: Dead by Dawn™ & © 2000 Canal+ DA.",
         "Evil Dead 2 is a trademark of Canal+ DA.",
         "Hail to the King, the Hail to the King logo,",
         "Heavy Iron Studios, the Heavy Iron Studios logo,",
         "THQ and the THQ logo are trademarks and/or",
         "registered trademarks of THQ Inc.",
         "All rights reserved."]
CREDIT = "Unofficial PC recompilation © 2026 Killers’ Heaven."
src = np.array(Image.open(sys.argv[1]).convert("RGBA"))
H, W = src.shape[:2]
out = np.repeat(np.repeat(src, S, 0), S, 1).copy()          # keeps alpha and the unused texture area
bg = src[5, 5, :3]
out[:240 * S, :320 * S, :3] = bg
# original layout: widest line (line 5) spans 246 px; line pitch 14.8 px; ink top of line 1 at y=43
widest, pitch, top0 = 246 * S, 14.8 * S, 43 * S
# match the original's capital height (7 px), then letter-space to match its line width
cap = 7.0 * S                 # the original capitals are 7 px tall
WEIGHT = 560                  # EB Garamond is a variable font: a little heavier than Regular, like the original
def load(sz):
    f = ImageFont.truetype(sys.argv[2], sz)
    try: f.set_variation_by_axes([WEIGHT])
    except Exception: pass
    return f
size = 20
while True:
    f = load(size)
    b = f.getbbox("E", anchor="ls")
    if b[3] - b[1] >= cap: break
    size += 1
font = load(size)
track = (widest - font.getlength(LINES[4])) / (len(LINES[4]) - 1)
def draw_tracked(d, cx, base, text):
    total = font.getlength(text) + track * (len(text) - 1)
    x = cx - total / 2
    for i, ch in enumerate(text):
        d.text((x + font.getlength(text[:i]) + track * i, base), ch, font=font, fill=255, anchor="ls")
gap = 0.6                                                 # blank space before the credit, in lines
shift = (gap + 1) * pitch / 2                             # keep the block centred
rows = [(t, i * pitch) for i, t in enumerate(LINES)] + [(CREDIT, (len(LINES) + gap) * pitch)]
asc = -font.getbbox(LINES[0], anchor="ls")[1]             # baseline to ink top of line 1
img = Image.new("L", (320 * S, 240 * S), 0)
d = ImageDraw.Draw(img)
for text, dy in rows:
    base = top0 - shift + asc + dy
    draw_tracked(d, 160 * S, base, text)
a = np.array(img).astype(np.float32)[..., None] / 255.0
ink = np.array([232, 232, 228], np.float32)
region = out[:240 * S, :320 * S, :3].astype(np.float32)
out[:240 * S, :320 * S, :3] = (region * (1 - a) + ink * a + 0.5).astype(np.uint8)
Image.fromarray(out, "RGBA").save(sys.argv[3])
print("font size %d px at %dx, tracking %.1f px, %d lines" % (size, S, track, len(rows)))
