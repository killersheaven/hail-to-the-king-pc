"""Function discovery for 1ST_READ.BIN (loaded at 0x8C010000).

Recursive descent from the entry point and from every call target found.
Within a function we follow conditional/unconditional branches; we stop at
rts / jmp / braf. Calls (bsr, jsr via a PC-relative literal, bsrf) create new
function seeds. Literal-pool words read by mov.l @(disp,PC) are marked data.
A second pass seeds from literal-pool words that point at plausible GCC
prologues (sts.l pr,@-r15 / mov.l r14,@-r15 ...), catching functions reached
only through pointers (vtables, callbacks, script method tables).
"""
import struct, sys, json, collections
import sh4

BASE = 0x8C010000
img = open(sys.argv[1] if len(sys.argv) > 1 else "../orig/1ST_READ.BIN", "rb").read()
END = BASE + len(img)


# Overlays: code the game writes into RAM at runtime (e.g. exception vector trampolines).
# notes/overlays/<HEXADDR>.bin holds a RAM snapshot of that region.
import os as _os
OVL = []
_od = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "../notes/overlays")
if _os.path.isdir(_od):
    for _f in sorted(_os.listdir(_od)):
        if _f.endswith(".bin"):
            _b = open(_os.path.join(_od, _f), "rb").read()
            OVL.append((int(_f[:-4], 16), _b))


def in_ovl(a):
    for base, b in OVL:
        if base <= a < base + len(b): return (base, b)
    return None


def in_img(a): return BASE <= a < END or in_ovl(a) is not None


def h(a):
    if BASE <= a < END: return struct.unpack_from("<H", img, a - BASE)[0]
    o = in_ovl(a)
    return struct.unpack_from("<H", o[1], a - o[0])[0] if o else 0xFFFF   # outside: illegal opcode


def w(a):
    if BASE <= a < END: return struct.unpack_from("<I", img, a - BASE)[0]
    o = in_ovl(a)
    return struct.unpack_from("<I", o[1], a - o[0])[0] if o else 0


code = {}            # addr -> Insn
data = set()         # addresses known to be data (literal pools)
funcs = {}           # entry -> dict(blocks, calls, end, unresolved)
func_of = {}         # insn addr -> owning function entry
queue = collections.deque()
seen_seed = set()
unresolved = []      # (func, pc, mnemonic)


def seed(a, why):
    if in_img(a) and a % 2 == 0 and a not in seen_seed:
        seen_seed.add(a); queue.append((a, why))


def is_prologue(a):
    """GCC SH prologue, possibly interleaved by the scheduler: within the first 8
    instructions a push of PR or R14 onto r15 appears before any control flow."""
    if not in_img(a + 16) or a % 2: return False
    for k in range(8):
        op = h(a + 2 * k)
        ins = sh4.decode(op)
        if ins is None or ins.kind: return False
        if op == 0x4F22 or op == 0x2FE6: return True
    return False


jumptables = {}   # braf pc -> dict(table, esize, targets)


