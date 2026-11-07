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
 * Not modelled: the effect DSP (reverb), LFOs, the filter envelope, noise.
 * Voices that only feed the DSP (DISDL = 0) are played dry at their DSP send
 * level so nothing is silent.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

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
        int disdl = (r24 >> 8) & 15, dipan = r24 & 31;
        if (!disdl) disdl = (r20 >> 4) & 15;                  /* DSP-only voices: play dry at the send level */
        float g = lin_cache_lvl[disdl] * lin_cache_tl[r28 >> 8] * db_to_lin(-0.09375f * (float)v->eg);
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
    float mv = lin_cache_lvl[aica_reg16(0x2800) & 15];
    *outl = (int32_t)(L * mv); *outr = (int32_t)(Rr * mv);
}
