/* SH-4 -> C static recompiler: the C version of tools/analyze.py (function
 * discovery) and tools/recomp.py (code generation), so the one-click build can
 * translate the player's program at launch without Python.
 *
 * It follows the Python tools step for step, including the order things are
 * explored in (which decides tail calls and function boundaries), and its
 * output must be identical to recomp.py's: recomp/main.c --check compares them.
 * Comments name the Python function each part comes from.
 */
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ decoder (sh4.py) */
enum { F_0, F_n, F_nm, F_md, F_nd4, F_nmd, F_d, F_d12, F_nd8, F_i, F_ni, F_bank };
enum { K_NONE, K_BRA, K_BSR, K_BRAF, K_BSRF, K_JMP, K_JSR, K_RTS, K_RTE, K_BT, K_BF, K_BTS, K_BFS, K_TRAPA };
typedef struct { uint16_t mask, match; const char *mn; uint8_t fmt, kind, delay; } tent;
static const tent T[] = {
#include "sh4_table.h"
};
typedef struct { uint16_t op; int16_t ok; uint8_t fmt, kind, delay, n, m; int32_t d, i; const char *mn; } insn;
static insn dec[0x10000];
static int dec_ready;

static void decode_init(void) {
    if (dec_ready) return;
    for (int op = 0; op < 0x10000; op++) {
        insn *r = &dec[op]; memset(r, 0, sizeof *r); r->op = (uint16_t)op;
        for (size_t k = 0; k < sizeof T / sizeof T[0]; k++) {
            if ((op & T[k].mask) != T[k].match) continue;
            int f = T[k].fmt;
            r->ok = 1; r->mn = T[k].mn; r->fmt = (uint8_t)f; r->kind = T[k].kind; r->delay = T[k].delay;
            if (f == F_n || f == F_nm || f == F_nmd || f == F_nd8 || f == F_ni || f == F_bank) r->n = (op >> 8) & 15;
            if (f == F_nm || f == F_nmd) r->m = (op >> 4) & 15;
            if (f == F_bank) r->m = (op >> 4) & 7;
            if (f == F_nmd || f == F_md || f == F_nd4) r->d = op & 15;
            if (f == F_md) r->m = (op >> 4) & 15;
            if (f == F_nd4) r->n = (op >> 4) & 15;
            if (f == F_d) r->d = (int8_t)(op & 0xFF);
            if (f == F_d12) r->d = (op & 0x800) ? (op & 0xFFF) - 0x1000 : (op & 0xFFF);
            if (f == F_nd8) r->d = op & 0xFF;
            if (f == F_ni) r->i = (int8_t)(op & 0xFF);
            if (f == F_i) { r->i = (int8_t)(op & 0xFF); r->d = op & 0xFF; }
            break;
        }
    }
    dec_ready = 1;
}
static int is_cond(int k) { return k == K_BT || k == K_BF || k == K_BTS || k == K_BFS; }
static int64_t branch_target(const insn *i, int64_t pc) { return pc + 4 + (int64_t)i->d * 2; }   /* bt/bf/bts/bfs/bra/bsr */
static int has_target(const insn *i) { return is_cond(i->kind) || i->kind == K_BRA || i->kind == K_BSR; }
/* address referenced by mov.w / mov.l / mova @(disp,PC); -1 if none */
static int64_t pcrel_addr(const insn *i, int64_t pc) {
    if ((i->op & 0xF000) == 0x9000) return pc + 4 + (int64_t)i->d * 2;
    if ((i->op & 0xF000) == 0xD000 || (i->op & 0xFF00) == 0xC700) return ((pc + 4) & ~(int64_t)3) + (int64_t)i->d * 4;
    return -1;
}

/* ------------------------------------------------------------------ text buffer */
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t l) {
    if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2 + 4096; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, l); b->n += l; b->p[b->n] = 0;
}
static void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[1024]; va_list ap; va_start(ap, fmt); int l = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (l < (int)sizeof tmp) { sb_put(b, tmp, (size_t)l); return; }
    char *big = malloc((size_t)l + 1); va_start(ap, fmt); vsnprintf(big, (size_t)l + 1, fmt, ap); va_end(ap);
    sb_put(b, big, (size_t)l); free(big);
}
/* one generated line */
static void E(sbuf *b, const char *fmt, ...) {
    char tmp[2048]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (b->n) sb_put(b, "\n", 1);
    sb_puts(b, tmp);
}

/* ------------------------------------------------------------------ program image (analyze.py) */
#define BASE 0x8C010000ull
static const uint8_t *img; static int64_t img_len, END;
static const uint8_t *ovl; static int64_t ovl_len, ovl_base;
static int64_t LO;                  /* lowest address tracked: overlay base or BASE */
static int64_t SPAN;                /* END - LO */
#define IDX(a) ((size_t)((a) - LO))

static int in_ovl(int64_t a) { return ovl && a >= ovl_base && a < ovl_base + ovl_len; }
static int in_img(int64_t a) { return (a >= (int64_t)BASE && a < END) || in_ovl(a); }
static unsigned h(int64_t a) {
    if (a >= (int64_t)BASE && a < END) { size_t o = (size_t)(a - BASE); return img[o] | (o + 1 < (size_t)img_len ? img[o + 1] << 8 : 0); }
    if (in_ovl(a)) { size_t o = (size_t)(a - ovl_base); return ovl[o] | (o + 1 < (size_t)ovl_len ? ovl[o + 1] << 8 : 0); }
    return 0xFFFF;                                  /* outside: illegal opcode */
}
static uint32_t w(int64_t a) {
    const uint8_t *b; size_t o, l;
    if (a >= (int64_t)BASE && a < END) { b = img; o = (size_t)(a - BASE); l = (size_t)img_len; }
    else if (in_ovl(a)) { b = ovl; o = (size_t)(a - ovl_base); l = (size_t)ovl_len; }
    else return 0;
    uint32_t v = 0; for (int k = 3; k >= 0; k--) v = (v << 8) | (o + k < l ? b[o + k] : 0);
    return v;
}
static const insn *D(int64_t a) { const insn *i = &dec[h(a)]; return i->ok ? i : NULL; }
static int32_t sx16(unsigned v) { return (v & 0x8000) ? (int32_t)v - 0x10000 : (int32_t)v; }

/* per-address state (every byte from LO to END) */
enum { A_CODE = 1, A_DATA = 2, A_SEEN = 4 };
static uint8_t *aflag;
static int32_t *func_at;            /* function index + 1 */
static uint32_t *vstamp;            /* visited-in-current-explore stamp */
static uint32_t stamp;
static int64_t code_min, code_max;
static int in_range(int64_t a) { return a >= LO && a < END; }
static int is_code(int64_t a) { return in_range(a) && (aflag[IDX(a)] & A_CODE); }
static int is_data(int64_t a) { return in_range(a) && (aflag[IDX(a)] & A_DATA); }
static void add_data(int64_t a) { if (in_range(a)) aflag[IDX(a)] |= A_DATA; }
static void add_code(int64_t a) {
    if (!in_range(a)) return;
    if (!(aflag[IDX(a)] & A_CODE)) { aflag[IDX(a)] |= A_CODE; if (a < code_min) code_min = a; if (a > code_max) code_max = a; }
}
static int func_of_addr(int64_t a) { return in_range(a) ? func_at[IDX(a)] - 1 : -1; }

typedef struct { int64_t *v; int n, cap; } vec;
static void vpush(vec *x, int64_t a) { if (x->n == x->cap) { x->cap = x->cap * 2 + 16; x->v = realloc(x->v, sizeof *x->v * x->cap); } x->v[x->n++] = a; }
static int vhas(const vec *x, int64_t a) { for (int i = 0; i < x->n; i++) if (x->v[i] == a) return 1; return 0; }
static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }

typedef struct { int64_t entry; vec visited, tail; } func;
static func *F; static int nF, capF;

typedef struct { int64_t pc; vec targets; } jtab;   /* targets: 0 = unused slot */
static jtab *JT; static int nJT, capJT;
static jtab *jt_find(int64_t pc) { for (int i = 0; i < nJT; i++) if (JT[i].pc == pc) return &JT[i]; return NULL; }
static int64_t *jt_index;  /* not used for lookup speed: few tables */

static int64_t *queue; static size_t qhead, qtail, qcap;
static void seed(int64_t a) {
    if (in_img(a) && a % 2 == 0 && !(aflag[IDX(a)] & A_SEEN)) {
        aflag[IDX(a)] |= A_SEEN;
        if (qtail == qcap) { qcap = qcap * 2 + 1024; queue = realloc(queue, sizeof *queue * qcap); }
        queue[qtail++] = a;
    }
}

