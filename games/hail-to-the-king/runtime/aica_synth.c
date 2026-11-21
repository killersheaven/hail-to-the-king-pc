/* AICA sound generation: 64 voices mixed to 44.1 kHz stereo.
 *
 * Written from the public AICA register documentation (dreamcast.wiki,
 * KallistiOS aica headers). Envelope rate times are the published Yamaha
 * SCSP/AICA timing tables (hardware facts).
 *
 * Per voice (registers at ch*0x80, 16-bit values on a 4-byte stride):
 *   +00 KYONEX KYONB SSCTL LPCTL PCMS(2) SA[22:16]   +04 SA[15:0]
 *   +08 LSA  +0C LEA (samples)
 *   +10 D2R(5) D1R(5) - AR(5)   +14 LPSLNK KRS(4) DL(5) RR(5)
 *   +18 OCT(4, signed) - FNS(10)
 *   +24 DISDL(4) DIPAN(5)   +28 TL(8) ...   +20 IMXL(4) ISEL(4) (DSP send)
 * PCMS: 0 = 16-bit PCM, 1 = 8-bit PCM, 2/3 = 4-bit Yamaha ADPCM.
 *
 * Effect DSP (reverb / echo): each voice also sends its signal (level IMXL)
 * to one of 16 DSP inputs (MIXS[ISEL]). Every sample the DSP runs the game's
 * program of up to 128 instructions (MPRO, 0x3400) over those inputs, its
 * temporary registers and a delay ring in sound RAM (RBP / RBL at 0x2804),
 * and leaves up to 16 outputs (EFREG) that are mixed in with their own level
 * and pan (EFSDL / EFPAN at 0x2000 + n*4). Coefficients at 0x3000 (COEF),
 * delay addresses at 0x3200 (MADRS). The ring holds a 16-bit float format
 * (4-bit exponent, sign, 11-bit mantissa) unless an instruction says NOFL.
 * Instruction fields and data path from the public AICA/SCSP DSP descriptions
 * (the same DSP as the Saturn's SCSP). DC_NODSP=1 turns it off.
 *
 * Not modelled: LFOs, the filter envelope, noise. Without a DSP program,
 * voices that only feed the DSP (DISDL = 0) are played dry at their send
 * level so nothing is silent.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

uint8_t *aica_ram(void);
uint16_t aica_reg16(uint32_t off);

enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE, EG_OFF };
typedef struct {
    int state;               /* EG_* */
    double eg;               /* attenuation 0 (loud) .. 1023 (silent) */
    double pos;              /* sample position relative to SA */
    int lp;                  /* loop end reached since last monitor read */
    /* ADPCM */
    int adpcm_pos, adpcm_pred, adpcm_step;
    int loop_pred, loop_step, loop_saved;
    int16_t cur, prev;       /* decoded samples at floor(pos) and floor(pos)-1 for interpolation */
    int32_t cur_idx;
} voice;
static voice V[64];

static const float AR_MS[64] = {
    1e9f, 1e9f, 8100.0f, 6900.0f, 6000.0f, 4800.0f, 4000.0f, 3400.0f, 3000.0f, 2400.0f, 2000.0f, 1700.0f, 1500.0f,
    1200.0f, 1000.0f, 860.0f, 760.0f, 600.0f, 500.0f, 430.0f, 380.0f, 300.0f, 250.0f, 220.0f, 190.0f, 150.0f, 130.0f,
    110.0f, 95.0f, 76.0f, 63.0f, 55.0f, 47.0f, 38.0f, 31.0f, 27.0f, 24.0f, 19.0f, 15.0f, 13.0f, 12.0f, 9.4f, 7.9f,
    6.8f, 6.0f, 4.7f, 3.8f, 3.4f, 3.0f, 2.4f, 2.0f, 1.8f, 1.6f, 1.3f, 1.1f, 0.93f, 0.85f, 0.65f, 0.53f, 0.44f, 0.40f,
    0.35f, 0.0f, 0.0f };
