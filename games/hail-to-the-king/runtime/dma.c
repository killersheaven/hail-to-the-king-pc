/* Store queues and the DMA engines the game uses.
 *
 *  - SH-4 store queues (SQ0/SQ1 at 0xE0000000, flushed by PREF)
 *  - channel-2 DMA (SH-4 DMAC ch2 + Holly SB_C2D*): RAM -> TA FIFO / texture memory
 *  - Maple DMA (SB_MD*): controller command lists, answered by maple.c
 *  - G2 DMA (SB_AD*, SB_E1*, SB_E2*, SB_DD*): RAM <-> sound RAM
 *  - PVR DMA (SB_PD*): RAM <-> video RAM
 *
 * Transfers complete instantly and raise their Holly end-of-transfer interrupt.
 * Written from public documentation (dreamcast.wiki, KallistiOS headers).
 */
#include "sh4ctx.h"
#include "pvr.h"
#include <stdio.h>
#include <string.h>

void hw_raise_nrm(int bit);
uint32_t hw_reg(uint32_t a);
void hw_set_reg(uint32_t a, uint32_t v);
int vram_off(uint32_t a, uint32_t *off);
void aica_write(uint32_t a, uint32_t v, int size);
uint32_t aica_read(uint32_t a, int size);
void maple_dma(uint32_t list);

/* ---------------- bus writes to areas other than main RAM ---------------- */
static void vram_write_block(uint32_t a, const uint8_t *src, uint32_t len) {
    /* the 64-bit path interleaves 32-bit words between the two banks: map word by word */
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t off;
        if (!vram_off(a + i, &off)) return;
        uint32_t n = len - i < 4 ? len - i : 4;
        memcpy(vram + off, src + i, n);
        vram_touch_lin(off);
    }
}
uint64_t dma_ta_bytes, dma_tex_bytes;
void bus_write_block(uint32_t a, const uint8_t *src, uint32_t len) {
    uint32_t p = a & 0x1FFFFFFFu;
    if ((p & 0x1C000000u) == 0x0C000000u) { memcpy(dc_ram + (p & 0x00FFFFFFu), src, len); return; }
    switch (p >> 24) {
    case 0x10: case 0x12:
        if ((p & 0x00800000u) == 0) { dma_ta_bytes += len; pvr_ta_write(src, len); }   /* polygon FIFO */
        else pvr_yuv_write(src, len);                                                 /* YUV converter */
        return;
    case 0x11: case 0x13: {                                                           /* texture memory, direct */
        uint32_t lm = hw_reg((p >> 24) == 0x11 ? 0x005F6884u : 0x005F6888u) & 1;
        dma_tex_bytes += len;
        vram_write_block((lm ? 0x05000000u : 0x04000000u) | (p & 0x00FFFFFFu), src, len);
        return;
    }
    case 0x04: case 0x05: case 0x06: case 0x07:
        vram_write_block(p, src, len); return;
    case 0x00:
        if (p >= 0x00700000u && p < 0x00A00000u) {
            for (uint32_t i = 0; i + 4 <= len; i += 4) { uint32_t v; memcpy(&v, src + i, 4); aica_write(p + i, v, 4); }
            return;
        }
        break;
    }
    static int warned;
    if (warned++ < 8) fprintf(stderr, "[bus] block write to unmapped %08X (%u bytes)\n", a, len);
}
static void bus_read_block(uint32_t a, uint8_t *dst, uint32_t len) {
    uint32_t p = a & 0x1FFFFFFFu;
    if ((p & 0x1C000000u) == 0x0C000000u) { memcpy(dst, dc_ram + (p & 0x00FFFFFFu), len); return; }
    for (uint32_t i = 0; i + 4 <= len; i += 4) { uint32_t v = R32(a + i); memcpy(dst + i, &v, 4); }
}

/* ---------------- store queues ---------------- */
static uint32_t sq[2][8];
void sq_store(uint32_t a, uint32_t v, int size) {
    uint8_t *b = (uint8_t *)sq[(a >> 5) & 1] + (a & 0x1F);
    memcpy(b, &v, size);
}
uint64_t sq_flushes, sq_ta_flushes;
void hw_sq_flush(uint32_t a) {
    sq_flushes++;
    int n = (a >> 5) & 1;
    uint32_t qacr = hw_reg(n ? 0xFF00003Cu : 0xFF000038u);
    uint32_t ext = ((qacr >> 2) & 7) << 26 | (a & 0x03FFFFE0u);
    if ((ext >> 24) == 0x10 || (ext >> 24) == 0x12) sq_ta_flushes++;
    bus_write_block(ext, (const uint8_t *)sq[n], 32);
}

