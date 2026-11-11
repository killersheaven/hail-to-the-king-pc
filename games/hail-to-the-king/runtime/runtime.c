/* Minimal host runtime: RAM, dispatch, CPU-system helpers, MMIO logging. */
#include "sh4ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include "platform.h"
void hw_report(void);
extern uint64_t irq_count;
extern uint8_t *dc_ram;
uint32_t sh4_resume_pc;
static void on_term(int sig) { (void)sig; { void dma_report(void); dma_report(); } hw_report();
    if (getenv("DC_DUMPRAM")) { extern uint8_t *dc_ram; FILE *d = fopen(getenv("DC_DUMPRAM"), "wb"); if (d) { fwrite(dc_ram, 1, 16 << 20, d); fclose(d); } }
    fprintf(stderr, "interrupts delivered: %llu\n", (unsigned long long)irq_count); _exit(2); }

uint8_t *dc_ram;
typedef struct { uint32_t a; sh4fn f; } fnent;
extern const fnent sh4_fn_table[];
extern const size_t sh4_fn_count;


static void final_report(void);
static void halt(const char *why, uint32_t pc) {
    fprintf(stderr, "[halt] %s at %08X\n", why, pc);
    final_report();
    exit(1);
}
void hle_halt(const char *why, uint32_t a) { halt(why, a); }

int bios_hle_dispatch(sh4ctx *c, uint32_t a);
void bios_hle_install(void);
int disc_open(const char *dir);
void disc_report(void);
void hle_halt(const char *why, uint32_t a);

int sh4_known_target(uint32_t t) {
    uint32_t key = (t & 0x1FFFFFFFu) | 0x80000000u;
    size_t lo = 0, hi = sh4_fn_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (sh4_fn_table[mid].a < key) lo = mid + 1; else hi = mid; }
    return lo < sh4_fn_count && sh4_fn_table[lo].a == key;
}
static uint32_t recent_t[64], recent_pr[64]; static unsigned recent_n;
void sh4_dispatch(sh4ctx *c, uint32_t t) {
    recent_t[recent_n & 63] = t; recent_pr[recent_n & 63] = c->pr; recent_n++;
    if (bios_hle_dispatch(c, t)) return;
    {   static int force = -1; if (force < 0) force = getenv("DC_FORCE_INTERP") != NULL;   /* test: interpret every indirect target */
        if (force && is_ram(t) && t != 0x8C0AF7B4u && !sh4_resume_pc) { void sh4_interp(sh4ctx *c, uint32_t pc); sh4_interp(c, t); return; } }
    size_t lo = 0, hi = sh4_fn_count;
    uint32_t key = t | 0x80000000u; /* treat P1/P2 mirrors alike */
    key = (key & 0x1FFFFFFFu) | 0x80000000u;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (sh4_fn_table[mid].a < key) lo = mid + 1; else hi = mid;
    }
    if (lo < sh4_fn_count && sh4_fn_table[lo].a == key) { sh4_fn_table[lo].f(c); return; }
    {   /* a return address inside a function: enter that function there */
        extern const fnent sh4_resume_table[]; extern const size_t sh4_resume_count;
        size_t l = 0, h = sh4_resume_count;
        while (l < h) { size_t m = (l + h) / 2; if (sh4_resume_table[m].a < key) l = m + 1; else h = m; }
        if (l < sh4_resume_count && sh4_resume_table[l].a == key) { sh4_resume_pc = key; sh4_resume_table[l].f(c); return; }
    }
    if (is_ram(t)) {                    /* code the analysis never found: interpret it, and note it for the next build */
        static uint32_t seen[512]; static int ns; int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == t) { known = 1; break; }
        if (!known) {
            if (ns < 512) seen[ns++] = t;
            fprintf(stderr, "[dispatch] no recompiled code at %08X (pr=%08X): interpreting\n", t, c->pr);
            FILE *m = fopen("missing_entries.txt", "a"); if (m) { fprintf(m, "%08X\n", t); fclose(m); }
        }
        void sh4_interp(sh4ctx *c, uint32_t pc);
        sh4_interp(c, t);
        return;
    }
    fprintf(stderr, "[dispatch] no function at %08X (pr=%08X)\n", t, c->pr);
    { FILE *m = fopen("missing_entries.txt", "a"); if (m) { fprintf(m, "%08X\n", t); fclose(m); } }
    halt("unknown indirect target", t);
}