static const float DR_MS[64] = {
    1e9f, 1e9f, 118200.0f, 101300.0f, 88600.0f, 70900.0f, 59100.0f, 50700.0f, 44300.0f, 35500.0f, 29600.0f, 25300.0f,
    22200.0f, 17700.0f, 14800.0f, 12700.0f, 11100.0f, 8900.0f, 7400.0f, 6300.0f, 5500.0f, 4400.0f, 3700.0f, 3200.0f,
    2800.0f, 2200.0f, 1800.0f, 1600.0f, 1400.0f, 1100.0f, 920.0f, 790.0f, 690.0f, 550.0f, 460.0f, 390.0f, 340.0f,
    270.0f, 230.0f, 200.0f, 170.0f, 140.0f, 110.0f, 98.0f, 85.0f, 68.0f, 57.0f, 49.0f, 43.0f, 34.0f, 28.0f, 25.0f,
    22.0f, 18.0f, 14.0f, 12.0f, 11.0f, 8.5f, 7.1f, 6.1f, 5.4f, 4.3f, 3.6f, 3.1f };

static inline uint16_t R(int ch, int off) { return aica_reg16((uint32_t)ch * 0x80 + off); }

static int eff_rate(int ch, int r) {
    if (r == 0) return 0;
    int krs = (R(ch, 0x14) >> 10) & 15, rate = r * 2;
    if (krs != 15) {
        int oct = (R(ch, 0x18) >> 11) & 15; if (oct & 8) oct -= 16;
        rate += 2 * (krs + oct) + ((R(ch, 0x18) >> 9) & 1);
    }
    return rate < 0 ? 0 : rate > 63 ? 63 : rate;
}
/* attenuation change per output sample for a full 0..1023 sweep in the table's time */
static double eg_step(const float *tab, int rate) {
    float ms = tab[rate];
    if (ms <= 0) return 1024.0;
    return 1023.0 / (ms * 44.1);
}

void synth_key(int ch, int on) {
    voice *v = &V[ch];
    if (on) {
        memset(v, 0, sizeof *v);
        v->state = EG_ATTACK; v->eg = 1023;
        v->adpcm_step = 127; v->cur_idx = -1;
    } else if (v->state != EG_OFF) v->state = EG_RELEASE;
}
uint16_t synth_egmon(int ch, int clear_lp) {
    voice *v = &V[ch];
    uint16_t r = (uint16_t)((v->lp ? 0x8000 : 0) | (v->state > 3 ? 3 : v->state) << 13 | ((uint32_t)v->eg & 0x3FF) << 3);
    if (v->state == EG_OFF) r = 0x8000 * v->lp | 3 << 13 | 0x1FFF;
    if (clear_lp) v->lp = 0;
    return r;
}
uint16_t synth_ca(int ch) { return (uint16_t)(uint32_t)V[ch].pos; }

/* Yamaha 4-bit ADPCM */
static const int adpcm_scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
static int adpcm_next(voice *v, uint32_t sa) {
    uint8_t *ram = aica_ram();
    uint8_t b = ram[(sa + (uint32_t)(v->adpcm_pos >> 1)) & 0x1FFFFF];
    int n = (v->adpcm_pos & 1) ? b >> 4 : b & 15;
    v->adpcm_pos++;
    int d = ((n & 7) * 2 + 1) * v->adpcm_step / 8;
    v->adpcm_pred += (n & 8) ? -d : d;
    if (v->adpcm_pred > 32767) v->adpcm_pred = 32767;
    if (v->adpcm_pred < -32768) v->adpcm_pred = -32768;
    v->adpcm_step = v->adpcm_step * adpcm_scale[n & 7] / 256;
    if (v->adpcm_step < 127) v->adpcm_step = 127;
    if (v->adpcm_step > 24576) v->adpcm_step = 24576;
    return v->adpcm_pred;
}