def resolve_jt(pc):
    """GCC SH switch: [mov #K,rL ... cmp/hi rL,rI; bt dflt] mova T,r0; [add rI,rI]; mov.{b,w} @(r0,rI),rX; [extu.b]; braf rX"""
    braf = sh4.decode(h(pc)); rx = braf.n
    tbl = esize = None; unsigned = False; limit = None; idx = None; idx_add = 0
    for a in [pc - 2 * k for k in range(1, 48)]:
        if not in_img(a): break
        i = sh4.decode(h(a))
        if i is None: break
        if i.kind in ("rts", "rte", "jmp", "braf", "bsr", "jsr"): break
        if esize is None and (i.op & 0xF00F) in (0x000C, 0x000D) and i.n == rx:
            esize = 1 if (i.op & 0xF) == 0xC else 2; idx = i.m
        elif esize is None and (i.op & 0xF00F) == 0x600C and i.n == rx:
            unsigned = True
        elif esize and tbl is None and (i.op & 0xFF00) == 0xC700:
            tbl = sh4.pcrel_addr(i, a)
        elif esize and (((i.op & 0xF00F) == 0x300C and i.n == idx and i.m == idx) or i.op == (0x4000 | (idx << 8))):
            pass                                       # index doubling (add rI,rI / shll rI)
        elif esize and (i.op & 0xF000) == 0x7000 and i.n == idx and limit is None:
            idx_add += i.i
        elif esize and (i.op & 0xF00F) == 0x6003 and i.n == idx and limit is None:
            idx = i.m                                  # mov rI',rI : follow copy
        elif tbl and limit is None and (i.op & 0xF00F) in (0x3006, 0x3002) and i.n == idx:   # cmp/hi | cmp/hs rL,rI
            rl = i.m; hs = (i.op & 0xF) == 2
            for b in [a - 2 * k for k in range(1, 48)]:
                j = sh4.decode(h(b))
                if j is None or j.kind in ("rts", "rte", "jmp", "braf", "bra"): break
                # instruction in a bra delay slot belongs to another path
                pj = sh4.decode(h(b - 2))
                if pj is not None and pj.kind == "bra": break
                if (j.op & 0xF000) == 0xE000 and j.n == rl:
                    limit = j.i - (1 if hs else 0); break
                if (j.op & 0xF000) == 0x9000 and j.n == rl:
                    limit = (lambda v: v - 0x10000 if v & 0x8000 else v)(h(sh4.pcrel_addr(j, b))) - (1 if hs else 0); break
                if j.fmt in ("nm", "n", "ni") and j.n == rl and not j.kind: break
            break
    if tbl is None or esize is None: return None
    n = (limit + 1 + max(0, idx_add)) if limit is not None and limit >= 0 else None
    targets = []; k = 0
    while True:
        if n is not None and k >= n: break
        if n is None and k >= 256: break
        ea = tbl + k * esize
        if not in_img(ea + esize - 1): break
        if n is None and (ea in code or ea in func_of): break
        if esize == 1:
            v = img[ea - BASE]; v = v if unsigned else (v - 256 if v & 0x80 else v)
        else:
            v = struct.unpack_from("<h", img, ea - BASE)[0]
        t = pc + 4 + v
        if k < idx_add:                                # slots below the biased index are never used
            targets.append(None); k += 1; continue
        if t & 1:
            if n is None: break
            targets.append(None); k += 1; continue
        if n is None and (not in_img(t) or sh4.decode(h(t)) is None or tbl <= t < ea + esize or t < pc - 0x4000): break
        targets.append(t); k += 1
    if not any(targets): return None
    for a in range(tbl, tbl + len(targets) * esize): data.add(a & ~1)
    jumptables[pc] = dict(table=tbl, esize=esize, n=len(targets), bounded=n is not None, targets=targets)
    return [t for t in targets if t]


