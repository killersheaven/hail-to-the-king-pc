/* ARM7 interpreter for the AICA's sound CPU.
 *
 * ARM state only (the AICA's ARM7DI has no Thumb). Implements the ARMv4 ARM
 * instruction set minus coprocessors; the ARMv4 extras (halfword transfers,
 * long multiplies, BX) are harmless if unused. Written from the ARM
 * Architecture Reference Manual; no emulator source was used.
 *
 * r15 convention: `pc` is the address of the instruction being executed;
 * reads of r15 inside an instruction see pc + 8 (pc + 12 for register-specified
 * shifts and STR/STM of r15), as on the real core.
 */
#include "arm7.h"
#include <string.h>
#include <stdio.h>

arm7_t arm;

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };
#define F_N 0x80000000u
#define F_Z 0x40000000u
#define F_C 0x20000000u
#define F_V 0x10000000u
#define F_I 0x80u
#define F_F 0x40u

static int bank_of(uint32_t mode) {
    switch (mode & 0x1F) {
    case M_FIQ: return 1; case M_IRQ: return 2; case M_SVC: return 3; case M_ABT: return 4; case M_UND: return 5;
    default: return 0;
    }
}
static void set_mode(uint32_t newcpsr) {
    int ob = bank_of(arm.cpsr), nb = bank_of(newcpsr);
    if (ob != nb) {
        arm.b13[ob] = arm.r[13]; arm.b14[ob] = arm.r[14];
        if (ob == 1) { memcpy(arm.fiq8, arm.r + 8, 20); memcpy(arm.r + 8, arm.usr8, 20); }
        if (nb == 1) { memcpy(arm.usr8, arm.r + 8, 20); memcpy(arm.r + 8, arm.fiq8, 20); }
        arm.r[13] = arm.b13[nb]; arm.r[14] = arm.b14[nb];
    }
    arm.cpsr = newcpsr;
}
static uint32_t *spsr_p(void) { int b = bank_of(arm.cpsr); return b ? &arm.spsr[b] : NULL; }

void arm7_reset(void) {
    memset(&arm, 0, sizeof arm);
    arm.cpsr = M_SVC | F_I | F_F;
    arm.pc = 0;
}

static void exception(uint32_t mode, uint32_t vec, uint32_t ret) {
    uint32_t old = arm.cpsr;
    set_mode((old & ~0x1Fu) | mode | F_I | (mode == M_FIQ ? F_F : 0));
    arm.spsr[bank_of(mode)] = old;
    arm.r[14] = ret;
    arm.pc = vec;
}

static inline int cond_ok(uint32_t c) {
    uint32_t f = arm.cpsr;
    int N = !!(f & F_N), Z = !!(f & F_Z), C = !!(f & F_C), V = !!(f & F_V);
    switch (c) {
    case 0: return Z; case 1: return !Z; case 2: return C; case 3: return !C;
    case 4: return N; case 5: return !N; case 6: return V; case 7: return !V;
    case 8: return C && !Z; case 9: return !C || Z; case 10: return N == V; case 11: return N != V;
    case 12: return !Z && N == V; case 13: return Z || N != V; case 14: return 1;
    default: return 0;
    }
}
static inline uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* value of register r as an operand (r15 reads ahead) */
static inline uint32_t rget(int r, uint32_t ahead) { return r == 15 ? arm.pc + ahead : arm.r[r]; }