static int16_t fetch(int ch, voice *v, uint32_t idx, int pcms, uint32_t sa) {
    uint8_t *ram = aica_ram();
    if (pcms == 0) { int16_t s; memcpy(&s, ram + ((sa + idx * 2) & 0x1FFFFE), 2); return s; }
    if (pcms == 1) return (int16_t)((int8_t)ram[(sa + idx) & 0x1FFFFF] * 256);
    /* ADPCM: decode forward to idx (sequential stream) */
    (void)ch;
    while ((uint32_t)v->adpcm_pos <= idx) {
        if (!v->loop_saved && (uint32_t)v->adpcm_pos == R(ch, 0x08)) { v->loop_saved = 1; v->loop_pred = v->adpcm_pred; v->loop_step = v->adpcm_step; }
        adpcm_next(v, sa);
    }
    return (int16_t)v->adpcm_pred;
}

/* ---- effect DSP ---- */
static struct {
    int32_t temp[128], mems[32], mixs[16], efreg[16], memval[4];
    int32_t acc, frc_reg, y_reg, adrs_reg;
    uint32_t dec;
    int last_step;                          /* steps after this are empty; -1 = no program */
} dsp;
static inline int32_t sx24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
static inline int32_t sx13(int32_t v) { return (int32_t)((uint32_t)v << 19) >> 19; }
static uint16_t dsp_pack(int32_t val) {      /* 24-bit -> ring's 16-bit float */
    int sign = (val >> 23) & 1;
    uint32_t t = (uint32_t)(val ^ (val << 1)) & 0xFFFFFF;
    int e = 0; while (e < 12 && !(t & 0x800000)) { t <<= 1; e++; }
    val = e < 12 ? (int32_t)(((uint32_t)val << e) & 0x3FFFFF) : (int32_t)((uint32_t)val << 11);
    val = (val >> 11) & 0x7FF;
    return (uint16_t)(val | sign << 15 | e << 11);
}
static int32_t dsp_unpack(uint16_t v) {
    int sign = (v >> 15) & 1, e = (v >> 11) & 15;
    int32_t u = (int32_t)(v & 0x7FF) << 11;
    if (e > 11) { e = 11; u |= sign << 22; } else u |= (sign ^ 1) << 22;
    u |= sign << 23;
    return sx24(u) >> e;
}
static void dsp_find_program(void) {
    static uint32_t seen_hash; uint32_t h = 2166136261u; dsp.last_step = -1;
    for (int st = 0; st < 128; st++) for (int k = 0; k < 4; k++) {
        uint16_t w = aica_reg16(0x3400u + st * 16u + k * 4u);
        if (w) dsp.last_step = st;
        h = (h ^ w) * 16777619u;
    }
    if (h != seen_hash) { seen_hash = h; if (dsp.last_step >= 0) fprintf(stderr, "[dsp] effect program: %d steps\n", dsp.last_step + 1); }
}
static void dsp_run(void) {
    uint16_t *ram = (uint16_t *)aica_ram();                     /* 1M 16-bit words */
    uint16_t r04 = aica_reg16(0x2804);
    uint32_t rbp = (uint32_t)(r04 & 0xFFF) << 10, rbl = (8u * 1024u) << ((r04 >> 13) & 3);
    memset(dsp.efreg, 0, sizeof dsp.efreg);
    int32_t inputs = 0, shifted = 0;
    for (int st = 0; st <= dsp.last_step; st++) {
        uint32_t a = 0x3400u + (uint32_t)st * 16u;
        uint16_t i0 = aica_reg16(a), i1 = aica_reg16(a + 4), i2 = aica_reg16(a + 8), i3 = aica_reg16(a + 12);
        int tra = (i0 >> 9) & 0x7F, twt = (i0 >> 8) & 1, twa = (i0 >> 1) & 0x7F;
        int xsel = (i1 >> 15) & 1, ysel = (i1 >> 13) & 3, ira = (i1 >> 7) & 0x3F, iwt = (i1 >> 6) & 1, iwa = (i1 >> 1) & 0x1F;
        int table = (i2 >> 15) & 1, mwt = (i2 >> 14) & 1, mrd = (i2 >> 13) & 1, ewt = (i2 >> 12) & 1, ewa = (i2 >> 8) & 0xF;
        int adrl = (i2 >> 7) & 1, frcl = (i2 >> 6) & 1, shift = (i2 >> 4) & 3, yrl = (i2 >> 3) & 1, negb = (i2 >> 2) & 1, zero = (i2 >> 1) & 1, bsel = i2 & 1;
        int nofl = (i3 >> 15) & 1, coef = (i3 >> 9) & 0x3F, masa = (i3 >> 2) & 0x1F, adreb = (i3 >> 1) & 1, nxadr = i3 & 1;
        /* input */
        if (ira <= 0x1F) inputs = dsp.mems[ira];
        else if (ira <= 0x2F) inputs = dsp.mixs[ira - 0x20] << 4;
        else inputs = 0;                                         /* EXTS (CD audio): none */
        inputs = sx24(inputs);
        if (iwt) { dsp.mems[iwa] = dsp.memval[st & 3]; if (ira == iwa) inputs = dsp.memval[st & 3]; }
        /* operands */
        int32_t b = 0;
        if (!zero) { b = bsel ? dsp.acc : sx24(dsp.temp[(tra + dsp.dec) & 0x7F]); if (negb) b = -b; }
        int32_t x = xsel ? inputs : sx24(dsp.temp[(tra + dsp.dec) & 0x7F]);
        int32_t y;
        if (ysel == 0) y = dsp.frc_reg;
        else if (ysel == 1) y = (int16_t)aica_reg16(0x3000u + (uint32_t)coef * 4u) >> 3;
        else if (ysel == 2) y = (dsp.y_reg >> 11) & 0x1FFF;
        else y = (dsp.y_reg >> 4) & 0x0FFF;
        if (yrl) dsp.y_reg = inputs;
        /* shifter (on the accumulator from the previous step) */
        if (shift == 0) { shifted = dsp.acc; if (shifted > 0x7FFFFF) shifted = 0x7FFFFF; if (shifted < -0x800000) shifted = -0x800000; }
        else if (shift == 1) { shifted = dsp.acc * 2; if (shifted > 0x7FFFFF) shifted = 0x7FFFFF; if (shifted < -0x800000) shifted = -0x800000; }
        else if (shift == 2) shifted = sx24(dsp.acc * 2);
        else shifted = sx24(dsp.acc);
        /* multiply-accumulate */
        dsp.acc = (int32_t)(((int64_t)x * (int64_t)sx13(y)) >> 12) + b;
        if (twt) dsp.temp[(twa + dsp.dec) & 0x7F] = shifted;
        if (frcl) dsp.frc_reg = shift == 3 ? shifted & 0xFFF : (shifted >> 11) & 0x1FFF;
        /* delay ring (memory is reached on odd steps) */
        if ((mrd || mwt) && (st & 1)) {
            uint32_t addr = aica_reg16(0x3200u + (uint32_t)masa * 4u);
            if (!table) addr += dsp.dec;
            if (adreb) addr += (uint32_t)dsp.adrs_reg & 0xFFF;
            if (nxadr) addr++;
            addr &= table ? 0xFFFFu : rbl - 1;
            addr = (addr + rbp) & 0xFFFFF;
            if (mrd) dsp.memval[(st + 2) & 3] = nofl ? (int32_t)(int16_t)ram[addr] << 8 : dsp_unpack(ram[addr]);
            if (mwt) ram[addr] = nofl ? (uint16_t)(shifted >> 8) : dsp_pack(shifted);
        }
        if (adrl) dsp.adrs_reg = shift == 3 ? (shifted >> 12) & 0xFFF : inputs >> 16;
        if (ewt) dsp.efreg[ewa] += shifted >> 8;
    }
    dsp.dec--;
    memset(dsp.mixs, 0, sizeof dsp.mixs);
}