extern const uint32_t sh4_baked_lits[];
extern const size_t sh4_baked_count;
void sh4_code_write(uint32_t a) {
    uint32_t key = ((a & 0x00FFFFFFu) | 0x8C000000u) & ~3u;
    size_t lo = 0, hi = sh4_baked_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (sh4_baked_lits[mid] < key) lo = mid + 1; else hi = mid; }
    if (lo < sh4_baked_count && sh4_baked_lits[lo] == key) {
        static uint32_t seen[256]; static int ns;
        for (int i = 0; i < ns; i++) if (seen[i] == key) return;
        if (ns < 256) seen[ns++] = key;
        fprintf(stderr, "[lit] program wrote to baked literal %08X -> will read it live after rebuild\n", key);
        FILE *m = fopen("live_literals.txt", "a"); if (m) { fprintf(m, "%08X\n", key); fclose(m); }
    }
}

/* An interrupt that arrived while masked (IMASK or BL) is taken on real hardware
 * the moment the mask drops. Polls are counter-based, so a loop whose period
 * lines up with the poll counter could otherwise always poll inside its masked
 * section and never take the interrupt: poll again right after the unmask. */
static uint32_t irq_deferred_lvl;
static int poll_start = 4096, poll_credit;      /* back-edges since the last slow poll (deterministic clock) */
void sh4_set_sr(sh4ctx *c, uint32_t v) {
    if (irq_deferred_lvl && !(v & 0x10000000u) && ((v >> 4) & 15) < irq_deferred_lvl) { irq_deferred_lvl = 0; poll_credit += poll_start - sh4_poll_ctr; poll_start = sh4_poll_ctr = 1; }
    uint32_t old_rb = c->sr_rest & 0x20000000u, old_md = c->sr_rest & 0x40000000u;
    uint32_t new_rb = v & 0x20000000u, new_md = v & 0x40000000u;
    int old_bank = old_md && old_rb, new_bank = new_md && new_rb;
    if (old_bank != new_bank)
        for (int i = 0; i < 8; i++) { uint32_t x = c->r[i]; c->r[i] = c->rbank[i]; c->rbank[i] = x; }
    c->t = v & 1; c->s = (v >> 1) & 1; c->q = (v >> 8) & 1; c->m_ = (v >> 9) & 1;
    c->sr_rest = v & 0x700083F0u;
}

void sh4_trapa(sh4ctx *c, uint32_t imm, uint32_t pc) { (void)c; fprintf(stderr, "[trapa] #%02X at %08X\n", imm, pc); }
void sh4_sleep(sh4ctx *c, uint32_t pc) { (void)c; (void)pc; /* TODO: advance to next interrupt */ }
#define SPC_RETURN_TO_HOST 0xFFFFFFF0u

/* ---- guest threads ----
 * Interrupt entry records the interrupted thread's stack pointer. An RTE that
 * goes back to "the interrupted code" (SPC sentinel) with a different stack
 * pointer resumes the thread that was interrupted with that stack pointer; an
 * RTE to a real address starts a new thread there. */
#include "coro.h"
enum { T_FREE, T_RUNNING, T_SUSPENDED, T_ZOMBIE };
#include <setjmp.h>
typedef struct { coro_t *co; uint32_t key_sp, start_pc; int state, in_irq, resume_irq; jmp_buf irq_jb; } gthread;
#define NTHREADS 32
static gthread th[NTHREADS];
static int cur_t, n_t = 1;
static sh4ctx *gctx;
uint64_t thread_switches;

/* A thread starts (or resumes after a voluntary switch) at a PC with its registers
 * already loaded. When that function returns, continue at the guest return
 * address, so a thread resumed in the middle of a call chain unwinds normally. */
