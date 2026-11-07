"""SH-4 -> C static recompiler for Evil Dead: Hail to the King (Dreamcast).

Usage: python3 recomp.py ../orig/1ST_READ.BIN ../build/gen

Each discovered function becomes `void f_XXXXXXXX(sh4ctx *c)`.
Intra-function branches become gotos; calls become C calls; indirect calls go
through sh4_dispatch(). Delay slots are emitted in the order the hardware
observes them (branch condition / target captured first, slot second).
"""
import sys, os, io, json, struct, contextlib, runpy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh4

IMG = sys.argv[1] if len(sys.argv) > 1 else "../orig/1ST_READ.BIN"
OUT = sys.argv[2] if len(sys.argv) > 2 else "../build/gen"
sys.argv = ["analyze.py", IMG]
with contextlib.redirect_stdout(io.StringIO()):
    A = runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "analyze.py"), run_name="__main__")
img, B, END = A["img"], A["BASE"], A["END"]
funcs, jumptables = A["funcs"], A["jumptables"]
h, w = A["h"], A["w"]
try:
    NAMES = {int(k, 16): v for k, v in json.load(open(os.path.join(os.path.dirname(OUT.rstrip("/")) or ".", "../notes/names.json"))).items()}
except Exception:
    try:
        NAMES = {int(k, 16): v for k, v in json.load(open("../notes/names.json")).items()}
    except Exception:
        NAMES = {}


LIVE = set()
_lf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../notes/live_literals.txt")
if os.path.exists(_lf):
    for _l in open(_lf):
        _l = _l.split("#")[0].strip()
        if _l: LIVE.add(int(_l, 16))
BAKED = set()


def fname(a): return "f_%08X" % a
def R(n): return "c->r[%d]" % n
S32 = "(int32_t)"


class Unsupported(Exception): pass


