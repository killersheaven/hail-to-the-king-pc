/* AICA sound chip: sound RAM, registers, interrupt controller, timers, ARM7.
 *
 * Written from public documentation (dreamcast.wiki AICA pages, KallistiOS
 * aica headers); no emulator source was used.
 *
 * Address spaces:
 *   SH-4: registers 0x00700000-0x00707FFF, RTC 0x00710000, sound RAM 0x00800000-0x009FFFFF
 *   ARM7: sound RAM 0x000000-0x7FFFFF (2 MB mirrored), registers 0x800000-0x807FFF
 * Registers are 16-bit values on a 4-byte stride.
 *
 * Interrupts:
 *   to the ARM7: FIQ while (SCIPD & SCIEB) != 0; L register (0x2D00) gives the level from SCILV0-2
 *   to the SH-4: Holly external interrupt bit 1 while (MCIPD & MCIEB) != 0
 *   bit 5 of SCIPD / MCIPD is the CPU-to-CPU "doorbell"; bits 6/7/8 are timers A/B/C; bit 10 is one sample
 *
 * Not yet: channel synthesis (sample playback), DSP, sound output.
 */
#include "arm7.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

void hw_set_ext(int bit, int on);

static uint8_t aram[2 << 20];
static uint8_t aregs[0x8000];
uint8_t *aica_ram(void) { return aram; }

static int aica_trace = -1;
static void tr(const char *k, uint32_t a, uint32_t v) {
    if (aica_trace < 0) aica_trace = getenv("DC_AICATRACE") != NULL;
    if (aica_trace) fprintf(stderr, "[aica] %s %04X = %08X\n", k, a, v);
}

static inline uint16_t reg16(uint32_t off) { uint16_t v; memcpy(&v, aregs + (off & 0x7FFC), 2); return v; }
static inline void set16(uint32_t off, uint16_t v) { memcpy(aregs + (off & 0x7FFC), &v, 2); }

#define R_MSLC   0x280C
#define R_EGMON  0x2810
#define R_CAMON  0x2814
#define R_TIMA   0x2890
#define R_SCIEB  0x289C
#define R_SCIPD  0x28A0
#define R_SCIRE  0x28A4
#define R_SCILV0 0x28A8
#define R_SCILV1 0x28AC
#define R_SCILV2 0x28B0
#define R_MCIEB  0x28B4
#define R_MCIPD  0x28B8
#define R_MCIRE  0x28BC
#define R_ARMRST 0x2C00
#define R_INTL   0x2D00
#define R_INTM   0x2D04

void synth_key(int ch, int on);
uint16_t synth_egmon(int ch, int clear_lp);
uint16_t synth_ca(int ch);
void synth_sample(int32_t *l, int32_t *r);
uint16_t aica_reg16(uint32_t off) { uint16_t v; memcpy(&v, aregs + (off & 0x7FFC), 2); return v; }

/* output ring buffer, 44.1 kHz stereo int16; the platform layer drains it */
#define RB_FRAMES 32768
static int16_t rb[RB_FRAMES * 2]; static volatile uint32_t rb_w, rb_r;
static FILE *wav; static uint32_t wav_frames;
static void wav_close(void) {
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav); fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav); fclose(wav); wav = NULL;
}
static void out_frame(int32_t l, int32_t r) {
    if (l > 32767) l = 32767; if (l < -32768) l = -32768; if (r > 32767) r = 32767; if (r < -32768) r = -32768;
    int16_t fr[2] = { (int16_t)l, (int16_t)r };
    if (rb_w - rb_r < RB_FRAMES) { memcpy(&rb[(rb_w % RB_FRAMES) * 2], fr, 4); rb_w++; }
    if (wav) { fwrite(fr, 2, 2, wav); wav_frames++; }
}
int aica_audio_read(int16_t *dst, int frames) {
    int n = 0;
    while (n < frames && rb_r != rb_w) { memcpy(dst + 2 * n, &rb[(rb_r % RB_FRAMES) * 2], 4); rb_r++; n++; }
    return n;
}
int aica_audio_available(void) { return (int)(rb_w - rb_r); }
void aica_audio_skip(int frames) { while (frames-- > 0 && rb_r != rb_w) rb_r++; }
static void wav_open(void) {
    static int tried; if (tried) return; tried = 1;
    const char *p = getenv("DC_WAV"); if (!p) return;
    wav = fopen(p, "wb"); if (!wav) return;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                      0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    fwrite(h, 1, 44, wav); atexit(wav_close);
}

