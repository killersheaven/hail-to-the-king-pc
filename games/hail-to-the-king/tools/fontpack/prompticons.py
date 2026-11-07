#!/usr/bin/env python3
"""Button-prompt icons for Xbox, PlayStation and Switch controllers.
Draws the four face-button icons for each controller as a white disc with the
symbol cut out, like the game's own icons, in the order the game's font sheet
keeps them: Y B A X (the Dreamcast's top, right, bottom and left buttons), so
buttons match by position. Writes <out>/<style>/icons.png: 4 cells of 18x24
texels at 4x (288x96), white with the brightness in alpha. The runtime paints
them over the icon row of the game's font sheet while the game runs, so no game
art is stored. ("dreamcast" is only used for the preview in the options window.) usage: prompticons.py <bold sans .ttf> <out dir>"""
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
STYLES = {"dreamcast": ["Y", "B", "A", "X"], "xbox": ["Y", "B", "A", "X"], "playstation": ["tri", "circ", "cross", "sq"], "switch": ["X", "A", "B", "Y"]}
SS, OUT = 16, 4                            # supersampling per texel; output scale

def icon(sym, font):
    W, H = 18 * SS, 24 * SS
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    cx, cy, r = W / 2, 12 * SS, 8.1 * SS     # disc 16.2 texels wide, centred in the 18-texel cell
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    lw = int(1.5 * SS)
    if len(sym) == 1:                        # a letter, cut out
        b = d.textbbox((0, 0), sym, font=font, anchor="lt")
        tw, th = b[2] - b[0], b[3] - b[1]
        d.text((cx - tw / 2 - b[0], cy - th / 2 - b[1]), sym, font=font, fill=0, anchor="lt")
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
        gx, gy = cx, cy + (-s + 0.62 * s * 2) / 3
        k = 1 - 2.2 * lw / (s * 1.62)
        d.polygon(pts, fill=0)
        d.polygon([(gx + (x - gx) * k, gy + (y - gy) * k) for x, y in pts], fill=255)
    return im.resize((18 * OUT, 24 * OUT), Image.LANCZOS)

if len(sys.argv) != 3: sys.exit(__doc__)
font = ImageFont.truetype(sys.argv[1], int(11.5 * SS))
for style, syms in STYLES.items():
    strip = np.concatenate([np.array(icon(s, font)) for s in syms], axis=1)
    rgba = np.stack([np.full_like(strip, 255)] * 3 + [strip], axis=-1)
    d = os.path.join(sys.argv[2], style); os.makedirs(d, exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(os.path.join(d, "icons.png"), optimize=True)
    print(style, "done")