/* barrel shifter for data-processing operand 2 (register form) */
static uint32_t shift_op(uint32_t op, int *carry) {
    int rm = op & 15, type = (op >> 5) & 3;
    uint32_t v, n; int c = !!(arm.cpsr & F_C);
    if (op & 0x10) {                                   /* shift by register */
        v = rget(rm, 12); n = arm.r[(op >> 8) & 15] & 0xFF;
        if (n == 0) { *carry = c; return v; }
        switch (type) {
        case 0: if (n < 32) { c = (v >> (32 - n)) & 1; v <<= n; } else if (n == 32) { c = v & 1; v = 0; } else { c = 0; v = 0; } break;
        case 1: if (n < 32) { c = (v >> (n - 1)) & 1; v >>= n; } else if (n == 32) { c = v >> 31; v = 0; } else { c = 0; v = 0; } break;
        case 2: if (n < 32) { c = ((int32_t)v >> (n - 1)) & 1; v = (uint32_t)((int32_t)v >> n); } else { c = v >> 31; v = c ? 0xFFFFFFFFu : 0; } break;
        default: if ((n & 31) == 0) { c = v >> 31; } else { c = (v >> ((n & 31) - 1)) & 1; v = ror32(v, n); } break;
        }
    } else {                                           /* shift by immediate */
        v = rget(rm, 8); n = (op >> 7) & 31;
        switch (type) {
        case 0: if (n) { c = (v >> (32 - n)) & 1; v <<= n; } break;
        case 1: if (n) { c = (v >> (n - 1)) & 1; v >>= n; } else { c = v >> 31; v = 0; } break;
        case 2: if (n) { c = ((int32_t)v >> (n - 1)) & 1; v = (uint32_t)((int32_t)v >> n); } else { c = v >> 31; v = c ? 0xFFFFFFFFu : 0; } break;
        default: if (n) { c = (v >> (n - 1)) & 1; v = ror32(v, n); } else { int nc = v & 1; v = (v >> 1) | ((uint32_t)c << 31); c = nc; } break;
        }
    }
    *carry = c; return v;
}

static inline void set_nz(uint32_t r) { arm.cpsr = (arm.cpsr & ~(F_N | F_Z)) | (r & F_N) | (r ? 0 : F_Z); }
static inline void set_c(int c) { arm.cpsr = c ? arm.cpsr | F_C : arm.cpsr & ~F_C; }
static inline void set_v(int v) { arm.cpsr = v ? arm.cpsr | F_V : arm.cpsr & ~F_V; }

static void write_pc_from_dp(uint32_t v, int s) {
    if (s) { uint32_t *sp = spsr_p(); if (sp) set_mode(*sp); }
    arm.pc = v & ~3u; arm.branched = 1;
}

static void data_proc(uint32_t op) {
    int opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t b; int sc;
    if (op & 0x02000000u) {
        uint32_t rot = ((op >> 8) & 15) * 2; b = ror32(op & 0xFF, rot);
        sc = rot ? (int)(b >> 31) : !!(arm.cpsr & F_C);
    } else b = shift_op(op, &sc);
    uint32_t a = rget(rn, (op & 0x02000010u) == 0x10 ? 12 : 8), r = 0;
    int C = !!(arm.cpsr & F_C);
    uint64_t w;
    int logical = 0, write = 1;
    switch (opc) {
    case 0x0: r = a & b; logical = 1; break;
    case 0x1: r = a ^ b; logical = 1; break;
    case 0x2: r = a - b; if (s && rd != 15) { set_c(a >= b); set_v(((a ^ b) & (a ^ r)) >> 31); } break;
    case 0x3: r = b - a; if (s && rd != 15) { set_c(b >= a); set_v(((b ^ a) & (b ^ r)) >> 31); } break;
    case 0x4: w = (uint64_t)a + b; r = (uint32_t)w; if (s && rd != 15) { set_c(w >> 32); set_v((~(a ^ b) & (a ^ r)) >> 31); } break;
    case 0x5: w = (uint64_t)a + b + C; r = (uint32_t)w; if (s && rd != 15) { set_c(w >> 32); set_v((~(a ^ b) & (a ^ r)) >> 31); } break;
    case 0x6: w = (uint64_t)a - b - !C; r = (uint32_t)w; if (s && rd != 15) { set_c((uint64_t)a >= (uint64_t)b + !C); set_v(((a ^ b) & (a ^ r)) >> 31); } break;
    case 0x7: w = (uint64_t)b - a - !C; r = (uint32_t)w; if (s && rd != 15) { set_c((uint64_t)b >= (uint64_t)a + !C); set_v(((b ^ a) & (b ^ r)) >> 31); } break;
    case 0x8: r = a & b; logical = 1; write = 0; break;
    case 0x9: r = a ^ b; logical = 1; write = 0; break;
    case 0xA: r = a - b; write = 0; set_c(a >= b); set_v(((a ^ b) & (a ^ r)) >> 31); break;
    case 0xB: w = (uint64_t)a + b; r = (uint32_t)w; write = 0; set_c(w >> 32); set_v((~(a ^ b) & (a ^ r)) >> 31); break;
    case 0xC: r = a | b; logical = 1; break;
    case 0xD: r = b; logical = 1; break;
    case 0xE: r = a & ~b; logical = 1; break;
    case 0xF: r = ~b; logical = 1; break;
    }
    if (s && (rd != 15 || !write)) { set_nz(r); if (logical) set_c(sc); }
    if (write) {
        if (rd == 15) write_pc_from_dp(r, s);
        else arm.r[rd] = r;
    }
}

