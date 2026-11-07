/* SH-4 CPU state and instruction helpers used by recompiled code. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct sh4ctx {
    uint32_t r[16];
    uint32_t rbank[8];          /* the inactive R0-R7 bank */
    union { float f[16]; uint32_t u[16]; } fr;   /* current FP bank (FPSCR.FR) */
    union { float f[16]; uint32_t u[16]; } xf;   /* other FP bank (XMTRX) */
    uint32_t t, s, q, m_;       /* SR bits kept unpacked */
    uint32_t sr_rest;           /* the other SR bits (MD, RB, BL, IMASK, FD) */
    uint32_t gbr, vbr, ssr, spc, sgr, dbr;
    uint32_t mach, macl, pr;
    uint32_t fpscr, fpul;
} sh4ctx;

#define FPSCR_RM 0x00000003u
#define FPSCR_DN 0x00040000u
#define FPSCR_PR 0x00080000u
#define FPSCR_SZ 0x00100000u
#define FPSCR_FR 0x00200000u

/* ---------------- memory ---------------- */
extern uint8_t *dc_ram;               /* 16 MB main RAM (0x0C000000) */
uint8_t  mmio_read8(uint32_t a);
uint16_t mmio_read16(uint32_t a);
uint32_t mmio_read32(uint32_t a);
void mmio_write8(uint32_t a, uint8_t v);
void mmio_write16(uint32_t a, uint16_t v);
void mmio_write32(uint32_t a, uint32_t v);
void sh4_sq_flush(sh4ctx *c, uint32_t a);

/* Main RAM is 0x0C000000-0x0FFFFFFF physical (16MB, mirrored) in areas P0-P3 */
static inline int is_ram(uint32_t a) { return ((a & 0x1C000000u) == 0x0C000000u) && (a < 0xE0000000u); }
#define RAMP(a) (dc_ram + ((a) & 0x00FFFFFFu))

static inline uint8_t  R8 (uint32_t a) { if (is_ram(a)) return *RAMP(a); return mmio_read8(a); }
static inline uint16_t R16(uint32_t a) { if (is_ram(a)) { uint16_t v; memcpy(&v, RAMP(a), 2); return v; } return mmio_read16(a); }
static inline uint32_t R32(uint32_t a) { if (is_ram(a)) { uint32_t v; memcpy(&v, RAMP(a), 4); return v; } return mmio_read32(a); }
/* writes into the program image are checked against literals the recompiler baked in as constants */
void sh4_code_write(uint32_t a);
#define CODE_LO 0x00010000u
#define CODE_HI 0x000C0000u
#define CHECK_CODE_WRITE(a) do { uint32_t o_ = (a) & 0x00FFFFFFu; if (o_ - CODE_LO < CODE_HI - CODE_LO) sh4_code_write(a); } while (0)
static inline void W8 (uint32_t a, uint8_t  v) { if (is_ram(a)) { CHECK_CODE_WRITE(a); *RAMP(a) = v; return; } mmio_write8(a, v); }
static inline void W16(uint32_t a, uint16_t v) { if (is_ram(a)) { CHECK_CODE_WRITE(a); memcpy(RAMP(a), &v, 2); return; } mmio_write16(a, v); }
static inline void W32(uint32_t a, uint32_t v) { if (is_ram(a)) { CHECK_CODE_WRITE(a); memcpy(RAMP(a), &v, 4); return; } mmio_write32(a, v); }

/* ---------------- dispatch / system ---------------- */
typedef void (*sh4fn)(sh4ctx *);
void sh4_dispatch(sh4ctx *c, uint32_t target);           /* indirect call */
void sh4_trapa(sh4ctx *c, uint32_t imm, uint32_t pc);
void sh4_sleep(sh4ctx *c, uint32_t pc);
void sh4_rte(sh4ctx *c, uint32_t pc);
void sh4_unreachable(sh4ctx *c, uint32_t pc);
void sh4_ldtlb(sh4ctx *c);