static void thread_entry(void *arg) {
    int me = (int)(intptr_t)arg;
    th[me].in_irq = 0;
    uint32_t pc = th[me].start_pc;
    for (;;) { sh4_dispatch(gctx, pc); pc = gctx->pr; }
}
static int ttrace = -1;
static void tdump(const char *what, int from, int to, sh4ctx *c) {
    if (ttrace < 0) ttrace = getenv("DC_THREADTRACE") != NULL;
    if (!ttrace) return;
    fprintf(stderr, "[tt] %s %d->%d pc %08X pr %08X r8-15 %08X %08X %08X %08X %08X %08X %08X %08X sr %08X irq %d\n", what, from, to,
            c->spc, c->pr, c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], c->r[15], sh4_get_sr(c), th[from].in_irq);
}
static void switch_to(int i, int as_irq_return) {
    tdump(as_irq_return ? "resume-irq" : "start", cur_t, i, gctx);
    /* a thread that switched away voluntarily (not from an interrupt) will be resumed
     * by an RTE to its saved PC on a fresh host stack: this one is no longer needed */
    th[cur_t].state = th[cur_t].in_irq ? T_SUSPENDED : T_ZOMBIE;
    cur_t = i; th[i].state = T_RUNNING;
    thread_switches++;
    th[i].resume_irq = as_irq_return;
    coro_switch(th[i].co);
    /* we were switched back to: an interrupted thread finishes its interrupt */
    if (th[cur_t].resume_irq) { th[cur_t].resume_irq = 0; longjmp(th[cur_t].irq_jb, 1); }
}
/* continue at pc with the registers already in c, on a fresh host stack */
static void start_context(sh4ctx *c, uint32_t pc, uint32_t from) {
    int n = -1;
    for (int i = 1; i < n_t; i++) if (th[i].state == T_ZOMBIE && i != cur_t) { n = i; coro_free(th[i].co); break; }
    if (n < 0) { if (n_t >= NTHREADS) halt("too many guest threads", pc); n = n_t++; }
    gctx = c;
    th[n].start_pc = pc; th[n].state = T_SUSPENDED; th[n].in_irq = 0; th[n].key_sp = 0; th[n].resume_irq = 0;
    th[n].co = coro_new(thread_entry, (void *)(intptr_t)n, 4u << 20);
    if (!th[n].co) halt("cannot create host stack", pc);
    static int logged; if (logged++ < 20) fprintf(stderr, "[thread] context #%d starts at %08X (sp %08X), from %08X\n", n, pc, c->r[15], from);
    switch_to(n, 0);
}
/* CRI middleware's longjmp (context restore): registers from the buffer at r4, return value r5 */
void hle_ctx_restore(sh4ctx *c) {
    uint32_t b = c->r[4], val = c->r[5];
    W32(0x8C6F08D0u, val);
    /* layout written by the matching save routine (8C0AFA34):
     * +0 r15 +4 r14 +8 pr +12 r13 ... +48 r4, +52 r3 +56 r2 +60 r1 +64 r0, +68 mach +72 macl +76 gbr,
     * +80..+140 fr0-fr15, +144 fpul, +148 fpscr */
    for (int i = 0; i < 16; i++) c->fr.u[i] = R32(b + 80 + 4 * i);
    c->fpul = R32(b + 144); sh4_set_fpscr(c, R32(b + 148));
    c->gbr = R32(b + 76); c->macl = R32(b + 72); c->mach = R32(b + 68);
    c->r[1] = R32(b + 60); c->r[2] = R32(b + 56); c->r[3] = R32(b + 52);
    c->r[5] = R32(b + 44); c->r[6] = R32(b + 40); c->r[7] = R32(b + 36);
    c->r[8] = R32(b + 32); c->r[9] = R32(b + 28); c->r[10] = R32(b + 24); c->r[11] = R32(b + 20);
    c->r[12] = R32(b + 16); c->r[13] = R32(b + 12); c->r[14] = R32(b + 4);
    c->r[4] = R32(b + 48);
    c->r[15] = R32(b + 0); c->pr = R32(b + 8);
    c->r[0] = val ? val : 1;
    start_context(c, c->pr, 0x8C0AF7B4u);
}
void sh4_rte(sh4ctx *c, uint32_t pc) {
    uint32_t spc = c->spc;
    sh4_set_sr(c, c->ssr);
    if (spc == SPC_RETURN_TO_HOST) {
        if (th[cur_t].in_irq && c->r[15] == th[cur_t].key_sp) return;   /* ordinary return from interrupt */
        for (int i = 0; i < n_t; i++)
            if (th[i].state == T_SUSPENDED && th[i].in_irq && th[i].key_sp == c->r[15]) { switch_to(i, 1); return; }
        static int warned;
        if (warned++ < 5) fprintf(stderr, "[rte] resume with unknown stack %08X (current thread %d had %08X)\n", c->r[15], cur_t, th[cur_t].key_sp);
        return;
    }
    start_context(c, spc, pc);
}