def op_c(ins, pc):
    """C statement(s) for a non-branch instruction."""
    o = ins.op; n, m = ins.n, ins.m; mn = ins.mn.split()[0]; hi = o >> 12; lo = o & 0xF
    rn, rm = R(n), R(m)
    # ---- data movement
    if hi == 0xE: return "%s = 0x%08Xu;" % (rn, ins.i & 0xFFFFFFFF)
    if hi == 0x9:
        if A["in_ovl"](pc) or (sh4.pcrel_addr(ins, pc) & ~3) in LIVE:   # literal modified at runtime: read it live
            return "%s = (uint32_t)(int16_t)R16(0x%08Xu);" % (rn, sh4.pcrel_addr(ins, pc))
        BAKED.add(sh4.pcrel_addr(ins, pc) & ~3)
        v = struct.unpack_from("<h", img, sh4.pcrel_addr(ins, pc) - B)[0]
        return "%s = 0x%08Xu; /* mov.w @pc */" % (rn, v & 0xFFFFFFFF)
    if hi == 0xD:
        a = sh4.pcrel_addr(ins, pc)
        if A["in_ovl"](pc) or a in LIVE:
            return "%s = R32(0x%08Xu); /* live literal */" % (rn, a)
        BAKED.add(a)
        return "%s = 0x%08Xu; /* mov.l @0x%08X */" % (rn, w(a), a)
    if (o & 0xFF00) == 0xC700:
        return "c->r[0] = 0x%08Xu; /* mova */" % sh4.pcrel_addr(ins, pc)
    if hi == 0x6:
        if lo == 0: return "%s = (uint32_t)(int8_t)R8(%s);" % (rn, rm)
        if lo == 1: return "%s = (uint32_t)(int16_t)R16(%s);" % (rn, rm)
        if lo == 2: return "%s = R32(%s);" % (rn, rm)
        if lo == 3: return "%s = %s;" % (rn, rm)
        if lo in (4, 5, 6):
            sz, rd, ext = {4: (1, "R8", "(int8_t)"), 5: (2, "R16", "(int16_t)"), 6: (4, "R32", "")}[lo]
            inc = "" if n == m else " %s += %d;" % (rm, sz)
            return "{ uint32_t v = (uint32_t)%s%s(%s);%s %s = v; }" % (ext, rd, rm, inc, rn)
        if lo == 7: return "%s = ~%s;" % (rn, rm)
        if lo == 8: return "{ uint32_t v = %s; %s = (v & 0xFFFF0000u) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }" % (rm, rn)
        if lo == 9: return "{ uint32_t v = %s; %s = (v >> 16) | (v << 16); }" % (rm, rn)
        if lo == 0xA: return "{ uint32_t t0 = 0 - %s; uint32_t r = t0 - c->t; c->t = (0 < t0) | (t0 < r); %s = r; }" % (rm, rn)
        if lo == 0xB: return "%s = 0 - %s;" % (rn, rm)
        if lo == 0xC: return "%s = %s & 0xFF;" % (rn, rm)
        if lo == 0xD: return "%s = %s & 0xFFFF;" % (rn, rm)
        if lo == 0xE: return "%s = (uint32_t)(int8_t)%s;" % (rn, rm)
        if lo == 0xF: return "%s = (uint32_t)(int16_t)%s;" % (rn, rm)
    if hi == 0x2:
        if lo == 0: return "W8(%s, (uint8_t)%s);" % (rn, rm)
        if lo == 1: return "W16(%s, (uint16_t)%s);" % (rn, rm)
        if lo == 2: return "W32(%s, %s);" % (rn, rm)
        if lo in (4, 5, 6):
            sz, wr, cast = {4: (1, "W8", "(uint8_t)"), 5: (2, "W16", "(uint16_t)"), 6: (4, "W32", "")}[lo]
            return "{ uint32_t v = %s; uint32_t a = %s - %d; %s(a, %sv); %s = a; }" % (rm, rn, sz, wr, cast, rn)
        if lo == 7: return "c->q = %s >> 31; c->m_ = %s >> 31; c->t = c->q ^ c->m_;" % (rn, rm)
        if lo == 8: return "c->t = (%s & %s) == 0;" % (rn, rm)
        if lo == 9: return "%s &= %s;" % (rn, rm)
        if lo == 0xA: return "%s ^= %s;" % (rn, rm)
        if lo == 0xB: return "%s |= %s;" % (rn, rm)
        if lo == 0xC: return ("{ uint32_t x = %s ^ %s; c->t = !(x & 0xFF000000u) || !(x & 0xFF0000u) || !(x & 0xFF00u) || !(x & 0xFFu); }" % (rn, rm))
        if lo == 0xD: return "%s = (%s >> 16) | (%s << 16);" % (rn, rn, rm)
        if lo == 0xE: return "c->macl = (uint32_t)(uint16_t)%s * (uint32_t)(uint16_t)%s;" % (rn, rm)
        if lo == 0xF: return "c->macl = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)%s);" % (rn, rm)
    if hi == 0x1: return "W32(%s + %d, %s);" % (rn, ins.d * 4, rm)
    if hi == 0x5: return "%s = R32(%s + %d);" % (rn, rm, ins.d * 4)
    if hi == 0x0:
        if (o & 0xF00F) in (4, 5, 6):
            return {4: "W8(c->r[0] + %s, (uint8_t)%s);", 5: "W16(c->r[0] + %s, (uint16_t)%s);", 6: "W32(c->r[0] + %s, %s);"}[lo] % (rn, rm)
        if lo == 7 and (o & 0xF00F) == 7: return "c->macl = %s * %s;" % (rn, rm)
        if (o & 0xF00F) in (0xC, 0xD, 0xE):
            return {0xC: "%s = (uint32_t)(int8_t)R8(c->r[0] + %s);", 0xD: "%s = (uint32_t)(int16_t)R16(c->r[0] + %s);",
                    0xE: "%s = R32(c->r[0] + %s);"}[lo] % (rn, rm)
        if (o & 0xF00F) == 0xF: return "sh4_macl(c, %d, %d);" % (n, m)
    if hi == 0x8:
        sub = (o >> 8) & 0xF; d = o & 0xF; rr = R((o >> 4) & 0xF)
        if sub == 0: return "W8(%s + %d, (uint8_t)c->r[0]);" % (rr, d)
        if sub == 1: return "W16(%s + %d, (uint16_t)c->r[0]);" % (rr, d * 2)
        if sub == 4: return "c->r[0] = (uint32_t)(int8_t)R8(%s + %d);" % (rr, d)
        if sub == 5: return "c->r[0] = (uint32_t)(int16_t)R16(%s + %d);" % (rr, d * 2)
        if sub == 8: return "c->t = c->r[0] == 0x%08Xu;" % (ins.i & 0xFFFFFFFF)
    if hi == 0xC:
        sub = (o >> 8) & 0xF; d = o & 0xFF
        tbl = {0: "W8(c->gbr + %d, (uint8_t)c->r[0]);" % d, 1: "W16(c->gbr + %d, (uint16_t)c->r[0]);" % (d * 2),
               2: "W32(c->gbr + %d, c->r[0]);" % (d * 4),
               4: "c->r[0] = (uint32_t)(int8_t)R8(c->gbr + %d);" % d, 5: "c->r[0] = (uint32_t)(int16_t)R16(c->gbr + %d);" % (d * 2),
               6: "c->r[0] = R32(c->gbr + %d);" % (d * 4),
               8: "c->t = (c->r[0] & 0x%02Xu) == 0;" % d, 9: "c->r[0] &= 0x%02Xu;" % d,
               0xA: "c->r[0] ^= 0x%02Xu;" % d, 0xB: "c->r[0] |= 0x%02Xu;" % d,
               0xC: "c->t = (R8(c->gbr + c->r[0]) & 0x%02X) == 0;" % d,
               0xD: "{ uint32_t a = c->gbr + c->r[0]; W8(a, R8(a) & 0x%02X); }" % d,
               0xE: "{ uint32_t a = c->gbr + c->r[0]; W8(a, R8(a) ^ 0x%02X); }" % d,
               0xF: "{ uint32_t a = c->gbr + c->r[0]; W8(a, R8(a) | 0x%02X); }" % d}
        if sub in tbl: return tbl[sub]
        if sub == 3: return "sh4_trapa(c, 0x%02X, 0x%08Xu);" % (d, pc)
    if hi == 0x7: return "%s += 0x%08Xu;" % (rn, ins.i & 0xFFFFFFFF)
    # ---- arithmetic 3nm
    if hi == 0x3:
        t = {0: "c->t = %s == %s;", 2: "c->t = %s >= %s;", 3: "c->t = (int32_t)%s >= (int32_t)%s;",
             6: "c->t = %s > %s;", 7: "c->t = (int32_t)%s > (int32_t)%s;",
             8: "%s -= %s;", 0xC: "%s += %s;"}
        if lo in t: return t[lo] % (rn, rm)
        if lo == 4: return "sh4_div1(c, %d, %d);" % (n, m)
        if lo == 5: return "{ uint64_t p = (uint64_t)%s * %s; c->mach = (uint32_t)(p >> 32); c->macl = (uint32_t)p; }" % (rn, rm)
        if lo == 0xD: return "{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; c->mach = (uint32_t)((uint64_t)p >> 32); c->macl = (uint32_t)p; }" % (rn, rm)
        if lo == 0xA: return "{ uint32_t t1 = %s - %s; uint32_t t0 = %s; %s = t1 - c->t; c->t = (t0 < t1) | (t1 < %s); }" % (rn, rm, rn, rn, rn)
        if lo == 0xE: return "{ uint32_t t1 = %s + %s; uint32_t t0 = %s; %s = t1 + c->t; c->t = (t0 > t1) | (t1 > %s); }" % (rn, rm, rn, rn, rn)
        if lo == 0xB: return "{ int32_t a = (int32_t)%s, b = (int32_t)%s, r = (int32_t)((uint32_t)a - (uint32_t)b); c->t = ((a ^ b) & (a ^ r)) >> 31 & 1; %s = (uint32_t)r; }" % (rn, rm, rn)
        if lo == 0xF: return "{ int32_t a = (int32_t)%s, b = (int32_t)%s, r = (int32_t)((uint32_t)a + (uint32_t)b); c->t = (~(a ^ b) & (a ^ r)) >> 31 & 1; %s = (uint32_t)r; }" % (rn, rm, rn)
    # ---- 4n.. group
    if hi == 0x4:
        if lo == 0xC: return "%s = sh4_shad(%s, %s);" % (rn, rn, rm)
        if lo == 0xD: return "%s = sh4_shld(%s, %s);" % (rn, rn, rm)
        if lo == 0xF: return "sh4_macw(c, %d, %d);" % (n, m)
        x = o & 0xF0FF
        simple = {
            0x4000: "c->t = %s >> 31; %s <<= 1;" % (rn, rn), 0x4020: "c->t = %s >> 31; %s <<= 1;" % (rn, rn),
            0x4001: "c->t = %s & 1; %s >>= 1;" % (rn, rn),
            0x4021: "c->t = %s & 1; %s = (uint32_t)((int32_t)%s >> 1);" % (rn, rn, rn),
            0x4004: "c->t = %s >> 31; %s = (%s << 1) | c->t;" % (rn, rn, rn),
            0x4005: "c->t = %s & 1; %s = (%s >> 1) | (c->t << 31);" % (rn, rn, rn),
            0x4024: "{ uint32_t t0 = c->t; c->t = %s >> 31; %s = (%s << 1) | t0; }" % (rn, rn, rn),
            0x4025: "{ uint32_t t0 = c->t; c->t = %s & 1; %s = (%s >> 1) | (t0 << 31); }" % (rn, rn, rn),
            0x4008: "%s <<= 2;" % rn, 0x4009: "%s >>= 2;" % rn, 0x4018: "%s <<= 8;" % rn, 0x4019: "%s >>= 8;" % rn,
            0x4028: "%s <<= 16;" % rn, 0x4029: "%s >>= 16;" % rn,
            0x4010: "%s -= 1; c->t = %s == 0;" % (rn, rn),
            0x4011: "c->t = (int32_t)%s >= 0;" % rn, 0x4015: "c->t = (int32_t)%s > 0;" % rn,
            0x401B: "{ uint8_t v = R8(%s); c->t = v == 0; W8(%s, v | 0x80); }" % (rn, rn),
            # lds / ldc / sts / stc (register)
            0x400A: "c->mach = %s;" % rn, 0x401A: "c->macl = %s;" % rn, 0x402A: "c->pr = %s;" % rn,
            0x405A: "c->fpul = %s;" % rn, 0x406A: "sh4_set_fpscr(c, %s);" % rn,
            0x400E: "sh4_set_sr(c, %s);" % rn, 0x401E: "c->gbr = %s;" % rn, 0x402E: "c->vbr = %s;" % rn,
            0x403E: "c->ssr = %s;" % rn, 0x404E: "c->spc = %s;" % rn, 0x40FA: "c->dbr = %s;" % rn,
        }
        if x in simple: return simple[x]
        # pre-decrement stores / post-increment loads of control regs
        st = {0x4002: "c->mach", 0x4012: "c->macl", 0x4022: "c->pr", 0x4052: "c->fpul", 0x4062: "c->fpscr",
              0x4003: "sh4_get_sr(c)", 0x4013: "c->gbr", 0x4023: "c->vbr", 0x4033: "c->ssr", 0x4043: "c->spc",
              0x4032: "c->sgr", 0x40F2: "c->dbr"}
        if x in st: return "%s -= 4; W32(%s, %s);" % (rn, rn, st[x])
        ld = {0x4006: "c->mach = v;", 0x4016: "c->macl = v;", 0x4026: "c->pr = v;", 0x4056: "c->fpul = v;",
              0x4066: "sh4_set_fpscr(c, v);", 0x4007: "sh4_set_sr(c, v);", 0x4017: "c->gbr = v;",
              0x4027: "c->vbr = v;", 0x4037: "c->ssr = v;", 0x4047: "c->spc = v;", 0x40F6: "c->dbr = v;"}
        if x in ld: return "{ uint32_t v = R32(%s); %s += 4; %s }" % (rn, rn, ld[x])
        if (o & 0xF08F) == 0x4083: return "%s -= 4; W32(%s, c->rbank[%d]);" % (rn, rn, ins.m)
        if (o & 0xF08F) == 0x4087: return "c->rbank[%d] = R32(%s); %s += 4;" % (ins.m, rn, rn)
        if (o & 0xF08F) == 0x408E: return "c->rbank[%d] = %s;" % (ins.m, rn)
    if hi == 0x0:
        x = o & 0xF0FF
        t = {0x0002: "%s = sh4_get_sr(c);", 0x0012: "%s = c->gbr;", 0x0022: "%s = c->vbr;", 0x0032: "%s = c->ssr;",
             0x0042: "%s = c->spc;", 0x0029: "%s = c->t;", 0x000A: "%s = c->mach;", 0x001A: "%s = c->macl;",
             0x002A: "%s = c->pr;", 0x003A: "%s = c->sgr;", 0x005A: "%s = c->fpul;", 0x006A: "%s = c->fpscr;",
             0x00FA: "%s = c->dbr;"}
        if x in t: return t[x] % rn
        if x == 0x0083: return "if ((%s & 0xFC000000u) == 0xE0000000u) sh4_sq_flush(c, %s);" % (rn, rn)
        if x in (0x0093, 0x00A3, 0x00B3): return "/* %s */" % ins.mn.split()[0]
        if x == 0x00C3: return "W32(%s, c->r[0]); /* movca.l */" % rn
        if (o & 0xF08F) == 0x0082: return "%s = c->rbank[%d];" % (rn, ins.m)
        z = {0x0008: "c->t = 0;", 0x0009: "", 0x0018: "c->t = 1;", 0x0019: "c->m_ = c->q = c->t = 0;",
             0x0028: "c->mach = c->macl = 0;", 0x0048: "c->s = 0;", 0x0058: "c->s = 1;",
             0x001B: "sh4_sleep(c, 0x%08Xu);" % pc, 0x0038: "sh4_ldtlb(c);"}
        if o in z: return z[o]
    # ---- FPU
    if hi == 0xF:
        fn, fm = "FR(%d)" % n, "FR(%d)" % m
        if o == 0xF3FD: return "c->fpscr ^= FPSCR_SZ;"
        if o == 0xFBFD: return "sh4_set_fpscr(c, c->fpscr ^ FPSCR_FR);"
        arith = {0: "+", 1: "-", 2: "*", 3: "/"}
        if lo in arith:
            return ("if (FPU_PR) sh4_setdr(c, %d, sh4_getdr(c, %d) %s sh4_getdr(c, %d)); else %s = %s %s %s;"
                    % (n & 14, n & 14, arith[lo], m & 14, fn, fn, arith[lo], fm))
        if lo == 4: return "c->t = FPU_PR ? (sh4_getdr(c, %d) == sh4_getdr(c, %d)) : (%s == %s);" % (n & 14, m & 14, fn, fm)
        if lo == 5: return "c->t = FPU_PR ? (sh4_getdr(c, %d) > sh4_getdr(c, %d)) : (%s > %s);" % (n & 14, m & 14, fn, fm)
        if lo == 6: return "sh4_fmov_load(c, %d, c->r[0] + %s);" % (n, rm)
        if lo == 7: return "sh4_fmov_store(c, %d, c->r[0] + %s);" % (m, rn)
        if lo == 8: return "sh4_fmov_load(c, %d, %s);" % (n, rm)
        if lo == 9: return "{ uint32_t a = %s; %s += FMOV_SIZE; sh4_fmov_load(c, %d, a); }" % (rm, rm, n)
        if lo == 0xA: return "sh4_fmov_store(c, %d, %s);" % (m, rn)
        if lo == 0xB: return "{ uint32_t a = %s - FMOV_SIZE; sh4_fmov_store(c, %d, a); %s = a; }" % (rn, m, rn)
        if lo == 0xC: return "sh4_fmov_rr(c, %d, %d);" % (n, m)
        if lo == 0xE: return "%s = FR(0) * %s + %s;" % (fn, fm, fn)
        x = o & 0xF0FF
        d = n & 14
        t = {0xF00D: "FRU(%d) = c->fpul;" % n, 0xF01D: "c->fpul = FRU(%d);" % n,
             0xF02D: "if (FPU_PR) sh4_setdr(c, %d, (double)(int32_t)c->fpul); else %s = (float)(int32_t)c->fpul;" % (d, fn),
             0xF03D: "c->fpul = (uint32_t)(FPU_PR ? sh4_ftrc_d(sh4_getdr(c, %d)) : sh4_ftrc_f(%s));" % (d, fn),
             0xF04D: "if (FPU_PR) FRU(%d) ^= 0x80000000u; else FRU(%d) ^= 0x80000000u;" % (d, n),
             0xF05D: "if (FPU_PR) FRU(%d) &= 0x7FFFFFFFu; else FRU(%d) &= 0x7FFFFFFFu;" % (d, n),
             0xF06D: "if (FPU_PR) sh4_setdr(c, %d, sqrt(sh4_getdr(c, %d))); else %s = sqrtf(%s);" % (d, d, fn, fn),
             0xF07D: "%s = 1.0f / sqrtf(%s);" % (fn, fn),
             0xF08D: "FRU(%d) = 0;" % n, 0xF09D: "FRU(%d) = 0x3F800000u;" % n,
             0xF0AD: "{ float f; memcpy(&f, &c->fpul, 4); sh4_setdr(c, %d, (double)f); }" % d,
             0xF0BD: "{ float f = (float)sh4_getdr(c, %d); memcpy(&c->fpul, &f, 4); }" % d,
             0xF0ED: "sh4_fipr(c, %d, %d);" % ((o >> 8) & 12, ((o >> 8) & 3) * 4)}
        if x in t: return t[x]
        if (o & 0xF3FF) == 0xF1FD: return "sh4_ftrv(c, %d);" % ((o >> 8) & 12)
        if (o & 0xF1FF) == 0xF0FD: return "sh4_fsca(c, %d);" % d
    raise Unsupported("%04x %s" % (o, ins.mn))