/* is_prologue: within the first 8 instructions a push of PR or R14 onto r15 appears before any control flow */
static int is_prologue(int64_t a) {
    if (!in_img(a + 16) || a % 2) return 0;
    for (int k = 0; k < 8; k++) {
        unsigned op = h(a + 2 * k); const insn *i = D(a + 2 * k);
        if (!i || i->kind) return 0;
        if (op == 0x4F22 || op == 0x2FE6) return 1;
    }
    return 0;
}

/* resolve_jt: GCC SH switch */
static int resolve_jt(int64_t pc, vec *out) {
    const insn *braf = D(pc); int rx = braf->n;
    int64_t tbl = -1; int esize = 0, unsigned_ = 0, have_limit = 0, idx = -1; int64_t limit = 0, idx_add = 0;
    for (int k1 = 1; k1 < 48; k1++) {
        int64_t a = pc - 2 * k1;
        if (!in_img(a)) break;
        const insn *i = D(a);
        if (!i) break;
        if (i->kind == K_RTS || i->kind == K_RTE || i->kind == K_JMP || i->kind == K_BRAF || i->kind == K_BSR || i->kind == K_JSR) break;
        if (!esize && ((i->op & 0xF00F) == 0x000C || (i->op & 0xF00F) == 0x000D) && i->n == rx) {
            esize = (i->op & 0xF) == 0xC ? 1 : 2; idx = i->m;
        } else if (!esize && (i->op & 0xF00F) == 0x600C && i->n == rx) {
            unsigned_ = 1;
        } else if (esize && tbl < 0 && (i->op & 0xFF00) == 0xC700) {
            tbl = pcrel_addr(i, a);
        } else if (esize && (((i->op & 0xF00F) == 0x300C && i->n == idx && i->m == idx) || i->op == (0x4000 | (idx << 8)))) {
            /* index doubling (add rI,rI / shll rI) */
        } else if (esize && (i->op & 0xF000) == 0x7000 && i->n == idx && !have_limit) {
            idx_add += i->i;
        } else if (esize && (i->op & 0xF00F) == 0x6003 && i->n == idx && !have_limit) {
            idx = i->m;                                 /* mov rI',rI : follow copy */
        } else if (tbl >= 0 && tbl != 0 && !have_limit && ((i->op & 0xF00F) == 0x3006 || (i->op & 0xF00F) == 0x3002) && i->n == idx) {
            int rl = i->m, hs = (i->op & 0xF) == 2;
            for (int k2 = 1; k2 < 48; k2++) {
                int64_t b = a - 2 * k2;
                const insn *j = D(b);
                if (!j || j->kind == K_RTS || j->kind == K_RTE || j->kind == K_JMP || j->kind == K_BRAF || j->kind == K_BRA) break;
                const insn *pj = D(b - 2);               /* an instruction in a bra delay slot belongs to another path */
                if (pj && pj->kind == K_BRA) break;
                if ((j->op & 0xF000) == 0xE000 && j->n == rl) { limit = j->i - (hs ? 1 : 0); have_limit = 1; break; }
                if ((j->op & 0xF000) == 0x9000 && j->n == rl) { limit = sx16(h(pcrel_addr(j, b))) - (hs ? 1 : 0); have_limit = 1; break; }
                if ((j->fmt == F_nm || j->fmt == F_n || j->fmt == F_ni) && j->n == rl && !j->kind) break;
            }
            break;
        }
    }
    if (tbl < 0 || !esize) return 0;
    int64_t n = -1;
    if (have_limit && limit >= 0) n = limit + 1 + (idx_add > 0 ? idx_add : 0);
    vec tg = {0};
    for (int64_t k = 0;; ) {
        if (n >= 0 && k >= n) break;
        if (n < 0 && k >= 256) break;
        int64_t ea = tbl + k * esize;
        if (!in_img(ea + esize - 1)) break;
        if (n < 0 && is_code(ea)) break;
        int64_t v;
        size_t o = (size_t)(ea - (int64_t)BASE);        /* tables live in the program image */
        if (ea < (int64_t)BASE) o = (size_t)((ea - (int64_t)BASE) + img_len);   /* Python negative index */
        if (esize == 1) { v = img[o]; if (!unsigned_ && (v & 0x80)) v -= 256; }
        else v = (int16_t)(img[o] | img[o + 1] << 8);
        int64_t t = pc + 4 + v;
        if (k < idx_add) { vpush(&tg, 0); k++; continue; }   /* slots below the biased index are never used */
        if (t & 1) { if (n < 0) break; vpush(&tg, 0); k++; continue; }
        if (n < 0 && (!in_img(t) || !D(t) || (tbl <= t && t < ea + esize) || t < pc - 0x4000)) break;
        vpush(&tg, t); k++;
    }
    int any = 0; for (int i = 0; i < tg.n; i++) if (tg.v[i]) any = 1;
    if (!any) { free(tg.v); return 0; }
    for (int64_t a = tbl; a < tbl + (int64_t)tg.n * esize; a++) add_data(a & ~(int64_t)1);
    if (nJT == capJT) { capJT = capJT * 2 + 64; JT = realloc(JT, sizeof *JT * capJT); }
    JT[nJT].pc = pc; JT[nJT].targets = tg; nJT++;
    for (int i = 0; i < tg.n; i++) if (tg.v[i]) vpush(out, tg.v[i]);
    return 1;
}

/* register contents for jsr/jmp/braf targets (analyze.py regsrc, recomp.py regconst) */
enum { RS_NONE, RS_INT, RS_W, RS_MOVA };
typedef struct { int kind; int64_t v; } rsrc;

/* explore: one function, following branches; calls seed new functions */
static void explore(int64_t entry) {
    if (nF == capF) { capF = capF * 2 + 256; F = realloc(F, sizeof *F * capF); }
    int fi = nF++; func *f = &F[fi]; memset(f, 0, sizeof *f); f->entry = entry;
    func_at[IDX(entry)] = fi + 1;
    stamp++;
    vec work = {0}; vpush(&work, entry);
    rsrc rs[16];
    while (work.n) {
        int64_t pc = work.v[--work.n];
        memset(rs, 0, sizeof rs);
        for (;;) {
            if (!in_range(pc) || vstamp[IDX(pc)] == stamp || !in_img(pc) || is_data(pc)) break;
            const insn *ins = D(pc);
            if (!ins) break;
            vstamp[IDX(pc)] = stamp; vpush(&f->visited, pc); add_code(pc);
            int64_t lit = pcrel_addr(ins, pc);
            if (lit >= 0 && in_img(lit)) {
                int size = (ins->op & 0xF000) == 0x9000 ? 2 : 4;
                if ((ins->op & 0xFF00) != 0xC700) {     /* mova only takes an address */
                    for (int k = 0; k < size; k += 2) add_data(lit + k);
                    if (size == 4) { rs[ins->n].kind = RS_INT; rs[ins->n].v = w(lit); }
                    else { rs[ins->n].kind = RS_W; rs[ins->n].v = sx16(h(lit)); }
                } else { rs[0].kind = RS_MOVA; rs[0].v = lit; }
            } else if ((ins->fmt == F_n || ins->fmt == F_nm || ins->fmt == F_ni || ins->fmt == F_nmd || ins->fmt == F_nd8) && !ins->kind) {
                rs[ins->n].kind = RS_NONE;               /* (analyze.py's "mov r" copy case never matches) */
            }
            int k = ins->kind;
            if (k == K_BSR) seed(branch_target(ins, pc));
            else if (k == K_JSR) { if (rs[ins->n].kind == RS_INT && in_img(rs[ins->n].v)) seed(rs[ins->n].v); }
            if (is_cond(k)) vpush(&work, branch_target(ins, pc));
            if (ins->delay) {                           /* the delay slot belongs to this path */
                int64_t ds = pc + 2;
                if (in_img(ds) && in_range(ds) && vstamp[IDX(ds)] != stamp) {
                    const insn *di = D(ds);
                    if (di) {
                        vstamp[IDX(ds)] = stamp; vpush(&f->visited, ds); add_code(ds);
                        int64_t dl = pcrel_addr(di, ds);
                        if (dl >= 0 && in_img(dl) && (di->op & 0xFF00) != 0xC700)
                            for (int kk = 0; kk < ((di->op & 0xF000) == 0x9000 ? 2 : 4); kk += 2) add_data(dl + kk);
                    }
                }
            }
            if (k == K_BRA) {
                int64_t t = branch_target(ins, pc);
                if (func_of_addr(t) >= 0 && t != entry) { if (!vhas(&f->tail, t)) vpush(&f->tail, t); }   /* tail call */
                else vpush(&work, t);
                break;
            }
            if (k == K_RTS || k == K_RTE) break;
            if (k == K_JMP) {
                if (rs[ins->n].kind == RS_INT && in_img(rs[ins->n].v)) { if (!vhas(&f->tail, rs[ins->n].v)) vpush(&f->tail, rs[ins->n].v); seed(rs[ins->n].v); }
                break;
            }
            if (k == K_BRAF && rs[ins->n].kind == RS_W) { vpush(&work, pc + 4 + rs[ins->n].v); break; }   /* long-range constant branch */
            if (k == K_BRAF) {
                vec ts = {0};
                if (resolve_jt(pc, &ts)) for (int i = 0; i < ts.n; i++) vpush(&work, ts.v[i]);
                free(ts.v);
                break;
            }
            pc += ins->delay ? 4 : 2;
        }
    }
    free(work.v);
    f = &F[fi];
}

