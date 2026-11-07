#!/usr/bin/env python3
"""Build an HD replacement for the game's main font sheet (texture-pack PNG).

The game's font sheet (256x256, white glyphs in alpha) holds a Courier-style
monospace font on a 14x6 grid, then the Y/B/A/X button icons and a tiny pixel
font. This script redraws every glyph of the main font with a real TrueType
font at SCALE x resolution, positioned so each glyph keeps the original's
cell, baseline and centre (layout is unchanged); the button icons are
upscaled with the runtime's edge-directed filter and the pixel font is kept
pixel-sharp (nearest neighbour).

usage: fontpack.py <original sheet.png> <font.ttf> <upscaled.rgba from upsheet> <out.png>
"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

SCALE = 4
ROWS = ["ABCDEFGHIJKLMN", "OPQRSTUVWXYZab", "cdefghijklmnop",
        "qrstuvwxyz0123", "456789?!.,;:+-", "=|&()%\"'_<>*[]"]
DESCENDERS = set("gjpqyQ,;_|()[]")
MAIN_BOTTOM = 146          # rows of the main font end here; icons and the pixel font follow
PIXEL_FONT_TOP = 167       # tiny pixel font: kept pixel-sharp below this line

src = np.array(Image.open(sys.argv[1]).convert("RGBA"))
H, W = src.shape[:2]
alpha = src[..., 3]
font_path, up_path, out_path = sys.argv[2], sys.argv[3], sys.argv[4]

# ---- measure the original glyphs ----
row_bands = []
rows_any = (alpha[:MAIN_BOTTOM] > 0).any(axis=1)
y = 0
while y < MAIN_BOTTOM:
    if rows_any[y]:
        s = y
        while y < MAIN_BOTTOM and rows_any[y]: y += 1
        row_bands.append((s, y))
    y += 1
assert len(row_bands) == len(ROWS), row_bands
cw = W / 14.0
glyphs = {}            # char -> (x0, y0, x1, y1) ink box in the original
row_base = []
for r, chars in enumerate(ROWS):
    top = 0 if r == 0 else (row_bands[r - 1][1] + row_bands[r][0]) // 2
    bot = MAIN_BOTTOM if r == len(ROWS) - 1 else (row_bands[r][1] + row_bands[r + 1][0]) // 2
    bottoms = []
    # glyphs are not on an even grid: split the row at the empty column nearest
    # each nominal cell boundary
    band_cols = (alpha[top:bot] > 0).any(axis=0)
    cuts = [0]
    for k in range(1, len(chars)):
        nom = k * cw
        cand = [x for x in range(int(nom) - 6, int(nom) + 7) if 0 <= x < W and not band_cols[x]]
        cuts.append(min(cand, key=lambda x: abs(x + 0.5 - nom)) if cand else int(round(nom)))
    cuts.append(W)
    for k, ch in enumerate(chars):
        x0, x1 = cuts[k], cuts[k + 1]
        cell = alpha[top:bot, x0:x1]
        ys, xs = np.nonzero(cell)
        if len(ys) == 0: continue
        box = (x0 + xs.min(), top + ys.min(), x0 + xs.max() + 1, top + ys.max() + 1)
        glyphs[ch] = (box, (x0, top, x1, bot))
        if ch not in DESCENDERS and (ch.isalnum() or ch in "&%?!"): bottoms.append(box[3])
    row_base.append(int(np.median(bottoms)) if bottoms else row_bands[r][1])

# ---- size the TrueType font so capitals match the original cap height ----
cap_h = glyphs["A"][0][3] - glyphs["A"][0][1]
size = 10
while True:
    f = ImageFont.truetype(font_path, size)
    b = f.getbbox("A", anchor="ls")
    if (b[3] - b[1]) >= cap_h * SCALE: break
    size += 1
font = ImageFont.truetype(font_path, size)
# horizontal fit: the widest original glyph vs the same glyph in the new font
ratios = []
for ch in "MWHNA":
    ob = glyphs[ch][0]; nb = font.getbbox(ch, anchor="ls")
    ratios.append(((ob[2] - ob[0]) * SCALE) / max(1, nb[2] - nb[0]))
xstretch = min(1.15, max(0.85, float(np.median(ratios))))

# ---- compose the HD sheet ----
out = np.zeros((H * SCALE, W * SCALE, 4), np.uint8)
up = np.fromfile(up_path, np.uint8).reshape(H * SCALE, W * SCALE, 4)
out[MAIN_BOTTOM * SCALE:] = up[MAIN_BOTTOM * SCALE:]                       # button icons etc.
pix = np.repeat(np.repeat(src[PIXEL_FONT_TOP:], SCALE, 0), SCALE, 1)        # pixel font: nearest
out[PIXEL_FONT_TOP * SCALE:] = pix

for r, chars in enumerate(ROWS):
    base_y = row_base[r] * SCALE
    for ch in chars:
        if ch not in glyphs: continue
        (ox0, oy0, ox1, oy1), (cx0, cy0, cx1, cy1) = glyphs[ch]
        nb = font.getbbox(ch, anchor="ls")
        gw, gh = nb[2] - nb[0], nb[3] - nb[1]
        tile = Image.new("L", (gw + 8, gh + 8), 0)
        ImageDraw.Draw(tile).text((4 - nb[0], 4 - nb[1]), ch, font=font, fill=255, anchor="ls")
        # crop to the ink horizontally, apply the width fit, and never let it be wider
        # than the original glyph (plus half a pixel each side): the game's texture
        # coordinates for a character end close to its ink, so anything wider is cut
        # off or shows up as a sliver next to the neighbouring character
        t = np.array(tile)
        cols = np.nonzero(t.max(axis=0))[0]
        t = t[:, cols[0]:cols[-1] + 1]
        iw = t.shape[1]
        tw = max(1, int(round(iw * xstretch)))
        tw = min(tw, (ox1 - ox0) * SCALE + 2 * SCALE)
        t = np.array(Image.fromarray(t).resize((tw, t.shape[0]), Image.LANCZOS))
        # centre on the original glyph horizontally; baseline at the row baseline
        cx = (ox0 + ox1) / 2.0 * SCALE
        x = int(round(cx - tw / 2.0))
        y = base_y + nb[1] - 4
        # clip to the original cell so nothing spills into neighbouring rows or the icons
        ys = slice(max(cy0 * SCALE, y), min(cy1 * SCALE, y + t.shape[0]))
        xs = slice(max(cx0 * SCALE, ox0 * SCALE - SCALE, x), min(cx1 * SCALE, ox1 * SCALE + SCALE, x + t.shape[1]))
        if ys.stop <= ys.start or xs.stop <= xs.start: continue
        tt = t[ys.start - y:ys.stop - y, xs.start - x:xs.stop - x]
        region = out[ys, xs]
        a = np.maximum(region[..., 3], tt)
        region[..., 0] = region[..., 1] = region[..., 2] = region[..., 3] = a   # white, brightness = alpha (as the original)

Image.fromarray(out, "RGBA").save(out_path)
print("font size %d px, horizontal fit %.2f, %d glyphs -> %s" % (size, xstretch, len(glyphs), out_path))