/* ---------------- DMA engines ---------------- */
static uint8_t xfer[1 << 16];
static void copy(uint32_t dst, uint32_t src, uint32_t len) {
    while (len) {
        uint32_t n = len > sizeof xfer ? sizeof xfer : len;
        bus_read_block(src, xfer, n);
        bus_write_block(dst, xfer, n);
        src += n; dst += n; len -= n;
    }
}

static void ch2_dma(void) {
    uint32_t src = hw_reg(0xFFA00020u), dst = hw_reg(0x005F6800u), len = hw_reg(0x005F6804u);
    copy(dst, src, len);
    if ((dst >> 24 & 0x1F) == 0x11 || (dst >> 24 & 0x1F) == 0x13) hw_set_reg(0x005F6800u, dst + len);
    hw_set_reg(0xFFA00020u, src + len);
    hw_set_reg(0xFFA00028u, 0);                                   /* DMATCR2 */
    hw_set_reg(0xFFA0002Cu, hw_reg(0xFFA0002Cu) | 2);             /* CHCR2.TE */
    hw_set_reg(0x005F6804u, 0);
    hw_set_reg(0x005F6808u, 0);
    hw_raise_nrm(19);
}

static void g2_dma(int ch) {
    uint32_t base = 0x005F7800u + 0x20u * ch;
    uint32_t g2 = hw_reg(base), sh = hw_reg(base + 4), lenr = hw_reg(base + 8), dir = hw_reg(base + 0xC) & 1;
    uint32_t len = lenr & 0x7FFFFFFFu;
    if (dir == 0) copy(g2, sh, len); else copy(sh, g2, len);
    hw_set_reg(base + 0x18, 0);
    if (lenr & 0x80000000u) hw_set_reg(base + 0x14, 0);          /* DMA end: disable */
    hw_raise_nrm(15 + ch);
}

static void pvr_dma(void) {
    uint32_t pv = hw_reg(0x005F7C00u), sh = hw_reg(0x005F7C04u), len = hw_reg(0x005F7C08u), dir = hw_reg(0x005F7C0Cu) & 1;
    if (dir == 0) copy(pv, sh, len); else copy(sh, pv, len);
    hw_set_reg(0x005F7C18u, 0);
    hw_raise_nrm(11);
}

uint64_t dma_count[8];
int dma_reg_write(uint32_t a, uint32_t v) {
    switch (a) {
    case 0x005F6808u:
        hw_set_reg(a, v);
        if (v & 1) { dma_count[0]++; ch2_dma(); }
        return 1;
    case 0x005F6C18u:
        hw_set_reg(a, v);
        if ((v & 1) && (hw_reg(0x005F6C14u) & 1)) {
            dma_count[1]++;
            maple_dma(hw_reg(0x005F6C04u));
            hw_set_reg(a, 0);
            hw_raise_nrm(12);
        }
        return 1;
    case 0x005F7818u: case 0x005F7838u: case 0x005F7858u: case 0x005F7878u:
        hw_set_reg(a, v);
        if (v & 1) { int ch = (a >> 5) & 3; dma_count[2 + ch]++; g2_dma(ch); }
        return 1;
    case 0x005F7C18u:
        hw_set_reg(a, v);
        if (v & 1) { dma_count[6]++; pvr_dma(); }
        return 1;
    }
    return 0;
}
void dma_report(void) {
    fprintf(stderr, "DMA: ch2 %llu (TA %llu bytes, tex %llu bytes), maple %llu, G2 %llu/%llu/%llu/%llu, PVR %llu\n",
            (unsigned long long)dma_count[0], (unsigned long long)dma_ta_bytes, (unsigned long long)dma_tex_bytes,
            (unsigned long long)dma_count[1], (unsigned long long)dma_count[2], (unsigned long long)dma_count[3],
            (unsigned long long)dma_count[4], (unsigned long long)dma_count[5], (unsigned long long)dma_count[6]);
}
