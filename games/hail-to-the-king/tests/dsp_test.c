/* Self-test for the AICA effect DSP (aica_synth.c): float format round trip and a
 * 100-sample echo through the delay ring. cc -I ../runtime dsp_test.c -lm && ./a.out */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint8_t ram[2 << 20], regs[0x8000];
uint8_t *aica_ram(void) { return ram; }
uint16_t aica_reg16(uint32_t o) { uint16_t v; memcpy(&v, regs + (o & 0x7FFC), 2); return v; }
static void w16(uint32_t o, uint16_t v) { memcpy(regs + o, &v, 2); }
uint64_t dc_frame_count(void) { return 0; }
#include "aica_synth.c"
int main(void) {
    int bad = 0;
    for (int32_t v = -0x800000; v < 0x800000; v += 997) { int32_t u = dsp_unpack(dsp_pack(v)); int32_t err = u - v; if (err < 0) err = -err;
        int32_t tol = (v < 0 ? -v : v) >> 10; if (tol < 16) tol = 16; if (err > tol) { if (bad++ < 3) printf("pack %d -> %d\n", v, u); } }
    printf("pack/unpack: %d bad\n", bad);
    /* echo: step 1 (odd) reads ring at MADRS[0]=100 into MEMVAL; step 3 writes MIXS0 to ring at MADRS[1]=0;
       step 5 moves MEMS->EFREG. Simplest check: write input to ring now, read it back 100 samples later. */
    w16(0x2804, 0x0000);                 /* RBP 0, RBL 8K words */
    w16(0x3200, 100); w16(0x3204, 0);    /* MADRS0 = 100 (read, 100 samples old), MADRS1 = 0 (write) */
    w16(0x3000 + 0 * 4, 0x7FF8);         /* COEF0 ~ +1.0 */
    /* step 0: X=INPUTS(MIXS0) * COEF0 -> ACC */
    w16(0x3400 + 0 * 16 + 4, (1 << 15) | (1 << 13) | (0x20 << 7));      /* XSEL=1 YSEL=1 IRA=0x20 */
    w16(0x3400 + 0 * 16 + 8, (1 << 1));                                    /* ZERO */
    w16(0x3400 + 0 * 16 + 12, (0 << 9));                                  /* COEF 0 */
    /* step 1 (odd): write SHIFTED(=ACC) to ring at MADRS1, NOFL */
    w16(0x3400 + 1 * 16 + 8, (1 << 14) | (1 << 1));                       /* MWT, ZERO */
    w16(0x3400 + 1 * 16 + 12, (1 << 15) | (1 << 2));                      /* NOFL, MASA=1 */
    /* step 3 (odd): read ring at MADRS0 -> MEMVAL[(3+2)&3=1] ; step 5: IWT MEMS0 <- MEMVAL[5&3=1] */
    w16(0x3400 + 3 * 16 + 8, (1 << 13) | (1 << 1));                       /* MRD, ZERO */
    w16(0x3400 + 3 * 16 + 12, (1 << 15) | (0 << 2));                      /* NOFL, MASA=0 */
    w16(0x3400 + 5 * 16 + 4, (1 << 6) | (0 << 1) | (0 << 7));             /* IWT, IWA=0, IRA=0 (MEMS0) */
    w16(0x3400 + 5 * 16 + 8, (1 << 1));
    /* step 6: X = INPUTS(MEMS0) * COEF0 -> ACC ; step 7: EWT EFREG0 <- SHIFTED */
    w16(0x3400 + 6 * 16 + 4, (1 << 15) | (1 << 13) | (0 << 7));
    w16(0x3400 + 6 * 16 + 8, (1 << 1));
    w16(0x3400 + 7 * 16 + 8, (1 << 12) | (0 << 8) | (1 << 1));
    dsp_find_program();
    int hit = -1;
    for (int n = 0; n < 300; n++) {
        dsp.mixs[0] = n == 10 ? (10000 << 4) : 0;      /* an impulse at sample 10 */
        dsp_run();
        if (dsp.efreg[0] && hit < 0) { hit = n; printf("echo at sample %d, level %d (impulse 10000)\n", n, dsp.efreg[0]); }
    }
    return 0;
}