static void drain(void) {
    while (qhead < qtail) { int64_t a = queue[qhead++]; if (func_of_addr(a) >= 0) continue; explore(a); }
}
static void run(int64_t entry) { seed(entry); drain(); }

/* speculative_ok: linear decode from a must reach rts/jmp/bra+slot with no illegal op or clash */
static int speculative_ok(int64_t a) {
    int64_t pc = a;
    for (int n = 0; n < 4000; n++) {
        if (!in_img(pc) || is_data(pc) || is_code(pc)) return 0;
        const insn *i = D(pc);
        if (!i) return 0;
        if (i->kind == K_RTS || i->kind == K_JMP || i->kind == K_BRA || i->kind == K_RTE || i->kind == K_BRAF) {
            int64_t ds = pc + 2;
            return in_img(ds) && !is_data(ds) && D(ds) != NULL;
        }
        pc += 2;
    }
    return 0;
}

/* gaps(): uncovered ranges in the code range. Like the Python, coverage is an array over
 * the program image indexed by address - BASE, where overlay addresses (below BASE)
 * wrap around to the end of the image. */
static int64_t cov_idx(int64_t a) { int64_t i = a - (int64_t)BASE; return i < 0 ? i + img_len : i; }
static int gaps(int64_t **out) {
    uint8_t *cov = calloc((size_t)img_len + 1, 1);
    for (int64_t a = LO; a < END; a++) {
        uint8_t fl = aflag[IDX(a)];
        if (fl & (A_CODE | A_DATA)) { int64_t i = cov_idx(a); cov[i] = 1; cov[i + 1] = 1; }
    }
    int64_t lo = code_min, hi = code_max + 2; int n = 0, cap = 256; int64_t *v = malloc(sizeof *v * 2 * cap);
    int64_t a = lo;
    while (a < hi) {
        if (!cov[cov_idx(a)]) {
            int64_t s = a;
            while (a < hi && !cov[cov_idx(a)]) a++;
            if (n == cap) { cap *= 2; v = realloc(v, sizeof *v * 2 * cap); }
            v[2 * n] = s; v[2 * n + 1] = a; n++;
        } else a++;
    }
    free(cov); *out = v; return n;
}
static int leaf_pass(void) {
    int added = 0; int64_t *g; int ng = gaps(&g);
    for (int k = 0; k < ng; k++) {
        int64_t s = g[2 * k], e = g[2 * k + 1], a = (s + 1) & ~(int64_t)1;
        while (a < e && (h(a) == 0x0009 || h(a) == 0x0000)) a += 2;     /* alignment padding */
        if ((a >= e || a % 4) && a < e && a % 4 == 2 && h(a) == 0x0009) a += 2;
        if (a < e && speculative_ok(a)) { seed(a); added++; }
    }
    free(g);
    drain();
    return added;
}

/* analyze.py __main__ */
static void analyze(const rc_input *in) {
    run(0x8C010000);
    run(0x8C05BEF8);
    for (int i = 0; i < in->nextra; i++) run(in->extra[i]);
    for (int64_t off = 0; off + 3 < img_len; off += 4) {             /* pointer pass */
        int64_t v = img[off] | img[off + 1] << 8 | img[off + 2] << 16 | (uint32_t)img[off + 3] << 24;
        if (in_img(v) && func_of_addr(v) < 0 && (is_prologue(v) ||
                (v % 4 == 0 && !is_code(v) && !is_data(v) && h(v - 2) == 0x0009 && speculative_ok(v))))
            seed(v);
    }
    drain();
    int64_t lo = code_min, hi = code_max;                              /* gap pass */
    for (int64_t a = lo; a < hi; a += 2)
        if (!is_code(a) && !is_data(a) && is_prologue(a)) { seed(a); drain(); }
    for (int it = 0; it < 6; it++) if (!leaf_pass()) break;
}

/* ------------------------------------------------------------------ code generation (recomp.py) */
static const uint32_t *LIVE; static int nLIVE;
static int is_live(int64_t a) { for (int i = 0; i < nLIVE; i++) if (LIVE[i] == (uint32_t)a) return 1; return 0; }
static uint8_t *baked;              /* per address from LO */
static int nbaked;
static void bake(int64_t a) { if (in_range(a) && !baked[IDX(a)]) { baked[IDX(a)] = 1; nbaked++; } }

static const char *RR(int n) { static const char *r[16] = {"c->r[0]","c->r[1]","c->r[2]","c->r[3]","c->r[4]","c->r[5]","c->r[6]","c->r[7]",
    "c->r[8]","c->r[9]","c->r[10]","c->r[11]","c->r[12]","c->r[13]","c->r[14]","c->r[15]"}; return r[n & 15]; }

