"""Recover function names: (1) self-identifying error strings, (2) {name*, func*} binding tables."""
import re, struct, json, sys, collections
sys.argv = ["x", "../orig/1ST_READ.BIN"]
import runpy, io, contextlib
with contextlib.redirect_stdout(io.StringIO()):
    A = runpy.run_path("analyze.py", run_name="__main__")
img, B, END, code, funcs, func_of = A["img"], A["BASE"], A["END"], A["code"], A["funcs"], A["func_of"]
import sh4

strs = {}
for m in re.finditer(rb"[\x20-\x7e]{3,}\x00", img):
    strs[B + m.start()] = m.group()[:-1].decode()
ident = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{2,}$")
names = {}; how = {}

# (2) binding tables: aligned word pairs (str*, func*) repeated >= 3 times, either order
words = {}
def w(a): return struct.unpack_from("<I", img, a - B)[0]
tables = []
for off in range(0, len(img) - 8, 4):
    a = B + off
    for stride in (8, 12, 16):
        run = []
        p = a
        while p + 8 <= END:
            s, f = w(p), w(p + 4)
            if s in strs and ident.match(strs[s]) and f in funcs:
                run.append((strs[s], f)); p += stride
            else:
                break
        if len(run) >= 3:
            tables.append((a, stride, run))
            break
# de-overlap: keep tables whose start isn't inside an earlier one
seen = set(); bt = 0
for a, stride, run in tables:
    if a in seen: continue
    for k in range(len(run)): seen.add(a + k * stride)
    bt += 1
    for nm, f in run:
        if f not in names: names[f] = nm; how[f] = "bindtable"

# (1) error/banner prefixes referenced by the function
pref = re.compile(r"^(?:[EW]\d{4}[: ]?\s*)?(_?[A-Za-z][A-Za-z0-9_]{3,})\s*(?:[-:;(]| - | return)")
cand = collections.defaultdict(collections.Counter)
for pc, ins in code.items():
    lit = sh4.pcrel_addr(ins, pc)
    if lit is None or (ins.op & 0xF000) != 0xD000: continue
    s = strs.get(w(lit))
    if not s: continue
    m = pref.match(s)
    if m and not m.group(1).lower().startswith(("warning", "error", "illigal", "illegal", "must", "invalid", "out", "sound", "no", "the")):
        cand[func_of[pc]][m.group(1)] += 1
for f, c in cand.items():
    nm, k = c.most_common(1)[0]
    if f not in names and (len(c) == 1 or k >= 2):
        names[f] = nm; how[f] = "errstr"
# (3) registration calls: string literal and function pointer loaded into arg regs before a jsr/bsr
reg = 0
for f, fd in funcs.items():
    pcs = sorted(p for p in fd["visited"])
    regs = {}
    for pc in pcs:
        ins = code[pc]
        lit = sh4.pcrel_addr(ins, pc)
        if lit is not None and (ins.op & 0xF000) == 0xD000:
            regs[ins.n] = w(lit)
        elif ins.kind in ("jsr", "bsr"):
            # delay slot may also load an arg
            ds = code.get(pc + 2)
            if ds is not None:
                l2 = sh4.pcrel_addr(ds, pc + 2)
                if l2 is not None and (ds.op & 0xF000) == 0xD000: regs[ds.n] = w(l2)
            ss = [regs.get(r) for r in (4, 5, 6, 7)]
            sname = [strs[v] for v in ss if v in strs and ident.match(strs.get(v, ""))]
            fptr = [v for v in ss if v in funcs]
            if len(sname) == 1 and len(fptr) == 1 and fptr[0] not in names:
                names[fptr[0]] = sname[0]; how[fptr[0]] = "register"; reg += 1
            regs = {}
        elif ins.kind:
            regs = {}
print("named from registration calls:", reg)
# drop pure error-code names
for f in [f for f, n in names.items() if re.match(r"^[EW]?\d+$", n) or re.match(r"^[EW]\d{3,}", n)]:
    del names[f]; del how[f]
# duplicates -> suffix
cnt = collections.Counter(names.values()); seen = collections.Counter()
for f in sorted(names):
    nm = names[f]
    if cnt[nm] > 1:
        seen[nm] += 1; names[f] = "%s_%d" % (nm, seen[nm])
print("binding tables:", bt, " named from tables:", sum(1 for v in how.values() if v == "bindtable"))
print("named from error strings:", sum(1 for v in how.values() if v == "errstr"))
print("total named: %d / %d functions" % (len(names), len(funcs)))
json.dump({"%08x" % f: n for f, n in sorted(names.items())}, open("../notes/names.json", "w"), indent=0)
for f in sorted(names)[::max(1, len(names) // 40)]:
    print("  %08x %-10s %s" % (f, how[f], names[f]))
