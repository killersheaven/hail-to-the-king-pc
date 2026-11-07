/* Dreamcast hardware register model (first pass).
 * - generic register file: writes are stored, reads return the last write
 * - SH-4 TMU (timer unit) counting from the host clock
 * - PVR SPG status (scanline / vblank) from the host clock
 */
#include "sh4ctx.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "pvr.h"
#include "platform.h"
uint32_t aica_read(uint32_t a, int size);
void aica_write(uint32_t a, uint32_t v, int size);
void sq_store(uint32_t a, uint32_t v, int size);
void bus_write_block(uint32_t a, const uint8_t *src, uint32_t len);
int dma_reg_write(uint32_t a, uint32_t v);

uint64_t pvr_render_count, pvr_ta_init;
extern uint64_t sq_flushes, sq_ta_flushes;
/* ---------- time ---------- */
static uint64_t t0_ns;
/* DC_VCLOCK=<ns>: deterministic test mode. Time advances by <ns> at every slow
 * interrupt poll (every 4096 loop back-edges) instead of following the host
 * clock, so scripted-input runs repeat exactly regardless of host speed. */
uint64_t hw_vclock_ns, hw_vclock_step;
static uint64_t now_ns(void) {
    if (hw_vclock_step) return hw_vclock_ns;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t n = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    if (!t0_ns) t0_ns = n;
    return n - t0_ns;
}
uint64_t hw_now_ns(void) { return now_ns(); }
/* SH-4 peripheral clock Pφ = 50 MHz */
#define PCLK_HZ 50000000ull

/* ---------- generic register storage (open addressing) ---------- */
#define REGN 8192
static uint32_t rk[REGN], rv[REGN]; static uint8_t ru[REGN];
static uint32_t reg_used, reg_dummy;
static uint32_t *reg_slot(uint32_t a) {
    uint32_t h = (a * 2654435761u) & (REGN - 1);
    while (ru[h] && rk[h] != a) h = (h + 1) & (REGN - 1);
    if (!ru[h]) {
        if (reg_used >= REGN * 3 / 4) {            /* table full: something is writing a memory range we do not model */
            static int warned; if (warned++ < 10) fprintf(stderr, "[mmio] register table full; unmodelled access at %08X\n", a);
            return &reg_dummy;
        }
        reg_used++; ru[h] = 1; rk[h] = a; rv[h] = 0;
    }
    return &rv[h];
}
uint32_t hw_reg(uint32_t a) { return *reg_slot(a); }
void hw_set_reg(uint32_t a, uint32_t v) { *reg_slot(a) = v; }
/* canonicalize mirrors: Holly/PVR/AICA registers appear at 0x005Fxxxx etc. through P1/P2/P0 */
static uint32_t canon(uint32_t a) { return a >= 0xE0000000u ? a : (a & 0x1FFFFFFFu); }

/* ---------- TMU ---------- */
static struct { uint32_t tcor, tcnt_base; uint64_t start_ns; uint16_t tcr; int running; } tmu[3];
static uint8_t tstr;
static uint32_t tmu_div(int ch) { static const uint32_t d[8] = {4, 16, 64, 256, 1024, 1024, 1024, 1024}; return d[tmu[ch].tcr & 7]; }
static uint32_t tmu_ticks(int ch) {
    if (!tmu[ch].running) return 0;
    return (uint32_t)(((now_ns() - tmu[ch].start_ns) * (PCLK_HZ / tmu_div(ch))) / 1000000000ull);
}
static uint32_t tmu_tcnt(int ch) {
    uint32_t t = tmu_ticks(ch), base = tmu[ch].tcnt_base;
    if (!tmu[ch].running) return base;
    if (t <= base) return base - t;
    uint64_t period = (uint64_t)tmu[ch].tcor + 1;
    return tmu[ch].tcor - (uint32_t)((t - base - 1) % period);
}
static int tmu_reg(uint32_t a, int *ch) {
    if (a < 0xFFD80008u || a > 0xFFD8002Fu) return -1;
    uint32_t o = a - 0xFFD80008u; *ch = o / 12; return o % 12;  /* 0 TCOR, 4 TCNT, 8 TCR */
}
static void tmu_start_stop(uint8_t v) {
    for (int ch = 0; ch < 3; ch++) {
        int run = (v >> ch) & 1;
        if (run && !tmu[ch].running) { tmu[ch].running = 1; tmu[ch].start_ns = now_ns(); }
        else if (!run && tmu[ch].running) { tmu[ch].tcnt_base = tmu_tcnt(ch); tmu[ch].running = 0; }
    }
    tstr = v;
}