RESUMES = {}
# guest routines replaced by runtime code (context switches the translator cannot follow)
HOOKS = {0x8C0AF7B4: "hle_ctx_restore"}
def gen_function(entry):
    if entry in HOOKS:
        RESUMES[entry] = []
        return "void %s(sh4ctx *c) { void %s(sh4ctx *); %s(c); }" % (fname(entry), HOOKS[entry], HOOKS[entry]), set()
    f = funcs[entry]; V = f["visited"]
    pcs = sorted(V)
    # delay-slot addresses (emitted inline with their branch)
    slots = {p + 2 for p in pcs if A["code"][p].delay}
    labels = set()
    for p in pcs:
        ins = A["code"][p]
        if ins.kind in ("bt", "bf", "bts", "bfs", "bra"):
            t = sh4.branch_target(ins, p)
            if t in V: labels.add(t)
        if ins.kind == "braf" and p in jumptables:
            labels.update(t for t in jumptables[p]["targets"] if t and t in V)
        if ins.kind == "braf" and p not in jumptables:
            for back in (2, 4, 6, 8):
                pi = A["code"].get(p - back)
                if pi is None or pi.kind: break
                if (pi.op & 0xF000) == 0x9000 and pi.n == ins.n:
                    t = p + 4 + (lambda v: v - 0x10000 if v & 0x8000 else v)(h(sh4.pcrel_addr(pi, p - back)))
                    if t in V: labels.add(t)
                    break
    # resume points: every return address after a call inside this function. A thread that the
    # guest OS resumes mid-function (saved PC = a return address) re-enters here.
    resumes = sorted({p + 4 for p in pcs if A["code"][p].kind in ("bsr", "jsr", "bsrf") and p + 4 in V and p + 4 not in slots})
    labels.update(resumes)
    RESUMES[entry] = resumes
    # long-range constant brafs resolved in analyze become plain jumps: recompute here
    out = []
    E = out.append
    nm = NAMES.get(entry)
    E("/* %08X%s */" % (entry, (" " + nm) if nm else ""))
    E("void %s(sh4ctx *c) {" % fname(entry))
    if resumes:
        E("    if (sh4_resume_pc) { uint32_t r_ = sh4_resume_pc; sh4_resume_pc = 0; switch (r_) {")
        for r in resumes: E("      case 0x%08Xu: goto L_%08X;" % (r, r))
        E("      default: sh4_unreachable(c, r_); return; } }")
    if pcs and pcs[0] != entry:
        # blocks below the entry point (shared tails / backward branches): start at the real entry
        labels.add(entry)
        E("    goto L_%08X;" % entry)
    regconst = {}
    prev_falls = False
    last_end = None
    for p in pcs:
        if p in slots and p not in labels:
            continue
        ins = A["code"][p]
        if last_end is not None and p != last_end and prev_falls:
            E("    goto L_%08X;" % last_end) if last_end in V else E("    sh4_unreachable(c, 0x%08Xu); return;" % last_end)
        if p in labels:
            E("L_%08X:" % p)
            regconst = {}   # another path joins here: register contents are not known
        k = ins.kind
        slot_c = ""
        if ins.delay:
            sp = p + 2
            sins = A["code"].get(sp) or sh4.decode(h(sp))
            if sins is None or sins.kind:
                slot_c = "sh4_unreachable(c, 0x%08Xu); /* illegal slot */" % sp
            else:
                slot_c = op_c(sins, sp)
        nxt = p + (4 if ins.delay else 2)
        falls = True
        if not k:
            s = op_c(ins, p)
            if s: E("    " + s)
            # track constant loads for jsr/jmp targets
            if (ins.op & 0xF000) == 0xD000 and not A["in_ovl"](p) and sh4.pcrel_addr(ins, p) not in LIVE:
                regconst[ins.n] = w(sh4.pcrel_addr(ins, p))
            elif ins.fmt in ("n", "nm", "ni", "nmd", "nd8", "bank"):
                regconst.pop(ins.n, None)
            if (ins.op & 0xF000) == 0x9000 and not A["in_ovl"](p) and (sh4.pcrel_addr(ins, p) & ~3) not in LIVE:
                regconst[ins.n] = ("w", struct.unpack_from("<h", img, sh4.pcrel_addr(ins, p) - B)[0])
        elif k in ("bt", "bf"):
            t = sh4.branch_target(ins, p)
            cond = "c->t" if k == "bt" else "!c->t"
            if t in V: E("    if (%s) {%s goto L_%08X; }" % (cond, " SH4_POLL(c);" if t <= p else "", t))
            elif t in funcs: E("    if (%s) { %s(c); return; }" % (cond, fname(t)))
            else: E("    if (%s) { sh4_dispatch(c, 0x%08Xu); return; }" % (cond, t))
        elif k in ("bts", "bfs"):
            t = sh4.branch_target(ins, p)
            cond = "c->t" if k == "bts" else "!c->t"
            E("    { uint32_t cond = %s; %s" % (cond, slot_c))
            if t in V: E("      if (cond) {%s goto L_%08X; } }" % (" SH4_POLL(c);" if t <= p else "", t))
            elif t in funcs: E("      if (cond) { %s(c); return; } }" % fname(t))
            else: E("      if (cond) { sh4_dispatch(c, 0x%08Xu); return; } }" % t)
        elif k == "bra":
            t = sh4.branch_target(ins, p)
            E("    " + slot_c)
            if t in V and t not in f["tail"]: E("   %s goto L_%08X;" % (" SH4_POLL(c);" if t <= p else "", t))
            elif t in funcs: E("    %s(c); return;" % fname(t))
            else: E("    sh4_dispatch(c, 0x%08Xu); return;" % t)
            falls = False
        elif k == "bsr":
            t = sh4.branch_target(ins, p)
            E("    c->pr = 0x%08Xu; %s" % (p + 4, slot_c))
            E("    %s(c);" % fname(t) if t in funcs else "    sh4_dispatch(c, 0x%08Xu);" % t)
        elif k == "jsr":
            t = regconst.get(ins.n)
            E("    { uint32_t tgt = %s; c->pr = 0x%08Xu; %s" % (R(ins.n), p + 4, slot_c))
            if isinstance(t, int) and t in funcs: E("      %s(c); }" % fname(t))
            else: E("      sh4_dispatch(c, tgt); }")
        elif k == "bsrf":
            E("    { uint32_t tgt = 0x%08Xu + %s; c->pr = 0x%08Xu; %s" % (p + 4, R(ins.n), p + 4, slot_c))
            E("      sh4_dispatch(c, tgt); }")
        elif k == "rts":
            E("    " + slot_c); E("    return;"); falls = False
        elif k == "rte":
            E("    " + slot_c); E("    sh4_rte(c, 0x%08Xu); return;" % p); falls = False
        elif k == "jmp":
            t = regconst.get(ins.n)
            E("    { uint32_t tgt = %s; %s" % (R(ins.n), slot_c))
            if isinstance(t, int) and t in funcs: E("      %s(c); return; }" % fname(t))
            else: E("      sh4_dispatch(c, tgt); return; }")
            falls = False
        elif k == "braf":
            E("    { uint32_t tgt = 0x%08Xu + %s; %s" % (p + 4, R(ins.n), slot_c))
            rc = regconst.get(ins.n)
            if isinstance(rc, tuple) and rc[0] == "w":
                t = (p + 4 + rc[1]) & 0xFFFFFFFF
                E("      goto L_%08X; }" % t) if t in labels else E("      sh4_dispatch(c, tgt); return; }")
            else:
                E("      switch (tgt) {")
                for t in sorted(set(x for x in jumptables.get(p, {}).get("targets", []) if x)):
                    if t in V: E("      case 0x%08Xu: goto L_%08X;" % (t, t))
                E("      default: sh4_dispatch(c, tgt); return; } }")
            falls = False
        elif k == "trapa":
            E("    " + op_c(ins, p))
        if k: regconst = {}
        prev_falls = falls
        last_end = nxt
    if prev_falls:
        E("    sh4_unreachable(c, 0x%08Xu); return;" % last_end)
    E("}")
    return "\n".join(out), labels


