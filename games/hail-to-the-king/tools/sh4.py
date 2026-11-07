"""SH-4 (SH7750 / Dreamcast) instruction decoder.

decode(op) -> Insn or None. Operand extraction is driven by a format code:
  0     no operands
  n     Rn = bits 8-11
  m     Rm = bits 8-11 (single-register "m" forms like JMP @Rm, LDS Rm,x)
  nm    Rn = 8-11, Rm = 4-7
  md    MOV.x @(disp,Rm),R0   m = 4-7, d = 0-3
  nd4   MOV.x R0,@(disp,Rn)   n = 4-7, d = 0-3
  nmd   Rn = 8-11, Rm = 4-7, d = 0-3
  d     8-bit displacement
  d12   12-bit displacement
  nd8   Rn = 8-11, d = 0-7
  i     8-bit immediate
  ni    Rn = 8-11, i = 0-7 (signed)
  bank  Rn = 8-11, bank reg = 4-6
"""
from dataclasses import dataclass


@dataclass
class Insn:
    op: int
    mn: str
    fmt: str
    n: int = 0
    m: int = 0
    d: int = 0
    i: int = 0
    # control flow
    kind: str = ""       # '', 'bra','bsr','braf','bsrf','jmp','jsr','rts','rte','bt','bf','bts','bfs','trapa'
    delay: bool = False  # has a delay slot

    def text(self, pc=None):
        return fmt_insn(self, pc)


def s8(x):  return x - 0x100 if x & 0x80 else x
def s12(x): return x - 0x1000 if x & 0x800 else x


# (mask, match, mnemonic, format, kind, delay)
T = []
def I(mask, match, mn, fmt="0", kind="", delay=False):
    T.append((mask, match, mn, fmt, kind, delay))

# --- no operand
for v, mn in [(0x0008, "clrt"), (0x0009, "nop"), (0x0018, "sett"), (0x0019, "div0u"),
              (0x001B, "sleep"), (0x0028, "clrmac"), (0x0038, "ldtlb"), (0x0048, "clrs"),
              (0x0058, "sets"), (0xF3FD, "fschg"), (0xFBFD, "frchg")]:
    I(0xFFFF, v, mn)
I(0xFFFF, 0x000B, "rts", kind="rts", delay=True)
I(0xFFFF, 0x002B, "rte", kind="rte", delay=True)

# --- 0nXX single register
for v, mn in [(0x0002, "stc sr,{n}"), (0x0012, "stc gbr,{n}"), (0x0022, "stc vbr,{n}"),
              (0x0032, "stc ssr,{n}"), (0x0042, "stc spc,{n}"), (0x0029, "movt {n}"),
              (0x000A, "sts mach,{n}"), (0x001A, "sts macl,{n}"), (0x002A, "sts pr,{n}"),
              (0x003A, "stc sgr,{n}"), (0x005A, "sts fpul,{n}"), (0x006A, "sts fpscr,{n}"),
              (0x00FA, "stc dbr,{n}"), (0x0083, "pref @{n}"), (0x0093, "ocbi @{n}"),
              (0x00A3, "ocbp @{n}"), (0x00B3, "ocbwb @{n}"), (0x00C3, "movca.l r0,@{n}")]:
    I(0xF0FF, v, mn, "n")
I(0xF0FF, 0x0003, "bsrf {n}", "n", "bsrf", True)
I(0xF0FF, 0x0023, "braf {n}", "n", "braf", True)
I(0xF08F, 0x0082, "stc {bank},{n}", "bank")

# --- 0nm4.. indexed
for lo, mn in [(4, "mov.b {m},@(r0,{n})"), (5, "mov.w {m},@(r0,{n})"), (6, "mov.l {m},@(r0,{n})"),
               (7, "mul.l {m},{n}"), (0xC, "mov.b @(r0,{m}),{n}"), (0xD, "mov.w @(r0,{m}),{n}"),
               (0xE, "mov.l @(r0,{m}),{n}"), (0xF, "mac.l @{m}+,@{n}+")]:
    I(0xF00F, lo, mn, "nm")

I(0xF000, 0x1000, "mov.l {m},@({d4},{n})", "nmd")