/* ---- interrupts ---- */
int sh4_poll_ctr = 4096;
uint32_t hw_pending_irq(sh4ctx *c, uint32_t *intevt);   /* from hw.c: returns level 0..15 */
void hw_set_intevt(uint32_t v);
uint64_t irq_count;
void aica_step(uint64_t now_ns);
uint64_t hw_now_ns(void);
static uint64_t poll_stat[4]; static uint32_t poll_masked_pc;
/* freeze detector: the CPU keeps polling but no interrupt has been taken for 2 s */
static void hang_check(sh4ctx *c, uint32_t lvl, uint32_t intevt) {
    static uint64_t last_irqs, last_change_ns; static int reported;
    uint64_t now = hw_now_ns();
    if (irq_count != last_irqs) { last_irqs = irq_count; last_change_ns = now; reported = 0; memset(poll_stat, 0, sizeof poll_stat); return; }
    if (reported || now - last_change_ns < 2000000000ull) return;
    reported = 1;
    uint32_t sr = sh4_get_sr(c);
    fprintf(stderr, "[hang] no interrupt taken for 2 s: thread %d in_irq %d, sr %08X (imask %u bl %u), pending level %u intevt %03X\n",
            cur_t, th[cur_t].in_irq, sr, (sr >> 4) & 15, (sr >> 28) & 1, lvl, intevt);
    fprintf(stderr, "[hang] r0-7 %08X %08X %08X %08X %08X %08X %08X %08X\n", c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    fprintf(stderr, "[hang] r8-15 %08X %08X %08X %08X %08X %08X %08X %08X pr %08X spc %08X ssr %08X\n",
            c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], c->r[15], c->pr, c->spc, c->ssr);
    for (int i = 0; i < n_t; i++) fprintf(stderr, "[hang] thread %d: state %d in_irq %d key_sp %08X start %08X\n", i, th[i].state, th[i].in_irq, th[i].key_sp, th[i].start_pc);
    fprintf(stderr, "[hang] polls: in_irq %llu bl %llu none %llu masked %llu (last masked pr %08X)\n", (unsigned long long)poll_stat[0], (unsigned long long)poll_stat[1], (unsigned long long)poll_stat[2], (unsigned long long)poll_stat[3], poll_masked_pc);
    fprintf(stderr, "[hang] recent indirect calls (oldest first):");
    for (unsigned k = 0; k < 64; k++) { unsigned i = (recent_n + k) & 63; if (recent_t[i]) fprintf(stderr, " %08X<%08X", recent_t[i], recent_pr[i]); }
    fprintf(stderr, "\n");
    fflush(stderr);
}
extern uint64_t hw_vclock_ns, hw_vclock_step;
void sh4_poll_slow(sh4ctx *c) {
    if (hw_vclock_step) {
        int n = poll_credit + poll_start - (sh4_poll_ctr < 0 ? 0 : sh4_poll_ctr);
        hw_vclock_ns += hw_vclock_step * (uint64_t)n / 4096;
    }
    poll_credit = 0; poll_start = sh4_poll_ctr = 4096;
    aica_step(hw_now_ns());
    { uint32_t ev = 0, lv = hw_pending_irq(c, &ev); hang_check(c, lv, ev); }
    gthread *t = &th[cur_t];
    if (t->in_irq) { poll_stat[0]++; return; }
    uint32_t sr = sh4_get_sr(c);
    uint32_t intevt, lvl = hw_pending_irq(c, &intevt);
    if (!lvl) { poll_stat[2]++; return; }
    if (sr & 0x10000000u) { poll_stat[1]++; irq_deferred_lvl = lvl; return; }                 /* BL */
    if (lvl <= ((sr >> 4) & 15)) { poll_stat[3]++; poll_masked_pc = c->pr; irq_deferred_lvl = lvl; return; }   /* IMASK */
    irq_deferred_lvl = 0;
    /* SH-4 interrupt acceptance */
    c->ssr = sr; c->spc = SPC_RETURN_TO_HOST; c->sgr = c->r[15];
    t->key_sp = c->r[15];
    hw_set_intevt(intevt);
    sh4_set_sr(c, sr | 0x70000000u);              /* MD=1 RB=1 BL=1 */
    t->in_irq = 1; irq_count++;
    static int first = 1;
    if (first) { first = 0; fprintf(stderr, "[irq] first interrupt INTEVT %03X, VBR %08X -> handler %08X\n", intevt, c->vbr, c->vbr + 0x600); }
    uint32_t save_r[16]; memcpy(save_r, c->r, sizeof save_r); uint32_t save_pr = c->pr, save_sw = (uint32_t)thread_switches;
    if (setjmp(t->irq_jb) == 0) sh4_dispatch(c, c->vbr + 0x600);
    t = &th[cur_t];
    th[cur_t].in_irq = 0;                         /* cur_t: we may have been switched away and back */
    if ((uint32_t)thread_switches == save_sw) {
        static int warned;
        for (int i = 0; i < 16 && warned < 12; i++) if (c->r[i] != save_r[i])
            { warned++; fprintf(stderr, "[irq] #%llu INTEVT %03X: r%d changed %08X -> %08X (pr %08X -> %08X, sr %08X)\n", (unsigned long long)irq_count, intevt, i, save_r[i], c->r[i], save_pr, c->pr, sh4_get_sr(c)); }
    }
}
void sh4_unreachable(sh4ctx *c, uint32_t pc) { (void)c; halt("fell into unreachable code", pc); }
void sh4_ldtlb(sh4ctx *c) { (void)c; }
extern uint64_t sq_flushes, sq_ta_flushes;
void hw_sq_flush(uint32_t a);
void sh4_sq_flush(sh4ctx *c, uint32_t a) { (void)c; hw_sq_flush(a); }