/* ---------- PVR SPG (NTSC 480i timing approximation) ---------- */
#define LINES_PER_FIELD 263
#define FIELD_NS 16683333ull       /* 59.94 Hz */
static uint32_t spg_status(void) {
    uint64_t t = now_ns() % FIELD_NS;
    uint32_t line = (uint32_t)(t * LINES_PER_FIELD / FIELD_NS);
    uint32_t vsync = line >= 240 ? 1 : 0;          /* vertical blank region */
    uint32_t field = (uint32_t)((now_ns() / FIELD_NS) & 1);
    return line | (field << 10) | (vsync << 13);
}
uint64_t dc_frame_count(void) { return now_ns() / FIELD_NS; }

/* ---------- Holly interrupt controller ---------- */
static uint32_t istnrm, istext, isterr;
void hw_raise_nrm(int bit) { istnrm |= 1u << bit; }
void hw_set_ext(int bit, int on) { if (on) istext |= 1u << bit; else istext &= ~(1u << bit); }
static uint64_t last_field;
static uint32_t intevt_reg;
void hw_set_intevt(uint32_t v) { intevt_reg = v; }
/* Testing cheats (settings.ini cheat_* / DC_CHEATS bitmask), from the published
 * Dreamcast CodeBreaker codes for this game, applied every display field:
 * 1 immortality (health 100), 2 unlimited pistol ammo (999), 4 unlimited
 * chainsaw fuel, 8 Damascene Steel chainsaw blade. Only while the player
 * record looks valid (health 1..100). */
int hw_cheats = -1;
static void apply_cheats(void) {
    if (hw_cheats < 0) hw_cheats = getenv("DC_CHEATS") ? atoi(getenv("DC_CHEATS")) : 0;
    if (!hw_cheats) return;
    uint32_t hp = R32(0x8C7AD778u);
    if (hp < 1 || hp > 100) return;
    if (hw_cheats & 1) W16(0x8C7AD778u, 100);
    if (hw_cheats & 2) W16(0x8C7AE2CCu, 999);
    if (hw_cheats & 4) W16(0x8C7AE3D6u, 0xFF);
    if (hw_cheats & 8) W16(0x8C7AE3D2u, 0x0D);
}
static void hw_status(uint64_t field) {
    extern uint64_t irq_count, arm7_insns, yuv_frames, thread_switches, maple_frames;
    fprintf(stderr, "[status] %3llus: renders %llu, TA lists %llu, interrupts %llu, sound CPU %llu Minsn, movie frames %llu, thread switches %llu, pad polls %llu\n",
            (unsigned long long)(field / 60), (unsigned long long)pvr_render_count, (unsigned long long)pvr_ta_init,
            (unsigned long long)irq_count, (unsigned long long)(arm7_insns / 1000000), (unsigned long long)yuv_frames,
            (unsigned long long)thread_switches, (unsigned long long)maple_frames);
    /* DC_PEEK=addr,addr,...: print 16-bit RAM values with each status line (debugging) */
    static const char *pk = (const char *)1; if (pk == (const char *)1) pk = getenv("DC_PEEK");
    if (pk) { fprintf(stderr, "[peek]"); for (const char *q = pk; *q; ) { char *e; uint32_t a = (uint32_t)strtoul(q, &e, 16); if (e == q) break;
        fprintf(stderr, " %08X=%u", a, R16(a)); q = *e == ',' ? e + 1 : e; } fprintf(stderr, "\n"); }
}
static void holly_update_events(void) {
    uint64_t f = now_ns() / FIELD_NS;
    if (f != last_field) { last_field = f; istnrm |= (1u << 3) | (1u << 4); apply_cheats();
        { static long ef = -2; if (ef == -2) ef = getenv("DC_EXIT_FIELD") ? atol(getenv("DC_EXIT_FIELD")) : -1;   /* test: stop at a field, dumping RAM */
          if (ef >= 0 && (long)f >= ef) { if (getenv("DC_DUMPRAM")) { FILE *d = fopen(getenv("DC_DUMPRAM"), "wb"); if (d) { fwrite(dc_ram, 1, 16 << 20, d); fclose(d); } }
                                         fprintf(stderr, "[exit] field %llu\n", (unsigned long long)f); fflush(stderr); _exit(0); } }
        platform_vblank(); if (f % 300 == 0) hw_status(f); }  /* VBlank in / out */
}
static uint32_t ist_nrm_read(void) {
    return (istnrm & 0x3FFFFFFFu) | (istext ? 0x40000000u : 0) | (isterr ? 0x80000000u : 0);
}
/* TMU underflow interrupt state */
static uint32_t tmu_unf_seen[3];
static void tmu_update_events(void) {
    for (int ch = 0; ch < 3; ch++) {
        if (!tmu[ch].running) continue;
        uint32_t t = tmu_ticks(ch), base = tmu[ch].tcnt_base;
        uint32_t unf = t <= base ? 0 : (uint32_t)(1 + (uint64_t)(t - base - 1) / ((uint64_t)tmu[ch].tcor + 1));
        if (unf != tmu_unf_seen[ch]) { tmu_unf_seen[ch] = unf; tmu[ch].tcr |= 0x100; }
    }
}
/* power-on register values the software checks */
void hw_init(void) {
    *reg_slot(0x005F8000u) = 0x17FD11DBu;   /* PVR ID */
    *reg_slot(0x005F8004u) = 0x00000011u;   /* PVR revision (later Holly) */
    *reg_slot(0xFF800030u) = 0x00000000u;   /* PDTRA bits 8-9: cable type 0 = VGA */
}