for lo, mn in enumerate(["mov.b {m},@{n}", "mov.w {m},@{n}", "mov.l {m},@{n}", None,
                         "mov.b {m},@-{n}", "mov.w {m},@-{n}", "mov.l {m},@-{n}", "div0s {m},{n}",
                         "tst {m},{n}", "and {m},{n}", "xor {m},{n}", "or {m},{n}",
                         "cmp/str {m},{n}", "xtrct {m},{n}", "mulu.w {m},{n}", "muls.w {m},{n}"]):
    if mn: I(0xF00F, 0x2000 | lo, mn, "nm")

for lo, mn in enumerate(["cmp/eq {m},{n}", None, "cmp/hs {m},{n}", "cmp/ge {m},{n}",
                         "div1 {m},{n}", "dmulu.l {m},{n}", "cmp/hi {m},{n}", "cmp/gt {m},{n}",
                         "sub {m},{n}", None, "subc {m},{n}", "subv {m},{n}",
                         "add {m},{n}", "dmuls.l {m},{n}", "addc {m},{n}", "addv {m},{n}"]):
    if mn: I(0xF00F, 0x3000 | lo, mn, "nm")

for v, mn in [(0x4000, "shll {n}"), (0x4001, "shlr {n}"), (0x4002, "sts.l mach,@-{n}"),
              (0x4003, "stc.l sr,@-{n}"), (0x4004, "rotl {n}"), (0x4005, "rotr {n}"),
              (0x4006, "lds.l @{n}+,mach"), (0x4007, "ldc.l @{n}+,sr"), (0x4008, "shll2 {n}"),
              (0x4009, "shlr2 {n}"), (0x400A, "lds {n},mach"), (0x400E, "ldc {n},sr"),
              (0x4010, "dt {n}"), (0x4011, "cmp/pz {n}"), (0x4012, "sts.l macl,@-{n}"),
              (0x4013, "stc.l gbr,@-{n}"), (0x4015, "cmp/pl {n}"), (0x4016, "lds.l @{n}+,macl"),
              (0x4017, "ldc.l @{n}+,gbr"), (0x4018, "shll8 {n}"), (0x4019, "shlr8 {n}"),
              (0x401A, "lds {n},macl"), (0x401B, "tas.b @{n}"), (0x401E, "ldc {n},gbr"),
              (0x4020, "shal {n}"), (0x4021, "shar {n}"), (0x4022, "sts.l pr,@-{n}"),
              (0x4023, "stc.l vbr,@-{n}"), (0x4024, "rotcl {n}"), (0x4025, "rotcr {n}"),
              (0x4026, "lds.l @{n}+,pr"), (0x4027, "ldc.l @{n}+,vbr"), (0x4028, "shll16 {n}"),
              (0x4029, "shlr16 {n}"), (0x402A, "lds {n},pr"), (0x402E, "ldc {n},vbr"),
              (0x4032, "stc.l sgr,@-{n}"), (0x4033, "stc.l ssr,@-{n}"), (0x4037, "ldc.l @{n}+,ssr"),
              (0x403E, "ldc {n},ssr"), (0x4043, "stc.l spc,@-{n}"), (0x4047, "ldc.l @{n}+,spc"),
              (0x404E, "ldc {n},spc"), (0x4052, "sts.l fpul,@-{n}"), (0x4056, "lds.l @{n}+,fpul"),
              (0x405A, "lds {n},fpul"), (0x4062, "sts.l fpscr,@-{n}"), (0x4066, "lds.l @{n}+,fpscr"),
              (0x406A, "lds {n},fpscr"), (0x40F2, "stc.l dbr,@-{n}"), (0x40F6, "ldc.l @{n}+,dbr"),
              (0x40FA, "ldc {n},dbr")]:
    I(0xF0FF, v, mn, "n")
I(0xF0FF, 0x400B, "jsr @{n}", "n", "jsr", True)
I(0xF0FF, 0x402B, "jmp @{n}", "n", "jmp", True)
I(0xF08F, 0x4083, "stc.l {bank},@-{n}", "bank")
I(0xF08F, 0x4087, "ldc.l @{n}+,{bank}", "bank")
I(0xF08F, 0x408E, "ldc {n},{bank}", "bank")
I(0xF00F, 0x400C, "shad {m},{n}", "nm")
I(0xF00F, 0x400D, "shld {m},{n}", "nm")
I(0xF00F, 0x400F, "mac.w @{m}+,@{n}+", "nm")

