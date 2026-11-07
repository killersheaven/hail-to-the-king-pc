#!/usr/bin/env python3
"""Copies 1ST_READ.BIN (the game's program) out of a Redump .cue/.bin set.

usage: extract_disc.py <disc.cue> <out folder>
Reads the high-density data track the same way the runtime does (disc.c)."""
import os, struct, sys

def tracks(cue):
    d = os.path.dirname(os.path.abspath(cue)); lba = 0; cur = None; out = []
    for line in open(cue, encoding="latin-1"):
        if "HIGH-DENSITY" in line: lba = 45000
        s = line.strip()
        if s.startswith("FILE") and '"' in s:
            cur = os.path.join(d, s.split('"')[1])
            if not os.path.exists(cur): sys.exit("missing disc file: %s" % cur)
        elif "TRACK" in s and cur:
            n = os.path.getsize(cur) // 2352
            out.append((lba, lba + n, cur, "AUDIO" in s)); lba += n; cur = None
    return out

class Disc:
    def __init__(self, cue):
        self.t = tracks(cue); self.f = {}
    def sector(self, lba):
        for a, b, p, audio in self.t:
            if a <= lba < b and not audio:
                f = self.f.setdefault(p, open(p, "rb")); f.seek((lba - a) * 2352 + 16); return f.read(2048)
        raise IOError("LBA %d is not on a data track" % lba)
    def find(self, name):
        pvd = self.sector(45016)
        if pvd[1:6] != b"CD001": sys.exit("no ISO 9660 volume on the data track")
        rl, rs = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        for k in range((rs + 2047) // 2048):
            s = self.sector(rl + k); i = 0
            while i < 2048 and s[i]:
                L, nl = s[i], s[i + 32]
                nm = s[i + 33:i + 33 + nl].split(b";")[0].decode("latin-1")
                if nm.upper() == name.upper():
                    return struct.unpack_from("<I", s, i + 2)[0], struct.unpack_from("<I", s, i + 10)[0]
                i += L
        sys.exit("%s not found on the disc" % name)
    def read(self, name):
        lba, size = self.find(name)    # GD-ROM: extents are absolute LBAs
        return b"".join(self.sector(lba + k) for k in range((size + 2047) // 2048))[:size]

if __name__ == "__main__":
    if len(sys.argv) != 3: sys.exit(__doc__)
    os.makedirs(sys.argv[2], exist_ok=True)
    data = Disc(sys.argv[1]).read("1ST_READ.BIN")
    open(os.path.join(sys.argv[2], "1ST_READ.BIN"), "wb").write(data)
    print("1ST_READ.BIN: %d bytes" % len(data))