uint32_t hw_pending_irq(sh4ctx *c, uint32_t *intevt) {
    (void)c;
    holly_update_events(); tmu_update_events();
    uint32_t nrm = ist_nrm_read(), best = 0, ev = 0;
    static const struct { uint32_t lvl, ev, nrm, ext, err; } L[3] = {
        {6, 0x320, 0x005F6930, 0x005F6934, 0x005F6938},
        {4, 0x360, 0x005F6920, 0x005F6924, 0x005F6928},
        {2, 0x3A0, 0x005F6910, 0x005F6914, 0x005F6918}};
    for (int i = 0; i < 3; i++) {
        if ((nrm & *reg_slot(L[i].nrm)) || (istext & *reg_slot(L[i].ext)) || (isterr & *reg_slot(L[i].err))) {
            if (L[i].lvl > best) { best = L[i].lvl; ev = L[i].ev; }
        }
    }
    /* TMU0-2: priority in IPRA bits 15-12, 11-8, 7-4 */
    uint32_t ipra = *reg_slot(0xFFD00004u);
    for (int ch = 0; ch < 3; ch++) {
        uint32_t pri = (ipra >> (12 - 4 * ch)) & 15;
        if ((tmu[ch].tcr & 0x120) == 0x120 && pri > best) { best = pri; ev = 0x400 + 0x20 * ch; }
    }
    *intevt = ev;
    return best;
}

/* ---------- VRAM: 8MB, 0x05xxxxxx linear (32-bit path), 0x04xxxxxx interleaved (64-bit path) ---------- */
uint8_t vram[8 << 20];
uint32_t vram_gen[2048];
int vram_off(uint32_t a, uint32_t *off) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (a >= 0xE0000000u) return 0;
    if ((p & 0xFF000000u) == 0x05000000u || (p & 0xFF000000u) == 0x07000000u) { *off = p & 0x7FFFFFu; return 1; }
    if ((p & 0xFF000000u) == 0x04000000u || (p & 0xFF000000u) == 0x06000000u) {
        uint32_t A = p & 0x7FFFFFu;
        *off = ((A & 0x7FFFF8u) >> 1) | (A & 3u) | (((A >> 2) & 1u) << 22);
        return 1;
    }
    return 0;
}

/* ---------- logging ---------- */
static int log_budget = 400; static int log_all = -1;
static uint32_t hk[REGN], hc[REGN];
static void unk(const char *k, uint32_t a, uint32_t v) {
    if (a >= 0xF0000000u && a < 0xF8000000u) return;   /* cache / TLB address arrays */
    uint32_t h = (a * 2654435761u) & (REGN - 1);
    while (hc[h] && hk[h] != a) h = (h + 1) & (REGN - 1);
    hk[h] = a; hc[h]++;
    if (log_all < 0) log_all = getenv("DC_LOGALL") != NULL;
    if (log_all || log_budget > 0) { log_budget--; fprintf(stderr, "[mmio] %-3s %08X = %08X\n", k, a, v); }
}