I(0xF000, 0x5000, "mov.l @({d4},{m}),{n}", "nmd")

for lo, mn in enumerate(["mov.b @{m},{n}", "mov.w @{m},{n}", "mov.l @{m},{n}", "mov {m},{n}",
                         "mov.b @{m}+,{n}", "mov.w @{m}+,{n}", "mov.l @{m}+,{n}", "not {m},{n}",
                         "swap.b {m},{n}", "swap.w {m},{n}", "negc {m},{n}", "neg {m},{n}",
                         "extu.b {m},{n}", "extu.w {m},{n}", "exts.b {m},{n}", "exts.w {m},{n}"]):
    I(0xF00F, 0x6000 | lo, mn, "nm")

I(0xF000, 0x7000, "add #{i},{n}", "ni")

I(0xFF00, 0x8000, "mov.b r0,@({d1},{n})", "nd4")
I(0xFF00, 0x8100, "mov.w r0,@({d2},{n})", "nd4")
I(0xFF00, 0x8400, "mov.b @({d1},{m}),r0", "md")
I(0xFF00, 0x8500, "mov.w @({d2},{m}),r0", "md")
I(0xFF00, 0x8800, "cmp/eq #{i},r0", "i")
I(0xFF00, 0x8900, "bt {tgt}", "d", "bt")
I(0xFF00, 0x8B00, "bf {tgt}", "d", "bf")
I(0xFF00, 0x8D00, "bt/s {tgt}", "d", "bts", True)
I(0xFF00, 0x8F00, "bf/s {tgt}", "d", "bfs", True)

I(0xF000, 0x9000, "mov.w @({pcw}),{n}", "nd8")
I(0xF000, 0xA000, "bra {tgt}", "d12", "bra", True)
I(0xF000, 0xB000, "bsr {tgt}", "d12", "bsr", True)

for hi, mn in [(0xC0, "mov.b r0,@({u1},gbr)"), (0xC1, "mov.w r0,@({u2},gbr)"), (0xC2, "mov.l r0,@({u4},gbr)"),
               (0xC4, "mov.b @({u1},gbr),r0"), (0xC5, "mov.w @({u2},gbr),r0"), (0xC6, "mov.l @({u4},gbr),r0"),
               (0xC7, "mova @({pcl}),r0"), (0xC8, "tst #{u},r0"), (0xC9, "and #{u},r0"),
               (0xCA, "xor #{u},r0"), (0xCB, "or #{u},r0"), (0xCC, "tst.b #{u},@(r0,gbr)"),
               (0xCD, "and.b #{u},@(r0,gbr)"), (0xCE, "xor.b #{u},@(r0,gbr)"), (0xCF, "or.b #{u},@(r0,gbr)")]:
    I(0xFF00, hi << 8, mn, "i")
I(0xFF00, 0xC300, "trapa #{u}", "i", "trapa")

I(0xF000, 0xD000, "mov.l @({pcl}),{n}", "nd8")
I(0xF000, 0xE000, "mov #{i},{n}", "ni")

# --- FPU
for lo, mn in enumerate(["fadd {fm},{fn}", "fsub {fm},{fn}", "fmul {fm},{fn}", "fdiv {fm},{fn}",
                         "fcmp/eq {fm},{fn}", "fcmp/gt {fm},{fn}", "fmov.s @(r0,{m}),{fn}", "fmov.s {fm},@(r0,{n})",
                         "fmov.s @{m},{fn}", "fmov.s @{m}+,{fn}", "fmov.s {fm},@{n}", "fmov.s {fm},@-{n}",
                         "fmov {fm},{fn}", None, "fmac fr0,{fm},{fn}", None]):
    if mn: I(0xF00F, 0xF000 | lo, mn, "nm")
