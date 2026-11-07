"""Attribute functions to source modules via referenced strings (assert __FILE__, library version banners)."""
import re, struct, json, sys, collections
sys.argv = ["x", "../orig/1ST_READ.BIN"]
import runpy
A = runpy.run_path("analyze.py", run_name="__main__")
img, B, code, funcs, func_of = A["img"], A["BASE"], A["code"], A["funcs"], A["func_of"]
import sh4

strs = {}
for m in re.finditer(rb"[\x20-\x7e]{4,}\x00", img):
    strs[B + m.start()] = m.group()[:-1].decode()
src_re = re.compile(r"([A-Za-z0-9_]+\.(c|cpp|cc|h|s|src))\b", re.I)

func_srcs = collections.defaultdict(collections.Counter)
func_strs = collections.defaultdict(list)
for pc, ins in code.items():
    lit = sh4.pcrel_addr(ins, pc)
    if lit is None or (ins.op & 0xF000) != 0xD000: continue
    v = struct.unpack_from("<I", img, lit - B)[0]
    s = strs.get(v)
    if s is None: continue
    f = func_of[pc]
    func_strs[f].append(s)
    for mm in src_re.finditer(s):
        func_srcs[f][mm.group(1)] += 1

# assign module per function, then propagate to unlabeled neighbours inside contiguous runs
entries = sorted(funcs)
label = {}
for f in entries:
    if func_srcs[f]:
        label[f] = func_srcs[f].most_common(1)[0][0]
# fill gaps between two functions with the same label
filled = dict(label)
last = None
for i, f in enumerate(entries):
    if f in label:
        if last is not None and label[entries[last]] == label[f]:
            for g in entries[last + 1:i]: filled.setdefault(g, label[f])
        last = i

runs = []
for f in entries:
    l = filled.get(f, "?")
    if runs and runs[-1][0] == l: runs[-1][2] = f; runs[-1][3] += 1
    else: runs.append([l, f, f, 1])
print("functions with a source-file string:", len(label))
print("distinct source files:", len(set(label.values())))
for l, a, b, n in runs:
    if l != "?" or n > 40:
        print("%-22s %08x-%08x %4d funcs" % (l, a, b, n))
json.dump({"%08x" % f: l for f, l in filled.items()}, open("../notes/modules.json", "w"), indent=0)
json.dump({"%08x" % f: v[:8] for f, v in func_strs.items()}, open("../notes/func_strings.json", "w"), indent=0)
