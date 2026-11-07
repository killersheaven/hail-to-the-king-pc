#!/usr/bin/env python3
"""Rebuilds notes/overlays/8C00F400.bin (exception handlers the game copies into RAM
at start-up) from the player's own 1ST_READ.BIN. The recipe lists which 16-byte rows
come from the program; the other rows are small tables of addresses.
usage: make_overlay.py <1ST_READ.BIN>"""
import json, os, sys
here = os.path.dirname(os.path.abspath(__file__))
r = json.load(open(os.path.join(here, "overlay_8C00F400.json")))
b = open(sys.argv[1], "rb").read(); o = bytearray(r["size"])
for a, kind, v in r["rows"]:
    o[a:a + 16] = b[v:v + 16] if kind == "bin" else bytes.fromhex(v)
d = os.path.join(here, "..", "notes", "overlays"); os.makedirs(d, exist_ok=True)
open(os.path.join(d, r["address"] + ".bin"), "wb").write(o)
print("overlay rebuilt")
