#!/usr/bin/env python3
"""Killers' Heaven splash screen, shared by the Evil Dead Collection ports.

    make_splash.py <logo.png> <out.rgba|out.h> "<game title>" [year]

Writes <u32 width><u32 height> then RGBA rows (1280x720), or, for a .h
output, a C header holding the picture as an embedded PNG: the
Killers' Heaven logo, the game's title and the credit lines. Fonts: DejaVu
(free licence), so nothing from a game is used."""
import struct, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
logo_path, out_path, title = sys.argv[1], sys.argv[2], sys.argv[3]
year = sys.argv[4] if len(sys.argv) > 4 else "2026"
import os
STYLE = os.environ.get("SPLASH_STYLE", "grey")   # grey: the logo as drawn; charcoal: logo lightened
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

if STYLE == "grey":                     # a soft radial grey, lighter in the middle
    img = Image.new("RGBA", (W, H))
    gp = img.load()
    for yy in range(H):
        for xx in range(W):
            d = (((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2.4) / (H / 1.6)) ** 2) ** 0.5
            v = int(150 - 62 * min(d, 1.0))
            gp[xx, yy] = (v, v, v + 3, 255)
else:
    img = Image.new("RGBA", (W, H), (34, 34, 38, 255))
logo = Image.open(logo_path).convert("RGBA")
# The logo is drawn for a white page: black letters with a red edge. On the
# dark splash the black fill becomes off-white and the red edge stays.
px = logo.load()
for yy in (range(logo.height) if STYLE != "grey" else ()):
    pass
for yy in (range(logo.height) if STYLE != "grey" else ()):
    for xx in range(logo.width):
        r, g, b, a = px[xx, yy]
        if a and r < 90 and g < 90 and b < 90:
            px[xx, yy] = (236, 236, 236, a)
bbox = logo.getbbox()
if bbox:
    logo = logo.crop(bbox)
lw = 820
logo = logo.resize((lw, round(logo.height * lw / logo.width)), Image.LANCZOS)
ly = 170
img.alpha_composite(logo, ((W - lw) // 2, ly))

d = ImageDraw.Draw(img)
def centred(text, y, size, fill, bold=False):
    f = ImageFont.truetype(FONT_B if bold else FONT, size)
    w = d.textlength(text, font=f)
    d.text(((W - w) / 2, y), text, font=f, fill=fill)

y = ly + logo.height + 46
T1, T2, T3 = ((22, 22, 24, 255), (45, 45, 48, 255), (60, 60, 64, 255)) if STYLE == "grey" \
    else ((236, 236, 236, 255), (170, 170, 170, 255), (110, 110, 114, 255))
centred(title, y, 34, T1, bold=True)
centred("Unofficial recompilation", y + 52, 24, T2)
centred(f"\u00a9 {year} Killers' Heaven", y + 88, 24, T2)
centred("A free fan project, not affiliated with or endorsed by the rights holders.",
        H - 74, 17, T3)
centred("Requires your own copy of the game.", H - 48, 17, T3)

if out_path.endswith(".h"):
    import io
    buf = io.BytesIO()
    img.convert("RGB").save(buf, "PNG", optimize=True)
    data = buf.getvalue()
    with open(out_path, "w") as f:
        f.write("/* Killers' Heaven splash, made by tools/make_splash.py. Do not edit. */\n")
        f.write("static const unsigned char splash_png[%d] = {\n" % len(data))
        for i in range(0, len(data), 20):
            f.write("    " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
        f.write("};\n")
else:
    with open(out_path, "wb") as f:
        f.write(struct.pack("<II", W, H))
        f.write(img.tobytes())
    img.convert("RGB").save(out_path.rsplit(".", 1)[0] + "_preview.png")