/* op_c: C statement(s) for a non-branch instruction; NULL if unsupported */
static int gen_interp_mode;
static const char *op_c(const insn *ins, int64_t pc) {
    static char b[600];
    unsigned o = ins->op; int n = ins->n, m = ins->m; unsigned hi = o >> 12, lo = o & 0xF;
    const char *rn = RR(n), *rm = RR(m);
#define RET(...) do { snprintf(b, sizeof b, __VA_ARGS__); return b; } while (0)
    /* ---- data movement */
    if (hi == 0xE) RET("%s = 0x%08Xu;", rn, (uint32_t)ins->i);
    if (hi == 0x9) {
        int64_t a = pcrel_addr(ins, pc);
        if (in_ovl(pc) || is_live(a & ~(int64_t)3)) RET("%s = (uint32_t)(int16_t)R16(0x%08Xu);", rn, (uint32_t)a);
        bake(a & ~(int64_t)3);
        int32_t v = (int16_t)(img[a - BASE] | img[a - BASE + 1] << 8);
        RET("%s = 0x%08Xu; /* mov.w @pc */", rn, (uint32_t)v);
    }
    if (hi == 0xD) {
        int64_t a = pcrel_addr(ins, pc);
        if (in_ovl(pc) || is_live(a)) RET("%s = R32(0x%08Xu); /* live literal */", rn, (uint32_t)a);
        bake(a);
        RET("%s = 0x%08Xu; /* mov.l @0x%08X */", rn, w(a), (uint32_t)a);
    }
    if ((o & 0xFF00) == 0xC700) RET("c->r[0] = 0x%08Xu; /* mova */", (uint32_t)pcrel_addr(ins, pc));
    if (hi == 0x6) {
        if (lo == 0) RET("%s = (uint32_t)(int8_t)R8(%s);", rn, rm);
        if (lo == 1) RET("%s = (uint32_t)(int16_t)R16(%s);", rn, rm);
        if (lo == 2) RET("%s = R32(%s);", rn, rm);
        if (lo == 3) RET("%s = %s;", rn, rm);
        if (lo >= 4 && lo <= 6) {
            int sz = lo == 4 ? 1 : lo == 5 ? 2 : 4; const char *rd = lo == 4 ? "R8" : lo == 5 ? "R16" : "R32", *ext = lo == 4 ? "(int8_t)" : lo == 5 ? "(int16_t)" : "";
            char inc[64] = ""; if (n != m) snprintf(inc, sizeof inc, " %s += %d;", rm, sz);
            RET("{ uint32_t v = (uint32_t)%s%s(%s);%s %s = v; }", ext, rd, rm, inc, rn);
        }
        if (lo == 7) RET("%s = ~%s;", rn, rm);
        if (lo == 8) RET("{ uint32_t v = %s; %s = (v & 0xFFFF0000u) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }", rm, rn);
        if (lo == 9) RET("{ uint32_t v = %s; %s = (v >> 16) | (v << 16); }", rm, rn);
        if (lo == 0xA) RET("{ uint32_t t0 = 0 - %s; uint32_t r = t0 - c->t; c->t = (0 < t0) | (t0 < r); %s = r; }", rm, rn);
        if (lo == 0xB) RET("%s = 0 - %s;", rn, rm);
        if (lo == 0xC) RET("%s = %s & 0xFF;", rn, rm);
        if (lo == 0xD) RET("%s = %s & 0xFFFF;", rn, rm);
        if (lo == 0xE) RET("%s = (uint32_t)(int8_t)%s;", rn, rm);
        if (lo == 0xF) RET("%s = (uint32_t)(int16_t)%s;", rn, rm);
    }
    if (hi == 0x2) {
        if (lo == 0) RET("W8(%s, (uint8_t)%s);", rn, rm);
        if (lo == 1) RET("W16(%s, (uint16_t)%s);", rn, rm);
        if (lo == 2) RET("W32(%s, %s);", rn, rm);
        if (lo >= 4 && lo <= 6) {
            int sz = lo == 4 ? 1 : lo == 5 ? 2 : 4; const char *wr = lo == 4 ? "W8" : lo == 5 ? "W16" : "W32", *cast = lo == 4 ? "(uint8_t)" : lo == 5 ? "(uint16_t)" : "";
            RET("{ uint32_t v = %s; uint32_t a = %s - %d; %s(a, %sv); %s = a; }", rm, rn, sz, wr, cast, rn);
        }
        if (lo == 7) RET("c->q = %s >> 31; c->m_ = %s >> 31; c->t = c->q ^ c->m_;", rn, rm);
        if (lo == 8) RET("c->t = (%s & %s) == 0;", rn, rm);
        if (lo == 9) RET("%s &= %s;", rn, rm);
        if (lo == 0xA) RET("%s ^= %s;", rn, rm);
        if (lo == 0xB) RET("%s |= %s;", rn, rm);
        if (lo == 0xC) RET("{ uint32_t x = %s ^ %s; c->t = !(x & 0xFF000000u) || !(x & 0xFF0000u) || !(x & 0xFF00u) || !(x & 0xFFu); }", rn, rm);
        if (lo == 0xD) RET("%s = (%s >> 16) | (%s << 16);", rn, rn, rm);
        if (lo == 0xE) RET("c->macl = (uint32_t)(uint16_t)%s * (uint32_t)(uint16_t)%s;", rn, rm);
        if (lo == 0xF) RET("c->macl = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)%s);", rn, rm);
    }
    if (hi == 0x1) RET("W32(%s + %d, %s);", rn, ins->d * 4, rm);
    if (hi == 0x5) RET("%s = R32(%s + %d);", rn, rm, ins->d * 4);
    if (hi == 0x0) {
        unsigned x = o & 0xF00F;
        if (x == 4) RET("W8(c->r[0] + %s, (uint8_t)%s);", rn, rm);
        if (x == 5) RET("W16(c->r[0] + %s, (uint16_t)%s);", rn, rm);
        if (x == 6) RET("W32(c->r[0] + %s, %s);", rn, rm);
        if (lo == 7 && x == 7) RET("c->macl = %s * %s;", rn, rm);
        if (x == 0xC) RET("%s = (uint32_t)(int8_t)R8(c->r[0] + %s);", rn, rm);
        if (x == 0xD) RET("%s = (uint32_t)(int16_t)R16(c->r[0] + %s);", rn, rm);
        if (x == 0xE) RET("%s = R32(c->r[0] + %s);", rn, rm);
        if (x == 0xF) RET("sh4_macl(c, %d, %d);", n, m);
    }
    if (hi == 0x8) {
        unsigned sub = (o >> 8) & 0xF, d = o & 0xF; const char *rr = RR((o >> 4) & 0xF);
        if (sub == 0) RET("W8(%s + %d, (uint8_t)c->r[0]);", rr, d);
        if (sub == 1) RET("W16(%s + %d, (uint16_t)c->r[0]);", rr, d * 2);
        if (sub == 4) RET("c->r[0] = (uint32_t)(int8_t)R8(%s + %d);", rr, d);
        if (sub == 5) RET("c->r[0] = (uint32_t)(int16_t)R16(%s + %d);", rr, d * 2);
        if (sub == 8) RET("c->t = c->r[0] == 0x%08Xu;", (uint32_t)ins->i);
    }
    if (hi == 0xC) {
        unsigned sub = (o >> 8) & 0xF, d = o & 0xFF;
        switch (sub) {
        case 0: RET("W8(c->gbr + %d, (uint8_t)c->r[0]);", d);
        case 1: RET("W16(c->gbr + %d, (uint16_t)c->r[0]);", d * 2);
        case 2: RET("W32(c->gbr + %d, c->r[0]);", d * 4);
        case 4: RET("c->r[0] = (uint32_t)(int8_t)R8(c->gbr + %d);", d);
        case 5: RET("c->r[0] = (uint32_t)(int16_t)R16(c->gbr + %d);", d * 2);
        case 6: RET("c->r[0] = R32(c->gbr + %d);", d * 4);
        case 8: RET("c->t = (c->r[0] & 0x%02Xu) == 0;", d);
        case 9: RET("c->r[0] &= 0x%02Xu;", d);
        case 0xA: RET("c->r[0] ^= 0x%02Xu;", d);
        case 0xB: RET("c->r[0] |= 0x%02Xu;", d);
        case 0xC: RET("c->t = (R8(c->gbr + c->r[0]) & 0x%02X) == 0;", d);
        case 0xD: RET("{ uint32_t a = c->gbr + c->r[0]; W8(a, R8(a) & 0x%02X); }", d);
        case 0xE: RET("{ uint32_t a = c->gbr + c->r[0]; W8(a, R8(a) ^ 0x%02X); }", d);
        case 0xF: RET("{ uint32_t a = c->gbr + c->r[0]; W8(a, R8(a) | 0x%02X); }", d);
        case 3: RET("sh4_trapa(c, 0x%02X, 0x%08Xu);", d, (uint32_t)pc);
        }
    }
    if (hi == 0x7) RET("%s += 0x%08Xu;", rn, (uint32_t)ins->i);
    /* ---- arithmetic 3nm */
    if (hi == 0x3) {
        switch (lo) {
        case 0: RET("c->t = %s == %s;", rn, rm);
        case 2: RET("c->t = %s >= %s;", rn, rm);
        case 3: RET("c->t = (int32_t)%s >= (int32_t)%s;", rn, rm);
        case 6: RET("c->t = %s > %s;", rn, rm);
        case 7: RET("c->t = (int32_t)%s > (int32_t)%s;", rn, rm);
        case 8: RET("%s -= %s;", rn, rm);
        case 0xC: RET("%s += %s;", rn, rm);
        case 4: RET("sh4_div1(c, %d, %d);", n, m);
        case 5: RET("{ uint64_t p = (uint64_t)%s * %s; c->mach = (uint32_t)(p >> 32); c->macl = (uint32_t)p; }", rn, rm);
        case 0xD: RET("{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; c->mach = (uint32_t)((uint64_t)p >> 32); c->macl = (uint32_t)p; }", rn, rm);
        case 0xA: RET("{ uint32_t t1 = %s - %s; uint32_t t0 = %s; %s = t1 - c->t; c->t = (t0 < t1) | (t1 < %s); }", rn, rm, rn, rn, rn);
        case 0xE: RET("{ uint32_t t1 = %s + %s; uint32_t t0 = %s; %s = t1 + c->t; c->t = (t0 > t1) | (t1 > %s); }", rn, rm, rn, rn, rn);
        case 0xB: RET("{ int32_t a = (int32_t)%s, b = (int32_t)%s, r = (int32_t)((uint32_t)a - (uint32_t)b); c->t = ((a ^ b) & (a ^ r)) >> 31 & 1; %s = (uint32_t)r; }", rn, rm, rn);
        case 0xF: RET("{ int32_t a = (int32_t)%s, b = (int32_t)%s, r = (int32_t)((uint32_t)a + (uint32_t)b); c->t = (~(a ^ b) & (a ^ r)) >> 31 & 1; %s = (uint32_t)r; }", rn, rm, rn);
        }
    }
    /* ---- 4n.. group */
    if (hi == 0x4) {
        if (lo == 0xC) RET("%s = sh4_shad(%s, %s);", rn, rn, rm);
        if (lo == 0xD) RET("%s = sh4_shld(%s, %s);", rn, rn, rm);
        if (lo == 0xF) RET("sh4_macw(c, %d, %d);", n, m);
        unsigned x = o & 0xF0FF;
        switch (x) {
        case 0x4000: case 0x4020: RET("c->t = %s >> 31; %s <<= 1;", rn, rn);
        case 0x4001: RET("c->t = %s & 1; %s >>= 1;", rn, rn);
        case 0x4021: RET("c->t = %s & 1; %s = (uint32_t)((int32_t)%s >> 1);", rn, rn, rn);
        case 0x4004: RET("c->t = %s >> 31; %s = (%s << 1) | c->t;", rn, rn, rn);
        case 0x4005: RET("c->t = %s & 1; %s = (%s >> 1) | (c->t << 31);", rn, rn, rn);
        case 0x4024: RET("{ uint32_t t0 = c->t; c->t = %s >> 31; %s = (%s << 1) | t0; }", rn, rn, rn);
        case 0x4025: RET("{ uint32_t t0 = c->t; c->t = %s & 1; %s = (%s >> 1) | (t0 << 31); }", rn, rn, rn);
        case 0x4008: RET("%s <<= 2;", rn);
        case 0x4009: RET("%s >>= 2;", rn);
        case 0x4018: RET("%s <<= 8;", rn);
        case 0x4019: RET("%s >>= 8;", rn);
        case 0x4028: RET("%s <<= 16;", rn);
        case 0x4029: RET("%s >>= 16;", rn);
        case 0x4010: RET("%s -= 1; c->t = %s == 0;", rn, rn);
        case 0x4011: RET("c->t = (int32_t)%s >= 0;", rn);
        case 0x4015: RET("c->t = (int32_t)%s > 0;", rn);
        case 0x401B: RET("{ uint8_t v = R8(%s); c->t = v == 0; W8(%s, v | 0x80); }", rn, rn);
        case 0x400A: RET("c->mach = %s;", rn);
        case 0x401A: RET("c->macl = %s;", rn);
        case 0x402A: RET("c->pr = %s;", rn);
        case 0x405A: RET("c->fpul = %s;", rn);
        case 0x406A: RET("sh4_set_fpscr(c, %s);", rn);
        case 0x400E: RET("sh4_set_sr(c, %s);", rn);
        case 0x401E: RET("c->gbr = %s;", rn);
        case 0x402E: RET("c->vbr = %s;", rn);
        case 0x403E: RET("c->ssr = %s;", rn);
        case 0x404E: RET("c->spc = %s;", rn);
        case 0x40FA: RET("c->dbr = %s;", rn);
        }
        const char *st = NULL;
        switch (x) {
        case 0x4002: st = "c->mach"; break; case 0x4012: st = "c->macl"; break; case 0x4022: st = "c->pr"; break;
        case 0x4052: st = "c->fpul"; break; case 0x4062: st = "c->fpscr"; break; case 0x4003: st = "sh4_get_sr(c)"; break;
        case 0x4013: st = "c->gbr"; break; case 0x4023: st = "c->vbr"; break; case 0x4033: st = "c->ssr"; break;
        case 0x4043: st = "c->spc"; break; case 0x4032: st = "c->sgr"; break; case 0x40F2: st = "c->dbr"; break;
        }
        if (st) RET("%s -= 4; W32(%s, %s);", rn, rn, st);
        const char *ld = NULL;
        switch (x) {
        case 0x4006: ld = "c->mach = v;"; break; case 0x4016: ld = "c->macl = v;"; break; case 0x4026: ld = "c->pr = v;"; break;
        case 0x4056: ld = "c->fpul = v;"; break; case 0x4066: ld = "sh4_set_fpscr(c, v);"; break; case 0x4007: ld = "sh4_set_sr(c, v);"; break;
        case 0x4017: ld = "c->gbr = v;"; break; case 0x4027: ld = "c->vbr = v;"; break; case 0x4037: ld = "c->ssr = v;"; break;
        case 0x4047: ld = "c->spc = v;"; break; case 0x40F6: ld = "c->dbr = v;"; break;
        }
        if (ld) RET("{ uint32_t v = R32(%s); %s += 4; %s }", rn, rn, ld);
        if ((o & 0xF08F) == 0x4083) RET("%s -= 4; W32(%s, c->rbank[%d]);", rn, rn, ins->m);
        if ((o & 0xF08F) == 0x4087) RET("c->rbank[%d] = R32(%s); %s += 4;", ins->m, rn, rn);
        if ((o & 0xF08F) == 0x408E) RET("c->rbank[%d] = %s;", ins->m, rn);
    }
    if (hi == 0x0) {
        unsigned x = o & 0xF0FF;
        const char *t = NULL;
        switch (x) {
        case 0x0002: t = "sh4_get_sr(c)"; break; case 0x0012: t = "c->gbr"; break; case 0x0022: t = "c->vbr"; break;
        case 0x0032: t = "c->ssr"; break; case 0x0042: t = "c->spc"; break; case 0x0029: t = "c->t"; break;
        case 0x000A: t = "c->mach"; break; case 0x001A: t = "c->macl"; break; case 0x002A: t = "c->pr"; break;
        case 0x003A: t = "c->sgr"; break; case 0x005A: t = "c->fpul"; break; case 0x006A: t = "c->fpscr"; break;
        case 0x00FA: t = "c->dbr"; break;
        }
        if (t) RET("%s = %s;", rn, t);
        if (x == 0x0083) RET("if ((%s & 0xFC000000u) == 0xE0000000u) sh4_sq_flush(c, %s);", rn, rn);
        if (x == 0x0093) RET("/* ocbi */");
        if (x == 0x00A3) RET("/* ocbp */");
        if (x == 0x00B3) RET("/* ocbwb */");
        if (x == 0x00C3) RET("W32(%s, c->r[0]); /* movca.l */", rn);
        if ((o & 0xF08F) == 0x0082) RET("%s = c->rbank[%d];", rn, ins->m);
        switch (o) {
        case 0x0008: RET("c->t = 0;");
        case 0x0009: RET("%s", "");
        case 0x0018: RET("c->t = 1;");
        case 0x0019: RET("c->m_ = c->q = c->t = 0;");
        case 0x0028: RET("c->mach = c->macl = 0;");
        case 0x0048: RET("c->s = 0;");
        case 0x0058: RET("c->s = 1;");
        case 0x001B: RET("sh4_sleep(c, 0x%08Xu);", (uint32_t)pc);
        case 0x0038: RET("sh4_ldtlb(c);");
        }
    }
    /* ---- FPU */
    if (hi == 0xF) {
        char fn[16], fm[16]; snprintf(fn, sizeof fn, "FR(%d)", n); snprintf(fm, sizeof fm, "FR(%d)", m);
        if (o == 0xF3FD) RET("c->fpscr ^= FPSCR_SZ;");
        if (o == 0xFBFD) RET("sh4_set_fpscr(c, c->fpscr ^ FPSCR_FR);");
        if (lo <= 3) {
            const char *op = lo == 0 ? "+" : lo == 1 ? "-" : lo == 2 ? "*" : "/";
            RET("if (FPU_PR) sh4_setdr(c, %d, sh4_getdr(c, %d) %s sh4_getdr(c, %d)); else %s = %s %s %s;", n & 14, n & 14, op, m & 14, fn, fn, op, fm);
        }
        if (lo == 4) RET("c->t = FPU_PR ? (sh4_getdr(c, %d) == sh4_getdr(c, %d)) : (%s == %s);", n & 14, m & 14, fn, fm);
        if (lo == 5) RET("c->t = FPU_PR ? (sh4_getdr(c, %d) > sh4_getdr(c, %d)) : (%s > %s);", n & 14, m & 14, fn, fm);
        if (lo == 6) RET("sh4_fmov_load(c, %d, c->r[0] + %s);", n, rm);
        if (lo == 7) RET("sh4_fmov_store(c, %d, c->r[0] + %s);", m, rn);
        if (lo == 8) RET("sh4_fmov_load(c, %d, %s);", n, rm);
        if (lo == 9) RET("{ uint32_t a = %s; %s += FMOV_SIZE; sh4_fmov_load(c, %d, a); }", rm, rm, n);
        if (lo == 0xA) RET("sh4_fmov_store(c, %d, %s);", m, rn);
        if (lo == 0xB) RET("{ uint32_t a = %s - FMOV_SIZE; sh4_fmov_store(c, %d, a); %s = a; }", rn, m, rn);
        if (lo == 0xC) RET("sh4_fmov_rr(c, %d, %d);", n, m);
        if (lo == 0xE) RET("%s = FR(0) * %s + %s;", fn, fm, fn);
        unsigned x = o & 0xF0FF; int d = n & 14;
        switch (x) {
        case 0xF00D: RET("FRU(%d) = c->fpul;", n);
        case 0xF01D: RET("c->fpul = FRU(%d);", n);
        case 0xF02D: RET("if (FPU_PR) sh4_setdr(c, %d, (double)(int32_t)c->fpul); else %s = (float)(int32_t)c->fpul;", d, fn);
        case 0xF03D: RET("c->fpul = (uint32_t)(FPU_PR ? sh4_ftrc_d(sh4_getdr(c, %d)) : sh4_ftrc_f(%s));", d, fn);
        case 0xF04D: RET("if (FPU_PR) FRU(%d) ^= 0x80000000u; else FRU(%d) ^= 0x80000000u;", d, n);
        case 0xF05D: RET("if (FPU_PR) FRU(%d) &= 0x7FFFFFFFu; else FRU(%d) &= 0x7FFFFFFFu;", d, n);
        case 0xF06D: RET("if (FPU_PR) sh4_setdr(c, %d, sqrt(sh4_getdr(c, %d))); else %s = sqrtf(%s);", d, d, fn, fn);
        case 0xF07D: RET("%s = 1.0f / sqrtf(%s);", fn, fn);
        case 0xF08D: RET("FRU(%d) = 0;", n);
        case 0xF09D: RET("FRU(%d) = 0x3F800000u;", n);
        case 0xF0AD: RET("{ float f; memcpy(&f, &c->fpul, 4); sh4_setdr(c, %d, (double)f); }", d);
        case 0xF0BD: RET("{ float f = (float)sh4_getdr(c, %d); memcpy(&c->fpul, &f, 4); }", d);
        case 0xF0ED: RET("sh4_fipr(c, %d, %d);", (o >> 8) & 12, ((o >> 8) & 3) * 4);
        }
        if ((o & 0xF3FF) == 0xF1FD) RET("sh4_ftrv(c, %d);", (o >> 8) & 12);
        if ((o & 0xF1FF) == 0xF0FD) RET("sh4_fsca(c, %d);", d);
    }
#undef RET
    return NULL;
}