static inline float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }
static float lin_cache_tl[256], lin_cache_lvl[16];
static void init_tables(void) {
    static int done; if (done) return; done = 1;
    for (int i = 0; i < 256; i++) lin_cache_tl[i] = db_to_lin(-0.375f * i);
    lin_cache_lvl[0] = 0; for (int i = 1; i < 16; i++) lin_cache_lvl[i] = db_to_lin(-3.0f * (15 - i));
}

uint64_t synth_active_samples;
void synth_sample(int32_t *outl, int32_t *outr) {
    init_tables();
    static int nodsp = -1; if (nodsp < 0) nodsp = getenv("DC_NODSP") != NULL;
    static uint32_t tick; if (!(tick++ & 1023)) { if (nodsp) dsp.last_step = -1; else dsp_find_program(); }
    int use_dsp = dsp.last_step >= 0;
    float L = 0, Rr = 0;
    for (int ch = 0; ch < 64; ch++) {
        voice *v = &V[ch];
        if (v->state == EG_OFF) continue;
        uint16_t r0 = R(ch, 0x00);
        if (r0 & 0x0400) continue;                              /* SSCTL noise: not modelled */
        int pcms = (r0 >> 7) & 3, lpctl = (r0 >> 9) & 1;
        uint32_t sa = ((uint32_t)(r0 & 0x7F) << 16 | R(ch, 0x04)) & 0x1FFFFF;
        uint32_t lsa = R(ch, 0x08), lea = R(ch, 0x0C);
        /* envelope */
        uint16_t r10 = R(ch, 0x10), r14 = R(ch, 0x14);
        switch (v->state) {
        case EG_ATTACK: {
            int rt = eff_rate(ch, r10 & 31);
            if (rt) { v->eg -= eg_step(AR_MS, rt) * (1.0 + v->eg / 256.0); if (v->eg <= 0) { v->eg = 0; v->state = EG_DECAY1; } }
            break; }
        case EG_DECAY1: {
            int rt = eff_rate(ch, (r10 >> 6) & 31); double dl = ((r14 >> 5) & 31) * 32.0;
            if (rt) v->eg += eg_step(DR_MS, rt);
            if (v->eg >= dl) v->state = EG_DECAY2;
            break; }
        case EG_DECAY2: { int rt = eff_rate(ch, (r10 >> 11) & 31); if (rt) v->eg += eg_step(DR_MS, rt); break; }
        case EG_RELEASE: { int rt = eff_rate(ch, r14 & 31); v->eg += rt ? eg_step(DR_MS, rt) : 0; break; }
        }
        if (v->eg >= 1023) { v->eg = 1023; if (v->state == EG_RELEASE || v->state == EG_DECAY2) { v->state = EG_OFF; continue; } }
        /* sample */
        uint32_t idx = (uint32_t)v->pos;
        if (pcms >= 2 && idx < (uint32_t)v->cur_idx) { v->adpcm_pos = 0; v->adpcm_pred = 0; v->adpcm_step = 127; }
        if ((int32_t)idx != v->cur_idx) {
            v->prev = (int32_t)idx == v->cur_idx + 1 ? v->cur : fetch(ch, v, idx ? idx - 1 : 0, pcms, sa);
            v->cur = fetch(ch, v, idx, pcms, sa);
            v->cur_idx = (int32_t)idx;
        }
        float fr = (float)(v->pos - idx);
        float s = (float)v->prev + ((float)v->cur - (float)v->prev) * fr;   /* linear interpolation */
        /* level */
        uint16_t r24 = R(ch, 0x24), r28 = R(ch, 0x28), r20 = R(ch, 0x20);
        int disdl = (r24 >> 8) & 15, dipan = r24 & 31, imxl = (r20 >> 4) & 15;
        float env = lin_cache_tl[r28 >> 8] * db_to_lin(-0.09375f * (float)v->eg);
        if (use_dsp) { if (imxl) dsp.mixs[r20 & 15] += (int32_t)(s * env * lin_cache_lvl[imxl]) << 4; }   /* effect send, 20-bit */
        else if (!disdl) disdl = imxl;                        /* no DSP program: play DSP-only voices dry */
        float g = lin_cache_lvl[disdl] * env;
        float pl = 1, pr = 1;
        if (dipan & 15) { float a = lin_cache_lvl[15 - (dipan & 15)]; if (dipan & 16) pl = a; else pr = a; }
        L += s * g * pl; Rr += s * g * pr;
        synth_active_samples++;
        /* advance pitch */
        uint16_t r18 = R(ch, 0x18);
        int oct = (r18 >> 11) & 15; if (oct & 8) oct -= 16;
        v->pos += ldexp(1.0 + (r18 & 0x3FF) / 1024.0, oct);
        if (v->pos >= lea && lea) {
            v->lp = 1;
            if (lpctl) {
                v->pos = lsa + (v->pos - lea);
                if (pcms >= 2 && v->loop_saved) { v->adpcm_pos = (int)lsa; v->adpcm_pred = v->loop_pred; v->adpcm_step = v->loop_step; v->cur_idx = -1; }
            } else { v->state = EG_OFF; v->pos = lea; }
        }
    }
    if (use_dsp) {                                            /* effect outputs, each with its own level and pan */
        { static int dbg = -1; if (dbg < 0) dbg = getenv("DC_DSPDEBUG") != NULL;
          static uint32_t n; static int32_t mx_in, mx_out;
          for (int i = 0; i < 16; i++) { int32_t m = dsp.mixs[i] < 0 ? -dsp.mixs[i] : dsp.mixs[i]; if (m > mx_in) mx_in = m; }
          static int announced; if (mx_in && !announced) { announced = 1; extern uint64_t dc_frame_count(void);
              fprintf(stderr, "[dsp] the game is sending sound to the effect DSP (reverb) from frame %llu\n", (unsigned long long)dc_frame_count()); }
          if (dbg && !(++n % 44100)) {
              fprintf(stderr, "[dsp] max send %d, max out %d, RBP/RBL %04X, EFSDL", mx_in, mx_out, aica_reg16(0x2804));
              for (int i = 0; i < 18; i++) fprintf(stderr, " %04X", aica_reg16(0x2000u + (uint32_t)i * 4u));
              int sends = 0; for (int ch = 0; ch < 64; ch++) if (V[ch].state != EG_OFF && (R(ch, 0x20) >> 4) & 15) sends++;
              fprintf(stderr, " | voices sending %d\n", sends); mx_in = mx_out = 0; }
          dsp_run();
          for (int i = 0; i < 16; i++) { int32_t m = dsp.efreg[i] < 0 ? -dsp.efreg[i] : dsp.efreg[i]; if (m > mx_out) mx_out = m; } }
        for (int i = 0; i < 16; i++) {
            if (!dsp.efreg[i]) continue;
            uint16_t e = aica_reg16(0x2000u + (uint32_t)i * 4u);
            int lvl = (e >> 8) & 15, pan = e & 31; if (!lvl) continue;
            float o = (float)dsp.efreg[i] * lin_cache_lvl[lvl], pl = 1, pr = 1;
            if (pan & 15) { float a = lin_cache_lvl[15 - (pan & 15)]; if (pan & 16) pl = a; else pr = a; }
            L += o * pl; Rr += o * pr;
        }
    }
    float mv = lin_cache_lvl[aica_reg16(0x2800) & 15];
    *outl = (int32_t)(L * mv); *outr = (int32_t)(Rr * mv);
}