void hw_report(void) {
    pvr_report();
    { void aica_report(void); aica_report(); }
    fprintf(stderr, "PVR: TA list inits %llu, start-render %llu; store-queue flushes %llu (%llu to TA)\n",
            (unsigned long long)pvr_ta_init, (unsigned long long)pvr_render_count,
            (unsigned long long)sq_flushes, (unsigned long long)sq_ta_flushes);
    fprintf(stderr, "most-accessed unmodelled registers:\n");
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < REGN; i++) if (hc[i] && (best < 0 || hc[i] > hc[best])) best = i;
        if (best < 0) break;
        fprintf(stderr, "  %08X x%u (last value %08X)\n", hk[best], hc[best], *reg_slot(hk[best]));
        hc[best] = 0;
    }
}

static uint32_t rd(uint32_t a, int size) {
    uint32_t vo;
    if (vram_off(a, &vo)) { uint32_t v = 0; memcpy(&v, vram + vo, size); return v; }
    a = canon(a); int ch, r;
    if (a >= 0xE0000000u && a < 0xE4000000u) return 0;              /* store queues are write-only */
    if (a >= 0x00700000u && a < 0x00A00000u) return aica_read(a, size);
    if ((r = tmu_reg(a, &ch)) >= 0) {
        if (r == 0) return tmu[ch].tcor;
        if (r == 4) return tmu_tcnt(ch);
        return tmu[ch].tcr;
    }
    if (a == 0xFFD80004u) return tstr;
    if (a == 0x005F6900u) { holly_update_events(); return ist_nrm_read(); }
    if (a == 0x005F6904u) return istext;
    if (a == 0x005F6908u) return isterr;
    if (a == 0xFF000028u) return intevt_reg;
    if (a == 0x005F810Cu) return spg_status();
    #ifdef SIGTRAP
    if (a < 0x00200000u && getenv("DC_TRAP_ROM")) raise(SIGTRAP);
#endif   /* debugging aid: BIOS ROM read */
    uint32_t v = *reg_slot(a);
    unk(size == 4 ? "r32" : size == 2 ? "r16" : "r8", a, v);
    return v;
}
static void wr(uint32_t a, uint32_t v, int size) {
    uint32_t vo;
    if (vram_off(a, &vo)) { memcpy(vram + vo, &v, size); vram_touch_lin(vo); return; }
    if (a >= 0xE0000000u && a < 0xE4000000u) { sq_store(a, v, size); return; }
    a = canon(a); int ch, r;
    if (a >= 0x00700000u && a < 0x00A00000u) { aica_write(a, v, size); return; }
    if (a >= 0x10000000u && a < 0x14000000u) { bus_write_block(a, (const uint8_t *)&v, size); return; }
    if ((r = tmu_reg(a, &ch)) >= 0) {
        if (r == 0) tmu[ch].tcor = v;
        else if (r == 4) { tmu[ch].tcnt_base = v; tmu[ch].start_ns = now_ns(); }
        else tmu[ch].tcr = (uint16_t)((v & ~0x100u) | (tmu[ch].tcr & v & 0x100u));
        return;
    }
    if (a == 0xFFD80004u) { tmu_start_stop((uint8_t)v); return; }
    if (a == 0x005F8014u) { pvr_start_render(); return; }                 /* STARTRENDER */
    if (a == 0x005F8144u) { *reg_slot(a) = 0; if (v & 0x80000000u) pvr_ta_list_init(); return; }  /* TA_LIST_INIT */
    if (a == 0x005F8160u) { if (v & 0x80000000u) pvr_ta_list_cont(); return; }                /* TA_LIST_CONT */
    if (a == 0x005F8148u) { *reg_slot(a) = v; pvr_yuv_reset(); return; }   /* TA_YUV_TEX_BASE: restarts the converter */
    if (dma_reg_write(a, v)) return;
    if (a == 0x005F6900u) { istnrm &= ~v; return; }            /* write-1-to-clear */
    if (a == 0x005F6908u) { isterr &= ~v; return; }
    *reg_slot(a) = v;
    unk(size == 4 ? "w32" : size == 2 ? "w16" : "w8", a, v);
}

uint8_t  mmio_read8 (uint32_t a) { return (uint8_t)rd(a, 1); }
uint16_t mmio_read16(uint32_t a) { return (uint16_t)rd(a, 2); }
uint32_t mmio_read32(uint32_t a) { return rd(a, 4); }
void mmio_write8 (uint32_t a, uint8_t v)  { wr(a, v, 1); }
void mmio_write16(uint32_t a, uint16_t v) { wr(a, v, 2); }
void mmio_write32(uint32_t a, uint32_t v) { wr(a, v, 4); }