static const char *const *NAMEV; static const rc_name *NAMES; static int nNAMES;
static const char *name_of(int64_t a) { for (int i = 0; i < nNAMES; i++) if (NAMES[i].addr == (uint32_t)a) return NAMES[i].name; return NULL; }

/* per-function lookup marks (address from LO) */
static uint32_t *vmark, *lmark, *smark; static uint32_t gstamp;
static int inV(int64_t a) { return in_range(a) && vmark[IDX(a)] == gstamp; }
static int isLabel(int64_t a) { return in_range(a) && lmark[IDX(a)] == gstamp; }
static int isSlot(int64_t a) { return in_range(a) && smark[IDX(a)] == gstamp; }
static void addLabel(int64_t a) { if (in_range(a)) lmark[IDX(a)] = gstamp; }
static int isFunc(int64_t a) { return func_of_addr(a) >= 0; }

typedef struct { int64_t r; int fi; } resent;
static resent *RES; static int nRES, capRES;

/* gen_function; returns 0 if an instruction is unsupported */
static int gen_function(int fi, sbuf *out) {
    func *f = &F[fi]; int64_t entry = f->entry;
    if (entry == 0x8C0AF7B4) {                      /* HOOKS: CRI longjmp -> runtime */
        sb_printf(out, "void f_%08X(sh4ctx *c) { void hle_ctx_restore(sh4ctx *); hle_ctx_restore(c); }", (uint32_t)entry);
        return 1;
    }
    int64_t *pcs = f->visited.v; int np = f->visited.n;
    qsort(pcs, (size_t)np, sizeof *pcs, cmp64);
    gstamp++;
    for (int i = 0; i < np; i++) vmark[IDX(pcs[i])] = gstamp;
    for (int i = 0; i < np; i++) if (D(pcs[i])->delay && in_range(pcs[i] + 2)) smark[IDX(pcs[i] + 2)] = gstamp;
    for (int i = 0; i < np; i++) {
        int64_t p = pcs[i]; const insn *ins = D(p);
        if (is_cond(ins->kind) || ins->kind == K_BRA) { int64_t t = branch_target(ins, p); if (inV(t)) addLabel(t); }
        if (ins->kind == K_BRAF) {
            jtab *j = jt_find(p);
            if (j) { for (int k = 0; k < j->targets.n; k++) if (j->targets.v[k] && inV(j->targets.v[k])) addLabel(j->targets.v[k]); }
            else for (int back = 2; back <= 8; back += 2) {
                if (!is_code(p - back)) break;
                const insn *pi = D(p - back);
                if (pi->kind) break;
                if ((pi->op & 0xF000) == 0x9000 && pi->n == ins->n) {
                    int64_t t = p + 4 + sx16(h(pcrel_addr(pi, p - back)));
                    if (inV(t)) addLabel(t);
                    break;
                }
            }
        }
    }
    /* resume points: every return address after a call inside this function */
    vec resumes = {0};
    for (int i = 0; i < np; i++) {
        const insn *ins = D(pcs[i]); int64_t r = pcs[i] + 4;
        if ((ins->kind == K_BSR || ins->kind == K_JSR || ins->kind == K_BSRF) && inV(r) && !isSlot(r) && !vhas(&resumes, r)) vpush(&resumes, r);
    }
    qsort(resumes.v, (size_t)resumes.n, sizeof *resumes.v, cmp64);
    for (int i = 0; i < resumes.n; i++) {
        addLabel(resumes.v[i]);
        if (nRES == capRES) { capRES = capRES * 2 + 1024; RES = realloc(RES, sizeof *RES * capRES); }
        RES[nRES].r = resumes.v[i]; RES[nRES].fi = fi; nRES++;
    }
    sbuf b = {0};
    const char *nm = name_of(entry);
    E(&b, "/* %08X%s%s */", (uint32_t)entry, nm ? " " : "", nm ? nm : "");
    E(&b, "void f_%08X(sh4ctx *c) {", (uint32_t)entry);
    if (resumes.n) {
        E(&b, "    if (sh4_resume_pc) { uint32_t r_ = sh4_resume_pc; sh4_resume_pc = 0; switch (r_) {");
        for (int i = 0; i < resumes.n; i++) E(&b, "      case 0x%08Xu: goto L_%08X;", (uint32_t)resumes.v[i], (uint32_t)resumes.v[i]);
        E(&b, "      default: sh4_unreachable(c, r_); return; } }");
    }
    free(resumes.v);
    if (np && pcs[0] != entry) { addLabel(entry); E(&b, "    goto L_%08X;", (uint32_t)entry); }
    rsrc rc[16]; memset(rc, 0, sizeof rc);
    int prev_falls = 0, ok = 1; int64_t last_end = -1;
    char slot_c[700];
    for (int i = 0; i < np; i++) {
        int64_t p = pcs[i];
        if (isSlot(p) && !isLabel(p)) continue;
        const insn *ins = D(p);
        if (last_end >= 0 && p != last_end && prev_falls) {
            if (inV(last_end)) E(&b, "    goto L_%08X;", (uint32_t)last_end);
            else E(&b, "    sh4_unreachable(c, 0x%08Xu); return;", (uint32_t)last_end);
        }
        if (isLabel(p)) { E(&b, "L_%08X:", (uint32_t)p); memset(rc, 0, sizeof rc); }
        int k = ins->kind;
        slot_c[0] = 0;
        if (ins->delay) {
            int64_t sp = p + 2; const insn *si = D(sp);
            if (!si || si->kind) snprintf(slot_c, sizeof slot_c, "sh4_unreachable(c, 0x%08Xu); /* illegal slot */", (uint32_t)sp);
            else { const char *s = op_c(si, sp); if (!s) { ok = 0; s = ""; } snprintf(slot_c, sizeof slot_c, "%s", s); }
        }
        int64_t nxt = p + (ins->delay ? 4 : 2);
        int falls = 1;
        if (!k) {
            const char *s = op_c(ins, p);
            if (!s) { ok = 0; s = ""; }
            if (*s) E(&b, "    %s", s);
            int64_t a = pcrel_addr(ins, p);
            if ((ins->op & 0xF000) == 0xD000 && !in_ovl(p) && !is_live(a)) { rc[ins->n].kind = RS_INT; rc[ins->n].v = w(a); }
            else if (ins->fmt == F_n || ins->fmt == F_nm || ins->fmt == F_ni || ins->fmt == F_nmd || ins->fmt == F_nd8 || ins->fmt == F_bank) rc[ins->n].kind = RS_NONE;
            if ((ins->op & 0xF000) == 0x9000 && !in_ovl(p) && !is_live(a & ~(int64_t)3)) { rc[ins->n].kind = RS_W; rc[ins->n].v = (int16_t)(img[a - BASE] | img[a - BASE + 1] << 8); }
        } else if (k == K_BT || k == K_BF) {
            int64_t t = branch_target(ins, p); const char *cond = k == K_BT ? "c->t" : "!c->t";
            if (inV(t)) E(&b, "    if (%s) {%s goto L_%08X; }", cond, t <= p ? " SH4_POLL(c);" : "", (uint32_t)t);
            else if (isFunc(t)) E(&b, "    if (%s) { f_%08X(c); return; }", cond, (uint32_t)t);
            else E(&b, "    if (%s) { sh4_dispatch(c, 0x%08Xu); return; }", cond, (uint32_t)t);
        } else if (k == K_BTS || k == K_BFS) {
            int64_t t = branch_target(ins, p); const char *cond = k == K_BTS ? "c->t" : "!c->t";
            E(&b, "    { uint32_t cond = %s; %s", cond, slot_c);
            if (inV(t)) E(&b, "      if (cond) {%s goto L_%08X; } }", t <= p ? " SH4_POLL(c);" : "", (uint32_t)t);
            else if (isFunc(t)) E(&b, "      if (cond) { f_%08X(c); return; } }", (uint32_t)t);
            else E(&b, "      if (cond) { sh4_dispatch(c, 0x%08Xu); return; } }", (uint32_t)t);
        } else if (k == K_BRA) {
            int64_t t = branch_target(ins, p);
            E(&b, "    %s", slot_c);
            if (inV(t) && !vhas(&f->tail, t)) E(&b, "   %s goto L_%08X;", t <= p ? " SH4_POLL(c);" : "", (uint32_t)t);
            else if (isFunc(t)) E(&b, "    f_%08X(c); return;", (uint32_t)t);
            else E(&b, "    sh4_dispatch(c, 0x%08Xu); return;", (uint32_t)t);
            falls = 0;
        } else if (k == K_BSR) {
            int64_t t = branch_target(ins, p);
            E(&b, "    c->pr = 0x%08Xu; %s", (uint32_t)(p + 4), slot_c);
            if (isFunc(t)) E(&b, "    f_%08X(c);", (uint32_t)t); else E(&b, "    sh4_dispatch(c, 0x%08Xu);", (uint32_t)t);
        } else if (k == K_JSR) {
            E(&b, "    { uint32_t tgt = %s; c->pr = 0x%08Xu; %s", RR(ins->n), (uint32_t)(p + 4), slot_c);
            if (rc[ins->n].kind == RS_INT && isFunc(rc[ins->n].v)) E(&b, "      f_%08X(c); }", (uint32_t)rc[ins->n].v);
            else E(&b, "      sh4_dispatch(c, tgt); }");
        } else if (k == K_BSRF) {
            E(&b, "    { uint32_t tgt = 0x%08Xu + %s; c->pr = 0x%08Xu; %s", (uint32_t)(p + 4), RR(ins->n), (uint32_t)(p + 4), slot_c);
            E(&b, "      sh4_dispatch(c, tgt); }");
        } else if (k == K_RTS) {
            E(&b, "    %s", slot_c); E(&b, "    return;"); falls = 0;
        } else if (k == K_RTE) {
            E(&b, "    %s", slot_c); E(&b, "    sh4_rte(c, 0x%08Xu); return;", (uint32_t)p); falls = 0;
        } else if (k == K_JMP) {
            E(&b, "    { uint32_t tgt = %s; %s", RR(ins->n), slot_c);
            if (rc[ins->n].kind == RS_INT && isFunc(rc[ins->n].v)) E(&b, "      f_%08X(c); return; }", (uint32_t)rc[ins->n].v);
            else E(&b, "      sh4_dispatch(c, tgt); return; }");
            falls = 0;
        } else if (k == K_BRAF) {
            E(&b, "    { uint32_t tgt = 0x%08Xu + %s; %s", (uint32_t)(p + 4), RR(ins->n), slot_c);
            if (rc[ins->n].kind == RS_W) {
                int64_t t = (p + 4 + rc[ins->n].v) & 0xFFFFFFFF;
                if (isLabel(t)) E(&b, "      goto L_%08X; }", (uint32_t)t); else E(&b, "      sh4_dispatch(c, tgt); return; }");
            } else {
                E(&b, "      switch (tgt) {");
                jtab *j = jt_find(p);
                if (j) {
                    vec u = {0};
                    for (int q = 0; q < j->targets.n; q++) if (j->targets.v[q] && !vhas(&u, j->targets.v[q])) vpush(&u, j->targets.v[q]);
                    qsort(u.v, (size_t)u.n, sizeof *u.v, cmp64);
                    for (int q = 0; q < u.n; q++) if (inV(u.v[q])) E(&b, "      case 0x%08Xu: goto L_%08X;", (uint32_t)u.v[q], (uint32_t)u.v[q]);
                    free(u.v);
                }
                E(&b, "      default: sh4_dispatch(c, tgt); return; } }");
            }
            falls = 0;
        } else if (k == K_TRAPA) {
            const char *s = op_c(ins, p); if (!s) { ok = 0; s = ""; }
            E(&b, "    %s", s);
        }
        if (k) memset(rc, 0, sizeof rc);
        prev_falls = falls; last_end = nxt;
    }
    if (prev_falls) E(&b, "    sh4_unreachable(c, 0x%08Xu); return;", (uint32_t)last_end);
    E(&b, "}");
    if (ok) sb_put(out, b.p, b.n);
    else sb_printf(out, "void f_%08X(sh4ctx *c) { sh4_unreachable(c, 0x%08Xu); } /* unsupported */", (uint32_t)entry, (uint32_t)entry);
    free(b.p);
    return ok;
}