static void psr_transfer(uint32_t op) {
    int spsr = (op >> 22) & 1;
    if (!(op & 0x00200000u)) {                         /* MRS */
        uint32_t *sp = spsr_p();
        arm.r[(op >> 12) & 15] = spsr ? (sp ? *sp : arm.cpsr) : arm.cpsr;
        return;
    }
    uint32_t v = (op & 0x02000000u) ? ror32(op & 0xFF, ((op >> 8) & 15) * 2) : arm.r[op & 15];
    uint32_t mask = 0;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 19)) mask |= 0xFF000000u;
    if (spsr) { uint32_t *sp = spsr_p(); if (sp) *sp = (*sp & ~mask) | (v & mask); return; }
    if ((arm.cpsr & 0x1F) == M_USR) mask &= 0xFF000000u;
    uint32_t n = (arm.cpsr & ~mask) | (v & mask);
    set_mode(n | 0x10);
}

static void single_transfer(uint32_t op) {
    int P = (op >> 24) & 1, U = (op >> 23) & 1, B = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off;
    if (op & 0x02000000u) { int c; off = shift_op(op & ~0x10u, &c); } else off = op & 0xFFF;
    uint32_t base = rget(rn, 8);
    uint32_t addr = P ? (U ? base + off : base - off) : base;
    uint32_t wb = U ? base + off : base - off;
    if (L) {
        uint32_t v = B ? arm7_read8(addr) : ror32(arm7_read32(addr & ~3u), (addr & 3) * 8);
        if (!P || W) arm.r[rn] = wb;
        if (rd == 15) { arm.pc = v & ~3u; arm.branched = 1; } else arm.r[rd] = v;
    } else {
        uint32_t v = rget(rd, 12);
        if (B) arm7_write8(addr, (uint8_t)v); else arm7_write32(addr & ~3u, v);
        if (!P || W) arm.r[rn] = wb;
    }
}

static void half_transfer(uint32_t op) {
    int P = (op >> 24) & 1, U = (op >> 23) & 1, I = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    uint32_t off = I ? ((op >> 4) & 0xF0) | (op & 0xF) : arm.r[op & 15];
    uint32_t base = rget(rn, 8);
    uint32_t addr = P ? (U ? base + off : base - off) : base, wb = U ? base + off : base - off;
    if (L) {
        uint32_t v;
        if (sh == 1) v = arm7_read16(addr & ~1u);
        else if (sh == 2) v = (uint32_t)(int32_t)(int8_t)arm7_read8(addr);
        else v = (uint32_t)(int32_t)(int16_t)arm7_read16(addr & ~1u);
        if (!P || W) arm.r[rn] = wb;
        if (rd == 15) { arm.pc = v & ~3u; arm.branched = 1; } else arm.r[rd] = v;
    } else {
        arm7_write16(addr & ~1u, (uint16_t)rget(rd, 12));
        if (!P || W) arm.r[rn] = wb;
    }
}

static void block_transfer(uint32_t op) {
    int P = (op >> 24) & 1, U = (op >> 23) & 1, S = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
    int rn = (op >> 16) & 15;
    uint32_t list = op & 0xFFFF;
    int n = 0; for (int i = 0; i < 16; i++) n += (list >> i) & 1;
    uint32_t base = arm.r[rn];
    uint32_t start = U ? (P ? base + 4 : base) : (P ? base - 4u * n : base - 4u * n + 4);
    uint32_t wb = U ? base + 4u * n : base - 4u * n;
    int user_bank = S && !(L && (list & 0x8000));
    uint32_t mode = arm.cpsr;
    if (user_bank) set_mode((arm.cpsr & ~0x1Fu) | M_USR);
    uint32_t a = start;
    if (L) {
        if (W) arm.r[rn] = wb;
        for (int i = 0; i < 16; i++) if (list >> i & 1) {
            uint32_t v = arm7_read32(a & ~3u); a += 4;
            if (i == 15) { arm.pc = v & ~3u; arm.branched = 1; } else arm.r[i] = v;
        }
        if (user_bank) set_mode(mode);
        if (S && (list & 0x8000)) { uint32_t *sp = spsr_p(); if (sp) set_mode(*sp); }
    } else {
        for (int i = 0; i < 16; i++) if (list >> i & 1) { arm7_write32(a & ~3u, i == 15 ? arm.pc + 12 : arm.r[i]); a += 4; }
        if (user_bank) set_mode(mode);
        if (W) arm.r[rn] = wb;
    }
}