for v, mn in [(0xF00D, "fsts fpul,{fn}"), (0xF01D, "flds {fn},fpul"), (0xF02D, "float fpul,{fn}"),
              (0xF03D, "ftrc {fn},fpul"), (0xF04D, "fneg {fn}"), (0xF05D, "fabs {fn}"),
              (0xF06D, "fsqrt {fn}"), (0xF08D, "fldi0 {fn}"), (0xF09D, "fldi1 {fn}"),
              (0xF0AD, "fcnvsd fpul,{dn}"), (0xF0BD, "fcnvds {dn},fpul")]:
    I(0xF0FF, v, mn, "n")
I(0xF0FF, 0xF0ED, "fipr {fvm},{fvn}", "n")
I(0xF3FF, 0xF1FD, "ftrv xmtrx,{fvn}", "n")
# Undocumented on SH7750 but implemented by the Dreamcast SH7091 and used by Sega's libs
I(0xF0FF, 0xF07D, "fsrra {fn}", "n")
I(0xF1FF, 0xF0FD, "fsca fpul,{dn}", "n")

# Mask specificity: try most specific mask first.
T.sort(key=lambda t: -bin(t[0]).count("1"))
_cache = {}


def decode(op):
    r = _cache.get(op)
    if r is not None or op in _cache:
        return r
    res = None
    for mask, match, mn, fmt, kind, delay in T:
        if op & mask == match:
            res = Insn(op, mn, fmt, kind=kind, delay=delay)
            if fmt in ("n", "nm", "nmd", "nd8", "ni", "bank"):
                res.n = (op >> 8) & 15
            if fmt in ("nm", "nmd"):
                res.m = (op >> 4) & 15
            if fmt == "bank":
                res.m = (op >> 4) & 7
            if fmt in ("nmd", "md", "nd4"):
                res.d = op & 15
            if fmt == "md":
                res.m = (op >> 4) & 15
            if fmt == "nd4":
                res.n = (op >> 4) & 15
            if fmt == "d":
                res.d = s8(op & 0xFF)
            if fmt == "d12":
                res.d = s12(op & 0xFFF)
            if fmt == "nd8":
                res.d = op & 0xFF
            if fmt == "ni":
                res.i = s8(op & 0xFF)
            if fmt == "i":
                res.i = s8(op & 0xFF); res.d = op & 0xFF
            break
    _cache[op] = res
    return res


def branch_target(ins, pc):
    if ins.kind in ("bt", "bf", "bts", "bfs", "bra", "bsr"):
        return pc + 4 + ins.d * 2
    return None


def pcrel_addr(ins, pc):
    """Address referenced by mov.w/mov.l/mova @(disp,PC)."""
    if (ins.op & 0xF000) == 0x9000:
        return pc + 4 + ins.d * 2
    if (ins.op & 0xF000) == 0xD000 or (ins.op & 0xFF00) == 0xC700:
        return ((pc + 4) & ~3) + ins.d * 4
    return None


def fmt_insn(ins, pc=None):
    m = ins.m; n = ins.n
    f = dict(n=f"r{n}", m=f"r{m}", fn=f"fr{n}", fm=f"fr{m}", dn=f"dr{n & 14}",
             fvn=f"fv{(n & 12)}", fvm=f"fv{(n & 3) * 4}", bank=f"r{m}_bank",
             i=ins.i, u=f"0x{ins.d:02x}", d4=ins.d * 4, d1=ins.d, d2=ins.d * 2,
             u1=ins.d, u2=ins.d * 2, u4=ins.d * 4)
    if ins.mn.startswith("ftrv"):
        f["fvn"] = f"fv{(ins.op >> 8) & 12}"
    if ins.mn.startswith("fipr"):
        f["fvn"] = f"fv{(ins.op >> 8) & 12}"; f["fvm"] = f"fv{((ins.op >> 8) & 3) * 4}"
    if pc is not None:
        t = branch_target(ins, pc)
        f["tgt"] = f"0x{t:08x}" if t is not None else "?"
        a = pcrel_addr(ins, pc)
        f["pcw"] = f["pcl"] = f"0x{a:08x}" if a is not None else "?"
    else:
        f["tgt"] = f"pc+{4 + ins.d * 2}"
        f["pcw"] = f"{ins.d * 2},pc"; f["pcl"] = f"{ins.d * 4},pc"
    return ins.mn.format(**f)