static int cmp_func(const void *a, const void *b) { int64_t x = F[*(const int *)a].entry, y = F[*(const int *)b].entry; return x < y ? -1 : x > y; }
static int cmp_res(const void *a, const void *b) {
    const resent *x = a, *y = b;
    if (x->r != y->r) return x->r < y->r ? -1 : 1;
    return F[x->fi].entry < F[y->fi].entry ? -1 : F[x->fi].entry > F[y->fi].entry;   /* setdefault: first (lowest) entry wins */
}

static void add_file(rc_output *out, const char *name, sbuf *b) {
    out->files = realloc(out->files, sizeof *out->files * (out->nfiles + 1));
    rc_file *f = &out->files[out->nfiles++]; memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    if (!b->p) sb_puts(b, "");
    f->text = b->p; f->len = b->n; b->p = NULL; b->n = b->cap = 0;
}

static void reset_state(void) {
    free(aflag); free(func_at); free(vstamp); free(baked); free(vmark); free(lmark); free(smark);
    for (int i = 0; i < nF; i++) { free(F[i].visited.v); free(F[i].tail.v); }
    free(F); F = NULL; nF = capF = 0;
    for (int i = 0; i < nJT; i++) free(JT[i].targets.v);
    free(JT); JT = NULL; nJT = capJT = 0;
    free(queue); queue = NULL; qhead = qtail = qcap = 0;
    free(RES); RES = NULL; nRES = capRES = 0;
    aflag = NULL; func_at = NULL; vstamp = NULL; baked = NULL; vmark = lmark = smark = NULL;
    stamp = gstamp = 0; nbaked = 0;
}