static inline uint32_t sh4_get_sr(sh4ctx *c) {
    return c->sr_rest | (c->t & 1) | ((c->s & 1) << 1) | ((c->q & 1) << 8) | ((c->m_ & 1) << 9);
}
void sh4_set_sr(sh4ctx *c, uint32_t v);          /* handles register bank switch */
static inline void sh4_swap_fbanks(sh4ctx *c) {
    uint32_t tmp[16]; memcpy(tmp, c->fr.u, 64); memcpy(c->fr.u, c->xf.u, 64); memcpy(c->xf.u, tmp, 64);
}
static inline void sh4_set_fpscr(sh4ctx *c, uint32_t v) {
    if ((v ^ c->fpscr) & FPSCR_FR) sh4_swap_fbanks(c);
    c->fpscr = v & 0x003FFFFFu;
}

/* ---------------- integer helpers ---------------- */
static inline void sh4_div1(sh4ctx *c, int n, int m) {
    /* Transcribed from the SH-4 programming manual's DIV1 description. */
    uint32_t old_q = c->q, tmp0, tmp1, tmp2 = c->r[m];
    uint32_t rn = c->r[n];
    c->q = (rn >> 31) & 1;
    rn = (rn << 1) | (c->t & 1);
    if (old_q == c->m_) {          /* (Q,M) = (0,0) or (1,1): subtract */
        tmp0 = rn; rn -= tmp2; tmp1 = rn > tmp0;
        if (c->m_ == 0) c->q = c->q ? (tmp1 == 0) : tmp1;
        else            c->q = c->q ? tmp1 : (tmp1 == 0);
    } else {                       /* (0,1) or (1,0): add */
        tmp0 = rn; rn += tmp2; tmp1 = rn < tmp0;
        if (c->m_ == 1) c->q = c->q ? tmp1 : (tmp1 == 0);
        else            c->q = c->q ? (tmp1 == 0) : tmp1;
    }
    c->r[n] = rn;
    c->t = (c->q == c->m_);
}
static inline uint32_t sh4_shad(uint32_t rn, uint32_t rm) {
    int32_t s = (int32_t)rm;
    if (s >= 0) return rn << (s & 31);
    if ((s & 31) == 0) return ((int32_t)rn < 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)rn >> ((~s & 31) + 1));
}
static inline uint32_t sh4_shld(uint32_t rn, uint32_t rm) {
    int32_t s = (int32_t)rm;
    if (s >= 0) return rn << (s & 31);
    if ((s & 31) == 0) return 0;
    return rn >> ((~s & 31) + 1);
}
static inline void sh4_macl(sh4ctx *c, int n, int m) {
    int32_t a = (int32_t)R32(c->r[n]); c->r[n] += 4;
    int32_t b = (int32_t)R32(c->r[m]); c->r[m] += 4;
    int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl);
    acc += (int64_t)a * b;
    if (c->s) { if (acc > 0x00007FFFFFFFFFFFLL) acc = 0x00007FFFFFFFFFFFLL;
                if (acc < -0x0000800000000000LL) acc = -0x0000800000000000LL; }
    c->mach = (uint32_t)((uint64_t)acc >> 32); c->macl = (uint32_t)acc;
}
static inline void sh4_macw(sh4ctx *c, int n, int m) {
    int16_t a = (int16_t)R16(c->r[n]); c->r[n] += 2;
    int16_t b = (int16_t)R16(c->r[m]); c->r[m] += 2;
    if (c->s) {
        int64_t acc = (int32_t)c->macl + (int64_t)a * b;
        if (acc > 0x7FFFFFFF) { acc = 0x7FFFFFFF; c->mach |= 1; }
        if (acc < -0x80000000LL) { acc = -0x80000000LL; c->mach |= 1; }
        c->macl = (uint32_t)acc;
    } else {
        int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + (int64_t)a * b;
        c->mach = (uint32_t)((uint64_t)acc >> 32); c->macl = (uint32_t)acc;
    }
}

