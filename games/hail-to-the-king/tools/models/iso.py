"""Sector and directory access for the model tools: wraps ../extract_disc.py."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from extract_disc import Disc
_disc = None
def open_disc(cue):
    global _disc; _disc = Disc(cue)
def sec(lba):
    try: return _disc.sector(lba)
    except IOError: return None
def files():
    pvd = sec(45016); rl, rs = struct.unpack('<I', pvd[158:162])[0], struct.unpack('<I', pvd[166:170])[0]
    out = []
    for k in range((rs + 2047) // 2048):
        s = sec(rl + k); i = 0
        while s and i < 2048 and s[i]:
            L = s[i]; nl = s[i + 32]; nm = s[i + 33:i + 33 + nl].split(b';')[0].decode('latin1')
            if nl > 1 or s[i + 33] > 1: out.append((nm, struct.unpack('<I', s[i + 2:i + 6])[0], struct.unpack('<I', s[i + 10:i + 14])[0]))
            i += L
    return out