def explore(entry):
    f = funcs.setdefault(entry, dict(calls=set(), insns=0, lo=entry, hi=entry, tail=set()))
    work = [entry]; visited = set()
    regsrc = {}  # last literal loaded per register in straight-line order (cheap const-prop)
    while work:
        pc = work.pop()
        regsrc = {}
        while True:
            if pc in visited or not in_img(pc) or pc in data:
                break
            ins = sh4.decode(h(pc))
            if ins is None:
                unresolved.append((entry, pc, "illegal %04x" % h(pc)))
                break
            visited.add(pc); code[pc] = ins; func_of.setdefault(pc, entry)
            f["insns"] += 1; f["lo"] = min(f["lo"], pc); f["hi"] = max(f["hi"], pc + 2)
            lit = sh4.pcrel_addr(ins, pc)
            if lit is not None and in_img(lit):
                size = 2 if (ins.op & 0xF000) == 0x9000 else 4
                if (ins.op & 0xFF00) != 0xC700:     # mova only takes an address
                    for k in range(0, size, 2): data.add(lit + k)
                    if size == 4:
                        regsrc[ins.n] = w(lit)
                    else:
                        regsrc[ins.n] = ("w", (lambda v: v - 0x10000 if v & 0x8000 else v)(h(lit)))
                else:
                    regsrc[0] = ("mova", lit)
            elif ins.fmt in ("nm",) and ins.mn.startswith("mov r"):
                regsrc[ins.n] = regsrc.get(ins.m)
            elif ins.fmt in ("n", "nm", "ni", "nmd", "nd8") and not ins.kind:
                regsrc.pop(ins.n, None)

            k = ins.kind
            if k in ("bsr",):
                t = sh4.branch_target(ins, pc); f["calls"].add(t); seed(t, "bsr")
            elif k == "jsr":
                t = regsrc.get(ins.n)
                if isinstance(t, int) and in_img(t):
                    f["calls"].add(t); seed(t, "jsr")
                else:
                    f["calls"].add(("ind", pc))
            elif k == "bsrf":
                f["calls"].add(("ind", pc))
            if k in ("bt", "bf", "bts", "bfs"):
                work.append(sh4.branch_target(ins, pc))
            if ins.delay:
                # decode delay slot as part of this path
                ds = pc + 2
                if in_img(ds) and ds not in visited:
                    dins = sh4.decode(h(ds))
                    if dins:
                        visited.add(ds); code[ds] = dins; func_of.setdefault(ds, entry)
                        f["insns"] += 1; f["hi"] = max(f["hi"], ds + 2)
                        lit = sh4.pcrel_addr(dins, ds)
                        if lit is not None and in_img(lit) and (dins.op & 0xFF00) != 0xC700:
                            for kk in range(0, 2 if (dins.op & 0xF000) == 0x9000 else 4, 2): data.add(lit + kk)
            if k == "bra":
                t = sh4.branch_target(ins, pc)
                if t in funcs and t != entry:
                    f["tail"].add(t)          # tail call into a known function
                else:
                    work.append(t)
                break
            if k in ("rts", "rte"):
                break
            if k == "jmp":
                t = regsrc.get(ins.n)
                if isinstance(t, int) and in_img(t):
                    f["tail"].add(t); seed(t, "tailjmp")
                else:
                    unresolved.append((entry, pc, "jmp @r%d" % ins.n))
                break
            if k == "braf" and isinstance(regsrc.get(ins.n), tuple) and regsrc[ins.n][0] == "w":
                work.append(pc + 4 + regsrc[ins.n][1])       # long-range constant branch
                break
            if k == "braf":
                ts = resolve_jt(pc)
                if ts:
                    work.extend(ts)
                else:
                    unresolved.append((entry, pc, "braf r%d" % ins.n))
                break
            if k == "bts" or k == "bfs":
                pass
            pc += 4 if ins.delay else 2
    f["visited"] = visited


def run(entry):
    seed(entry, "entry")
    while queue:
        a, why = queue.popleft()
        if a in funcs: continue
        explore(a)


def gaps():
    cov = bytearray(END - BASE)
    for a in code: cov[a - BASE] = cov[a - BASE + 1] = 1
    for a in data: cov[a - BASE] = cov[a - BASE + 1] = 1
    lo, hi = min(code), max(code) + 2
    out = []; a = lo
    while a < hi:
        if not cov[a - BASE]:
            s = a
            while a < hi and not cov[a - BASE]: a += 1
            out.append((s, a))
        else:
            a += 1
    return out


def is_padding(a, b):
    """zero fill or nop/0x0009 fill used for alignment."""
    seg = img[a - BASE:b - BASE]
    return all(x == 0 for x in seg) or all(seg[k:k + 2] in (b"\x09\x00", b"\x00\x00") for k in range(0, len(seg) - 1, 2))


def speculative_ok(a, limit=4000):
    """Linear decode from a: must reach rts/jmp/bra+slot, no illegal ops, no clash with known code/data."""
    pc = a
    for _ in range(limit):
        if not in_img(pc) or pc in data or pc in code: return False
        ins = sh4.decode(h(pc))
        if ins is None: return False
        if ins.kind in ("rts", "jmp", "bra", "rte", "braf"):
            ds = pc + 2
            return in_img(ds) and ds not in data and sh4.decode(h(ds)) is not None
        pc += 2
    return False