static uint16_t scipd, mcipd;
static int arm_running;
static uint8_t key_on[64];
static uint32_t timer_frac[3];

static void update_irqs(void) {
    arm.fiq_line = (scipd & reg16(R_SCIEB)) != 0;
    hw_set_ext(1, (mcipd & reg16(R_MCIEB)) != 0);
}
static uint32_t int_level(void) {
    uint32_t p = scipd & reg16(R_SCIEB);
    if (!p) return 0;
    int i = 0; while (!(p >> i & 1)) i++;
    if (i > 7) i = 7;
    return (reg16(R_SCILV0) >> i & 1) | (reg16(R_SCILV1) >> i & 1) << 1 | (reg16(R_SCILV2) >> i & 1) << 2;
}

static uint32_t reg_read(uint32_t off) {
    off &= 0x7FFC;
    switch (off) {
    case R_SCIPD: return scipd;
    case R_MCIPD: return mcipd;
    case R_INTL:  return int_level();
    case R_EGMON: return synth_egmon((reg16(R_MSLC) >> 8) & 63, 1);   /* LP flag clears on read */
    case R_CAMON: return synth_ca((reg16(R_MSLC) >> 8) & 63);
    }
    if (off < 0x2000 && (off & 0x7F) == 0) return reg16(off) & 0x7FFF;   /* KYONEX reads 0 */
    return reg16(off);
}

static void reg_write(uint32_t off, uint32_t v, int size, uint32_t byte_addr) {
    off &= 0x7FFC;
    if (size == 1 && (byte_addr & 2)) return;   /* upper half of the 32-bit slot: unused */
    uint16_t cur = reg16(off), nv;
    if (size == 1) nv = (byte_addr & 1) ? (uint16_t)((cur & 0x00FF) | (v & 0xFF) << 8) : (uint16_t)((cur & 0xFF00) | (v & 0xFF));
    else nv = (uint16_t)v;
    switch (off) {
    case R_SCIPD: if (nv & 0x20) scipd |= 0x20; update_irqs(); return;
    case R_SCIRE: scipd &= ~nv; update_irqs(); return;
    case R_MCIPD: if (nv & 0x20) mcipd |= 0x20; update_irqs(); return;
    case R_MCIRE: mcipd &= ~nv; update_irqs(); return;
    case R_SCIEB: case R_MCIEB: set16(off, nv); update_irqs(); return;
    case R_INTM: return;                      /* interrupt clear: the level is recomputed from pending bits */
    case R_TIMA: case R_TIMA + 4: case R_TIMA + 8: timer_frac[(off - R_TIMA) / 4] = 0; set16(off, nv); return;
    case R_ARMRST: {
        int was = arm_running;
        set16(off, nv);
        arm_running = !(nv & 1);
        if (arm_running && !was) { arm7_reset(); update_irqs(); fprintf(stderr, "[aica] ARM7 released from reset\n"); }
        return;
    }
    }
    set16(off, nv);
    if (off < 0x2000 && (off & 0x7F) == 0 && (nv & 0x8000)) {   /* KYONEX: apply KYONB of every channel */
        for (int ch = 0; ch < 64; ch++) {
            int on = (reg16(ch * 0x80) >> 14) & 1;
            if (on != key_on[ch]) synth_key(ch, on);
            key_on[ch] = (uint8_t)on;
        }
        set16(off, nv & 0x7FFF);
    }
}

/* ---------------- SH-4 side ---------------- */
uint32_t aica_read(uint32_t a, int size) {
    uint32_t v = 0;
    if (a >= 0x00800000u) { memcpy(&v, aram + (a & 0x1FFFFFu), size); return v; }
    if (a >= 0x00710000u && a < 0x00710010u) {           /* RTC: seconds since 1950-01-01 */
        extern uint64_t hw_vclock_ns, hw_vclock_step;
        uint32_t t = hw_vclock_step ? 1577836800u + 631152000u + (uint32_t)(hw_vclock_ns / 1000000000ull)   /* deterministic: 2020-01-01 + game time */
                                    : (uint32_t)time(NULL) + 631152000u;
        return (a & 0xF) == 0 ? t >> 16 : (a & 0xF) == 4 ? t & 0xFFFF : 0;
    }
    if (a < 0x00708000u) {
        v = reg_read(a);
        if (size == 1 && (a & 1)) v >>= 8;
        tr("sh-rd", a & 0x7FFF, v);
    }
    return v;
}
void aica_write(uint32_t a, uint32_t v, int size) {
    if (a >= 0x00800000u) { memcpy(aram + (a & 0x1FFFFFu), &v, size); return; }
    if (a < 0x00708000u) { tr("sh-wr", a & 0x7FFF, v); reg_write(a, v, size, a); }
}