/* ---------------- FPU helpers ---------------- */
#define FR(n) (c->fr.f[n])
#define FRU(n) (c->fr.u[n])
static inline double sh4_getdr(sh4ctx *c, int n) {   /* n even */
    uint64_t v = ((uint64_t)c->fr.u[n] << 32) | c->fr.u[n + 1]; double d; memcpy(&d, &v, 8); return d;
}
static inline void sh4_setdr(sh4ctx *c, int n, double d) {
    uint64_t v; memcpy(&v, &d, 8); c->fr.u[n] = (uint32_t)(v >> 32); c->fr.u[n + 1] = (uint32_t)v;
}
#define FPU_PR (c->fpscr & FPSCR_PR)
#define FPU_SZ (c->fpscr & FPSCR_SZ)

/* 64-bit FMOV register selection: even -> DRn (current bank), odd -> XDn (other bank) */
static inline uint32_t *sh4_fpair(sh4ctx *c, int n) { return (n & 1) ? &c->xf.u[n & 14] : &c->fr.u[n & 14]; }

static inline void sh4_fmov_rr(sh4ctx *c, int n, int m) {
    if (FPU_SZ) { uint32_t *d = sh4_fpair(c, n), *s = sh4_fpair(c, m); d[0] = s[0]; d[1] = s[1]; }
    else c->fr.u[n] = c->fr.u[m];
}
static inline void sh4_fmov_load(sh4ctx *c, int n, uint32_t a) {
    if (FPU_SZ) { uint32_t *d = sh4_fpair(c, n); d[0] = R32(a); d[1] = R32(a + 4); }
    else c->fr.u[n] = R32(a);
}
static inline void sh4_fmov_store(sh4ctx *c, int m, uint32_t a) {
    if (FPU_SZ) { uint32_t *s = sh4_fpair(c, m); W32(a, s[0]); W32(a + 4, s[1]); }
    else W32(a, c->fr.u[m]);
}
#define FMOV_SIZE (FPU_SZ ? 8u : 4u)

static inline int32_t sh4_ftrc_f(float f) {
    if (f != f) return (int32_t)0x80000000;
    if (f >= 2147483648.0f) return 0x7FFFFFFF;
    if (f < -2147483648.0f) return (int32_t)0x80000000;
    return (int32_t)f;
}
static inline int32_t sh4_ftrc_d(double f) {
    if (f != f) return (int32_t)0x80000000;
    if (f >= 2147483648.0) return 0x7FFFFFFF;
    if (f < -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)f;
}
static inline void sh4_fipr(sh4ctx *c, int n, int m) {
    float s = c->fr.f[m] * c->fr.f[n] + c->fr.f[m + 1] * c->fr.f[n + 1] + c->fr.f[m + 2] * c->fr.f[n + 2] + c->fr.f[m + 3] * c->fr.f[n + 3];
    c->fr.f[n + 3] = s;
}
static inline void sh4_ftrv(sh4ctx *c, int n) {
    float v0 = c->fr.f[n], v1 = c->fr.f[n + 1], v2 = c->fr.f[n + 2], v3 = c->fr.f[n + 3];
    const float *x = c->xf.f;
    for (int i = 0; i < 4; i++)
        c->fr.f[n + i] = x[i] * v0 + x[i + 4] * v1 + x[i + 8] * v2 + x[i + 12] * v3;
}
static inline void sh4_fsca(sh4ctx *c, int n) {
    double ang = (double)(c->fpul & 0xFFFF) * (2.0 * 3.14159265358979323846 / 65536.0);
    c->fr.f[n] = (float)sin(ang); c->fr.f[n + 1] = (float)cos(ang);
}

/* interrupt polling at loop back-edges */
extern int sh4_poll_ctr;
void sh4_poll_slow(sh4ctx *c);
#define SH4_POLL(c) do { if (--sh4_poll_ctr <= 0) sh4_poll_slow(c); } while (0)

/* set before calling a function to enter it at one of its return addresses (thread resume) */
extern uint32_t sh4_resume_pc;

/* generated function declarations */
#include "funcs.h"