def gen_interp(path):
    """Fallback interpreter for code the analysis never found (run by sh4_dispatch).
    Non-branch instructions reuse op_c, so the interpreter and the recompiled code
    share one definition of every instruction; branches, delay slots and
    PC-relative loads are handled in the runtime loop (interp_rt in this file)."""
    cases = []
    for op in range(0x10000):
        ins = sh4.decode(op)
        if ins is None or ins.kind: continue
        if (op & 0xF000) in (0x9000, 0xD000) or (op & 0xFF00) == 0xC700: continue   # PC-relative: runtime
        try: code = op_c(ins, 0x8C010000)
        except Unsupported: continue
        if code is None: continue
        cases.append("    case 0x%04X: %s return 0;" % (op, code))
    src = ['#include "sh4ctx.h"', '/* generated by recomp.py gen_interp: one case per non-branch opcode */',
           'int sh4_interp_exec(sh4ctx *c, uint16_t op) {', '    switch (op) {'] + cases + ['    default: return -1;', '    }', '}']
    open(path, "w").write("\n".join(src) + "\n")
    return len(cases)


def main():
    os.makedirs(OUT, exist_ok=True)
    entries = sorted(funcs)
    errs = []
    # pass 1 to discover late labels (constant braf targets) -> regenerate once
    chunks = []; per_file = 250
    for i in range(0, len(entries), per_file):
        chunks.append(entries[i:i + per_file])
    hdr = ["#pragma once", "struct sh4ctx;"] + ["void %s(struct sh4ctx *c);" % fname(e) for e in entries]
    open(os.path.join(OUT, "funcs.h"), "w").write("\n".join(hdr) + "\n")
    total_lines = 0
    for ci, ch in enumerate(chunks):
        buf = ['#include "sh4ctx.h"', "#pragma GCC diagnostic ignored \"-Wunused-label\"", ""]
        for e in ch:
            try:
                txt, labels = gen_function(e)
                buf.append(txt)
            except Unsupported as ex:
                errs.append((e, str(ex)))
                buf.append("void %s(sh4ctx *c) { sh4_unreachable(c, 0x%08Xu); } /* unsupported: %s */" % (fname(e), e, ex))
        src = "\n\n".join(buf) + "\n"
        total_lines += src.count("\n")
        open(os.path.join(OUT, "funcs_%03d.c" % ci), "w").write(src)
    print("interpreter cases: %d" % gen_interp(os.path.join(OUT, "interp_ops.c")))
    # dispatch table
    d = ['#include "sh4ctx.h"', "#include <stddef.h>", "typedef struct { uint32_t a; sh4fn f; } fnent;",
         "const fnent sh4_fn_table[] = {"]
    d += ["  {0x%08Xu, %s}," % (e, fname(e)) for e in entries]
    d += ["};", "const size_t sh4_fn_count = %d;" % len(entries)]
    open(os.path.join(OUT, "fntable.c"), "w").write("\n".join(d) + "\n")
    # resume table: return address -> function containing it
    rt = {}
    for e in entries:
        for r in RESUMES.get(e, []): rt.setdefault(r, e)
    d = ['#include "sh4ctx.h"', "#include <stddef.h>", "typedef struct { uint32_t a; sh4fn f; } fnent;",
         "const fnent sh4_resume_table[] = {"]
    d += ["  {0x%08Xu, %s}," % (r, fname(rt[r])) for r in sorted(rt)]
    d += ["};", "const size_t sh4_resume_count = %d;" % len(rt)]
    open(os.path.join(OUT, "resume.c"), "w").write("\n".join(d) + "\n")
    # literals baked in as constants: the runtime reports writes to them
    bk = sorted(BAKED)
    lm = ['#include <stdint.h>', '#include <stddef.h>', "const uint32_t sh4_baked_lits[] = {"]
    lm += ["  0x%08Xu," % a for a in bk] + ["};", "const size_t sh4_baked_count = %d;" % len(bk)]
    open(os.path.join(OUT, "litmap.c"), "w").write("\n".join(lm) + "\n")
    print("baked literals: %d, live literals: %d" % (len(bk), len(LIVE)))
    print("functions: %d  files: %d  lines: %d  unsupported: %d" % (len(entries), len(chunks), total_lines, len(errs)))
    for e, x in errs[:20]: print("  %08x %s" % (e, x))


if __name__ == "__main__":
    main()