int rc_translate(const rc_input *in, rc_output *out) {
    memset(out, 0, sizeof *out);
    decode_init();
    img = in->img; img_len = (int64_t)in->img_len; END = (int64_t)BASE + img_len;
    ovl = in->ovl; ovl_len = (int64_t)in->ovl_len; ovl_base = in->ovl_base;
    LO = ovl && ovl_base < (int64_t)BASE ? ovl_base : (int64_t)BASE;
    if (ovl && ovl_base + ovl_len > (int64_t)BASE && ovl_base < (int64_t)BASE) return 0;   /* overlapping the program: unsupported */
    if (ovl && ovl_base >= (int64_t)BASE) return 0;
    SPAN = END - LO;
    aflag = calloc((size_t)SPAN + 16, 1); func_at = calloc((size_t)SPAN + 16, sizeof *func_at); vstamp = calloc((size_t)SPAN + 16, sizeof *vstamp);
    baked = calloc((size_t)SPAN + 16, 1);
    vmark = calloc((size_t)SPAN + 16, sizeof *vmark); lmark = calloc((size_t)SPAN + 16, sizeof *lmark); smark = calloc((size_t)SPAN + 16, sizeof *smark);
    if (!aflag || !func_at || !vstamp || !baked || !vmark || !lmark || !smark) { reset_state(); return 0; }
    code_min = END; code_max = -1;
    LIVE = in->live; nLIVE = in->nlive; NAMES = in->names; nNAMES = in->nnames;

    analyze(in);

    int *order = malloc(sizeof *order * nF);
    for (int i = 0; i < nF; i++) order[i] = i;
    qsort(order, (size_t)nF, sizeof *order, cmp_func);
    sbuf b = {0};
    E(&b, "#pragma once"); E(&b, "struct sh4ctx;");
    for (int i = 0; i < nF; i++) E(&b, "void f_%08X(struct sh4ctx *c);", (uint32_t)F[order[i]].entry);
    sb_puts(&b, "\n"); add_file(out, "funcs.h", &b);
    for (int ci = 0; ci * 250 < nF; ci++) {
        sbuf s = {0};
        sb_puts(&s, "#include \"sh4ctx.h\"\n\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n");
        for (int k = ci * 250; k < nF && k < ci * 250 + 250; k++) {
            sb_puts(&s, "\n\n");
            if (!gen_function(order[k], &s)) out->unsupported++;
        }
        sb_puts(&s, "\n");
        char nm[32]; snprintf(nm, sizeof nm, "funcs_%03d.c", ci); add_file(out, nm, &s);
    }
    E(&b, "#include \"sh4ctx.h\""); E(&b, "typedef struct { uint32_t a; sh4fn f; } fnent;"); E(&b, "const fnent sh4_fn_table[] = {");
    for (int i = 0; i < nF; i++) E(&b, "  {0x%08Xu, f_%08X},", (uint32_t)F[order[i]].entry, (uint32_t)F[order[i]].entry);
    E(&b, "};"); E(&b, "const size_t sh4_fn_count = %d;", nF); sb_puts(&b, "\n"); add_file(out, "fntable.c", &b);
    qsort(RES, (size_t)nRES, sizeof *RES, cmp_res);
    E(&b, "#include \"sh4ctx.h\""); E(&b, "typedef struct { uint32_t a; sh4fn f; } fnent;"); E(&b, "const fnent sh4_resume_table[] = {");
    int nr = 0;
    for (int i = 0; i < nRES; i++) {
        if (i && RES[i].r == RES[i - 1].r) continue;
        E(&b, "  {0x%08Xu, f_%08X},", (uint32_t)RES[i].r, (uint32_t)F[RES[i].fi].entry); nr++;
    }
    E(&b, "};"); E(&b, "const size_t sh4_resume_count = %d;", nr); sb_puts(&b, "\n"); add_file(out, "resume.c", &b);
    E(&b, "#include \"sh4ctx.h\""); E(&b, "const uint32_t sh4_baked_lits[] = {");
    for (int64_t a = LO; a < END; a++) if (baked[IDX(a)]) E(&b, "  0x%08Xu,", (uint32_t)a);
    E(&b, "};"); E(&b, "const size_t sh4_baked_count = %d;", nbaked); sb_puts(&b, "\n"); add_file(out, "litmap.c", &b);
    out->nfuncs = nF; out->nbaked = nbaked;
    free(order);
    reset_state();
    return 1;
}