def leaf_pass():
    added = 0
    for (s, e) in gaps():
        a = (s + 1) & ~1
        # skip alignment padding
        while a < e and h(a) in (0x0009, 0x0000): a += 2
        if a >= e or a % 4: 
            if a < e and a % 4 == 2 and h(a) in (0x0009,): a += 2
        if a < e and speculative_ok(a):
            seed(a, "leaf"); added += 1
    while queue:
        x, why = queue.popleft()
        if x in funcs: continue
        explore(x)
    return added


if __name__ == "__main__":
    run(0x8C010000)
    run(0x8C05BEF8)
    # extra entry points discovered at runtime (one hex address per line)
    import os
    ef = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../notes/extra_entries.txt")
    if os.path.exists(ef):
        for line in open(ef):
            line = line.split("#")[0].strip()
            if line: run(int(line, 16))
    n1 = len(funcs)
    # pointer pass: any aligned word anywhere in the image that points at a prologue
    for off in range(0, len(img) - 3, 4):
        v = struct.unpack_from("<I", img, off)[0]
        if in_img(v) and v not in funcs and (is_prologue(v) or (
                v % 4 == 0 and v not in code and v not in data and h(v - 2) == 0x0009 and speculative_ok(v))):
            seed(v, "ptr")      # may also be reachable as a branch target inside another function
    while queue:
        a, why = queue.popleft()
        if a in funcs: continue
        explore(a)
    # gap pass: prologues sitting in unclaimed space right after another function
    covered = set(code) | data
    lo = min(code); hi = max(code)
    a = lo
    gap_seeds = 0
    while a < hi:
        if a not in covered and is_prologue(a):
            seed(a, "gap"); gap_seeds += 1
            while queue:
                s, why = queue.popleft()
                if s in funcs: continue
                explore(s)
            covered = set(code) | data
        a += 2
    leaf = 0
    for it in range(6):
        k = leaf_pass(); leaf += k
        if not k: break
    print("functions: descent=%d total=%d (prologue gap seeds %d, leaf seeds %d)" % (n1, len(funcs), gap_seeds, leaf))
    gp = [(a, b) for a, b in gaps() if not is_padding(a, b)]
    print("non-padding uncovered bytes in code range: %d in %d gaps" % (sum(b - a for a, b in gp), len(gp)))
    json.dump([("%08x" % a, b - a) for a, b in gp], open("../notes/gaps.json", "w"))
    print("instructions decoded: %d  (%.2f MB of code)" % (len(code), len(code) * 2 / 1e6))
    print("code range: %08x - %08x" % (lo, max(code) + 2))
    print("literal-pool halfwords: %d" % len(data))
    kinds = collections.Counter(u[2].split()[0] for u in unresolved)
    print("unresolved control flow:", dict(kinds))
    ind = sum(1 for f in funcs.values() for c in f["calls"] if isinstance(c, tuple))
    print("indirect call sites:", ind)
    print("jump tables: %d (%d bounded by cmp/hi), %d case targets" % (len(jumptables), sum(j["bounded"] for j in jumptables.values()), sum(j["n"] for j in jumptables.values())))
    json.dump({"%08x" % k: dict(v, targets=[("%08x" % t) if t else None for t in v["targets"]]) for k, v in jumptables.items()}, open("../notes/jumptables.json", "w"))
    json.dump(dict(funcs={("%08x" % k): dict(lo=v["lo"], hi=v["hi"], insns=v["insns"],
                                            calls=sorted("%08x" % c for c in v["calls"] if isinstance(c, int)),
                                            tail=sorted("%08x" % c for c in v["tail"]))
                         for k, v in sorted(funcs.items())},
                   unresolved=[("%08x" % a, "%08x" % b, c) for a, b, c in unresolved]),
              open("../notes/funcs.json", "w"), indent=0)