#ifdef __ANDROID__
#include <SDL2/SDL_main.h>                   /* SDL's Java side calls SDL_main */
void android_prepare(int *argc, char ***argv);   /* platform_sdl.c: app folder, disc, assets */
#endif
int main(int argc, char **argv) {
#ifdef __ANDROID__
    android_prepare(&argc, &argv);
#endif
    /* hail-to-the-king <disc.cue>                 (the program is read from the disc)
     * hail-to-the-king <1ST_READ.BIN> <disc.cue | folder>   (development) */
    const char *a1 = argc > 1 && argv[1][0] != '-' ? argv[1] : "1ST_READ.BIN";
    size_t l1 = strlen(a1);
    int cue_only = l1 > 4 && (!strcmp(a1 + l1 - 4, ".cue") || !strcmp(a1 + l1 - 4, ".CUE"));
    const char *path = cue_only ? NULL : a1;
    const char *disc = cue_only ? a1 : argc > 2 && argv[2][0] != '-' ? argv[2] : ".";
    dc_ram = calloc(1, 16 << 20);
    if (getenv("DC_VCLOCK")) { hw_vclock_step = strtoull(getenv("DC_VCLOCK"), NULL, 10); if (!hw_vclock_step) hw_vclock_step = 200000; fprintf(stderr, "[time] deterministic clock: %llu ns per poll\n", (unsigned long long)hw_vclock_step); }
    disc_open(disc);
    size_t n = 0;
    if (path) {
        FILE *f = fopen(path, "rb");
        if (!f) { perror(path); return 1; }
        n = fread(dc_ram + 0x10000, 1, (16 << 20) - 0x10000, f); fclose(f);
    } else {
        uint8_t *iso_load(const char *p, uint32_t *size_out); uint32_t sz = 0;
        uint8_t *b = iso_load("1ST_READ.BIN", &sz);
        if (!b || sz > (16u << 20) - 0x10000) { fprintf(stderr, "cannot read 1ST_READ.BIN from %s\n", disc); return 1; }
        memcpy(dc_ram + 0x10000, b, sz); free(b); n = sz;
    }
    fprintf(stderr, "loaded %zu bytes at 8C010000, %zu recompiled functions\n", n, sh4_fn_count);
    { void text_patch(uint32_t a, uint32_t len); text_patch(0x8C010000u, (uint32_t)n); }   /* memory-card wording -> save slots */
    {   int disc_read_ip(uint8_t *dst32k);
        if (disc_read_ip(dc_ram + 0x8000)) fprintf(stderr, "IP.BIN: read from the disc image\n");
        else {
            char ip[1024]; snprintf(ip, sizeof ip, "%s/IP.BIN", disc);
            FILE *g = fopen(ip, "rb");
            if (g) { size_t k = fread(dc_ram + 0x8000, 1, 0x8000, g); fclose(g); fprintf(stderr, "IP.BIN: %zu bytes at 8C008000\n", k); }
        } }
    bios_hle_install();
    { void hw_init(void); hw_init(); }
    if (getenv("DC_ISO_LIST")) { void iso_list(void); iso_list(); return 0; }
    if (!platform_init(argc, argv)) fprintf(stderr, "[platform] init failed; running without video\n");
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    static sh4ctx c;
    c.r[15] = 0x8C00F400u;
    sh4_set_sr(&c, 0x600000F0u);
    c.fpscr = 0x00040001u;
    c.vbr = 0x8C00F400u;
    th[0].co = coro_main(); th[0].state = T_RUNNING; gctx = &c;
    f_8C010000(&c);
    fprintf(stderr, "entry returned\n");
    final_report();
    return 0;
}
static void final_report(void) {
    hw_report();
    { void dma_report(void); dma_report(); }
    disc_report();
    if (getenv("DC_DUMPRAM")) { FILE *d = fopen(getenv("DC_DUMPRAM"), "wb"); if (d) { fwrite(dc_ram, 1, 16 << 20, d); fclose(d); } }
    fprintf(stderr, "interrupts delivered: %llu, guest threads %d, thread switches %llu\n", (unsigned long long)irq_count, n_t, (unsigned long long)thread_switches);
}