char *rc_gen_interp(size_t *len, int *ncases) {
    decode_init();
    /* op_c reads no program bytes for the opcodes kept here (PC-relative ones are skipped) */
    sbuf b = {0}; int n = 0;
    E(&b, "#include \"sh4ctx.h\""); E(&b, "/* generated by recomp.py gen_interp: one case per non-branch opcode */");
    E(&b, "int sh4_interp_exec(sh4ctx *c, uint16_t op) {"); E(&b, "    switch (op) {");
    const uint8_t *save_ovl = ovl; ovl = NULL;
    for (int op = 0; op < 0x10000; op++) {
        const insn *i = &dec[op];
        if (!i->ok || i->kind) continue;
        if ((op & 0xF000) == 0x9000 || (op & 0xF000) == 0xD000 || (op & 0xFF00) == 0xC700) continue;
        const char *s = op_c(i, 0x8C010000);
        if (!s) continue;
        E(&b, "    case 0x%04X: %s return 0;", op, s); n++;
    }
    ovl = save_ovl;
    E(&b, "    default: return -1;"); E(&b, "    }"); E(&b, "}"); sb_puts(&b, "\n");
    if (len) *len = b.n;
    if (ncases) *ncases = n;
    return b.p;
}

void rc_free(rc_output *out) {
    for (int i = 0; i < out->nfiles; i++) free(out->files[i].text);
    free(out->files); memset(out, 0, sizeof *out);
}

/* ------------------------------------------------------------------ notes files */
int rc_parse_hex_list(const char *t, uint32_t **out) {
    int n = 0, cap = 64; uint32_t *v = malloc(sizeof *v * cap);
    while (t && *t) {
        const char *e = strchr(t, '\n'); size_t l = e ? (size_t)(e - t) : strlen(t);
        char line[256]; if (l >= sizeof line) l = sizeof line - 1; memcpy(line, t, l); line[l] = 0;
        char *hsh = strchr(line, '#'); if (hsh) *hsh = 0;
        char *p = line; while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p) { if (n == cap) v = realloc(v, sizeof *v * (cap *= 2)); v[n++] = (uint32_t)strtoul(p, NULL, 16); }
        t = e ? e + 1 : t + l;
        if (!e) break;
    }
    *out = v; return n;
}
int rc_parse_names(const char *t, rc_name **out) {
    int n = 0, cap = 64; rc_name *v = malloc(sizeof *v * cap);
    while (t && (t = strchr(t, '"'))) {
        const char *ke = strchr(t + 1, '"'); if (!ke) break;
        const char *vs = strchr(ke + 1, '"'); if (!vs) break;
        const char *ve = strchr(vs + 1, '"'); if (!ve) break;
        char key[32]; size_t kl = (size_t)(ke - t - 1); if (kl >= sizeof key) kl = sizeof key - 1; memcpy(key, t + 1, kl); key[kl] = 0;
        char *nm = malloc((size_t)(ve - vs)); memcpy(nm, vs + 1, (size_t)(ve - vs - 1)); nm[ve - vs - 1] = 0;
        if (n == cap) v = realloc(v, sizeof *v * (cap *= 2));
        v[n].addr = (uint32_t)strtoul(key, NULL, 16); v[n].name = nm; n++;
        t = ve + 1;
    }
    *out = v; return n;
}
/* {"address": "8C00F400", "size": 3072, "rows": [[256, "bin", 221104], [304, "hex", "6461..."], ...]} */
uint8_t *rc_build_overlay(const char *j, const uint8_t *im, size_t imlen, uint32_t *base, size_t *len) {
    const char *p = strstr(j, "\"address\""); if (!p) return NULL;
    p = strchr(p + 9, '"'); if (!p) return NULL;
    *base = (uint32_t)strtoul(p + 1, NULL, 16);
    p = strstr(j, "\"size\""); if (!p) return NULL;
    p = strchr(p, ':'); if (!p) return NULL;
    size_t size = strtoul(p + 1, NULL, 10);
    uint8_t *o = calloc(size ? size : 1, 1);
    p = strstr(j, "\"rows\""); if (!p) { free(o); return NULL; }
    p = strchr(p, '['); if (!p) { free(o); return NULL; }
    p++;
    for (;;) {
        p = strchr(p, '['); if (!p) break;
        char *e; size_t a = strtoul(p + 1, &e, 10);
        const char *k = strchr(e, '"'); if (!k) break;
        int isbin = !strncmp(k + 1, "bin", 3);
        const char *ve = strchr(k + 1, '"'); if (!ve) break;
        const char *q = ve + 1; while (*q == ',' || *q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
        if (a + 16 > size) break;
        if (isbin) { size_t v = strtoul(q, (char **)&q, 10); if (v + 16 <= imlen) memcpy(o + a, im + v, 16); }
        else {
            q++;                                            /* opening quote */
            for (int i = 0; i < 16 && q[0] && q[1] && q[0] != '"'; i++, q += 2) { char hx[3] = {q[0], q[1], 0}; o[a + i] = (uint8_t)strtoul(hx, NULL, 16); }
        }
        p = strchr(q, ']'); if (!p) break;
        p++;
    }
    *len = size;
    return o;
}