/* ---------------- ARM7 side ---------------- */
uint32_t arm7_read32(uint32_t a) {
    if (a < 0x800000u) { uint32_t v; memcpy(&v, aram + (a & 0x1FFFFC), 4); return v; }
    if (a < 0x808000u) return reg_read(a);
    return 0;
}
uint16_t arm7_read16(uint32_t a) {
    if (a < 0x800000u) { uint16_t v; memcpy(&v, aram + (a & 0x1FFFFE), 2); return v; }
    if (a < 0x808000u) return (uint16_t)reg_read(a);
    return 0;
}
uint8_t arm7_read8(uint32_t a) {
    if (a < 0x800000u) return aram[a & 0x1FFFFF];
    if (a < 0x808000u) { uint32_t v = reg_read(a); return (uint8_t)((a & 1) ? v >> 8 : v); }
    return 0;
}
void arm7_write32(uint32_t a, uint32_t v) {
    if (a < 0x800000u) { memcpy(aram + (a & 0x1FFFFC), &v, 4); return; }
    if (a < 0x808000u) { tr("arm-wr", a & 0x7FFF, v); reg_write(a, v, 4, a); }
}
void arm7_write16(uint32_t a, uint16_t v) {
    if (a < 0x800000u) { memcpy(aram + (a & 0x1FFFFE), &v, 2); return; }
    if (a < 0x808000u) { tr("arm-wr", a & 0x7FFF, v); reg_write(a, v, 2, a); }
}
void arm7_write8(uint32_t a, uint8_t v) {
    if (a < 0x800000u) { aram[a & 0x1FFFFF] = v; return; }
    if (a < 0x808000u) { tr("arm-wr", a & 0x7FFF, v); reg_write(a, v, 1, a); }
}

/* ---------------- time ---------------- */
#define SAMPLE_RATE 44100
#define ARM_PER_SAMPLE 256           /* ~11 M instructions/s: the ARM7 runs at 22.5 MHz with wait states */
static uint64_t last_ns, frac_ns;
uint64_t aica_samples;

static void tick_sample(void) {
    aica_samples++;
    for (int t = 0; t < 3; t++) {
        uint16_t r = reg16(R_TIMA + 4 * t);
        uint32_t pres = 1u << ((r >> 8) & 7);
        if (++timer_frac[t] >= pres) {
            timer_frac[t] = 0;
            uint16_t cnt = (uint16_t)((r & 0xFF) + 1);
            if (cnt > 0xFF) { cnt = 0; scipd |= 0x40 << t; mcipd |= 0x40 << t; }
            set16(R_TIMA + 4 * t, (uint16_t)((r & 0x0700) | (cnt & 0xFF)));
        }
    }
    scipd |= 0x400;                  /* one-sample interval */
    update_irqs();
    int32_t l, r; synth_sample(&l, &r); out_frame(l, r);
}

/* called from the CPU poll loop with the host clock; runs the sound side up to "now" */
void aica_step(uint64_t now_ns) {
    wav_open();
    if (!last_ns) { last_ns = now_ns; return; }
    uint64_t d = now_ns - last_ns + frac_ns;
    last_ns = now_ns;
    uint64_t n = d * SAMPLE_RATE / 1000000000ull;
    frac_ns = d - n * 1000000000ull / SAMPLE_RATE;
    if (n > SAMPLE_RATE / 20) n = SAMPLE_RATE / 20;   /* never catch up more than 50 ms at once */
    for (uint64_t i = 0; i < n; i++) {
        tick_sample();
        if (arm_running) arm7_run(ARM_PER_SAMPLE);
    }
}

void aica_report(void) {
    fprintf(stderr, "AICA: ARM7 %s, %llu instructions, pc %06X, %llu samples; SCIEB %04X SCIPD %04X MCIEB %04X MCIPD %04X\n",
            arm_running ? "running" : "in reset", (unsigned long long)arm7_insns, arm.pc,
            (unsigned long long)aica_samples, reg16(R_SCIEB), scipd, reg16(R_MCIEB), mcipd);
}