uint64_t arm7_insns;
static void step(void) {
    if (arm.fiq_line && !(arm.cpsr & F_F)) { exception(M_FIQ, 0x1C, arm.pc + 4); }
    uint32_t op = arm7_read32(arm.pc & ~3u);
    arm.branched = 0;
    arm7_insns++;
    if (cond_ok(op >> 28)) {
        switch ((op >> 25) & 7) {
        case 0:
            if ((op & 0x0FFFFFF0u) == 0x012FFF10u) { uint32_t t = arm.r[op & 15]; arm.pc = t & ~3u; arm.branched = 1; break; }   /* BX */
            if ((op & 0x0FC000F0u) == 0x00000090u) {                          /* MUL / MLA */
                int rd = (op >> 16) & 15;
                uint32_t r = arm.r[op & 15] * arm.r[(op >> 8) & 15];
                if (op & (1u << 21)) r += arm.r[(op >> 12) & 15];
                arm.r[rd] = r; if (op & (1u << 20)) set_nz(r);
                break;
            }
            if ((op & 0x0F8000F0u) == 0x00800090u) {                          /* UMULL / UMLAL / SMULL / SMLAL */
                int hi = (op >> 16) & 15, lo = (op >> 12) & 15;
                uint64_t r = (op & (1u << 22)) ? (uint64_t)((int64_t)(int32_t)arm.r[op & 15] * (int32_t)arm.r[(op >> 8) & 15])
                                               : (uint64_t)arm.r[op & 15] * arm.r[(op >> 8) & 15];
                if (op & (1u << 21)) r += ((uint64_t)arm.r[hi] << 32) | arm.r[lo];
                arm.r[lo] = (uint32_t)r; arm.r[hi] = (uint32_t)(r >> 32);
                if (op & (1u << 20)) arm.cpsr = (arm.cpsr & ~(F_N | F_Z)) | ((uint32_t)(r >> 32) & F_N) | (r ? 0 : F_Z);
                break;
            }
            if ((op & 0x0FB00FF0u) == 0x01000090u) {                          /* SWP / SWPB */
                uint32_t a = arm.r[(op >> 16) & 15], src = arm.r[op & 15], v;
                if (op & (1u << 22)) { v = arm7_read8(a); arm7_write8(a, (uint8_t)src); }
                else { v = ror32(arm7_read32(a & ~3u), (a & 3) * 8); arm7_write32(a & ~3u, src); }
                arm.r[(op >> 12) & 15] = v;
                break;
            }
            if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60)) { half_transfer(op); break; }
            if ((op & 0x0D900000u) == 0x01000000u) { psr_transfer(op); break; }   /* MRS/MSR: TST/TEQ/CMP/CMN without S */
            data_proc(op);
            break;
        case 1:
            if ((op & 0x0D900000u) == 0x01000000u) { psr_transfer(op); break; }   /* MSR immediate */
            data_proc(op);
            break;
        case 2: case 3:
            if ((op & 0x02000010u) == 0x02000010u) { exception(M_UND, 0x04, arm.pc + 4); arm.branched = 1; break; }
            single_transfer(op);
            break;
        case 4: block_transfer(op); break;
        case 5: {
            int32_t off = (int32_t)(op << 8) >> 6;
            if (op & (1u << 24)) arm.r[14] = arm.pc + 4;
            arm.pc = arm.pc + 8 + off; arm.branched = 1;
            break;
        }
        case 6: break;                                                       /* coprocessor transfers: none on the AICA */
        case 7:
            if (op & (1u << 24)) { exception(M_SVC, 0x08, arm.pc + 4); arm.branched = 1; }
            break;
        }
    }
    if (!arm.branched) arm.pc += 4;
}

void arm7_run(int n) {
    for (int i = 0; i < n; i++) step();
}
