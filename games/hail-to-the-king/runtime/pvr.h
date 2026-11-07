/* Host-side PowerVR2 display list, filled by the TA parser (pvr.c). */
#pragma once
#include <stdint.h>

typedef struct { float x, y, z, u, v; uint32_t col, ofs; } pvr_vert;   /* screen x/y, z = 1/w */
typedef struct { uint32_t pcw, isp, tsp, tcw; uint8_t list, sprite, modvol, stride; } pvr_param;   /* stride: TEXT_CONTROL at submission */
typedef struct { uint32_t param, first, count; uint8_t list; } pvr_strip;

enum { PVR_OPAQUE = 0, PVR_OPAQUE_MOD = 1, PVR_TRANS = 2, PVR_TRANS_MOD = 3, PVR_PUNCH = 4 };

typedef struct pvr_frame {
    int used;
    uint32_t param_base;
    uint32_t list_done;
    uint32_t seq;                     /* list number (order of TA_LIST_INIT) */
    pvr_vert *verts;  uint32_t nverts, cap_verts;
    pvr_strip *strips; uint32_t nstrips, cap_strips;
    pvr_param *params; uint32_t nparams, cap_params;
    uint32_t list_first[5], list_count[5];
} pvr_frame;

typedef void (*pvr_render_fn)(const pvr_frame *f);
void pvr_set_renderer(pvr_render_fn fn);
void pvr_ta_write(const uint8_t *src, uint32_t len);
void pvr_ta_list_init(void);
void pvr_ta_list_cont(void);
void pvr_start_render(void);
void pvr_report(void);
void pvr_yuv_write(const uint8_t *src, uint32_t len);
void pvr_yuv_reset(void);

extern uint8_t vram[8 << 20];   /* linear (32-bit path) layout */

/* texture-cache invalidation: one generation counter per 4 KB page of the 64-bit (texture) address space */
extern uint32_t vram_gen[2048];
static inline void vram_touch_lin(uint32_t off) {
    uint32_t a64 = ((off & 0x3FFFFCu) << 1) | (((off >> 22) & 1u) << 2);
    vram_gen[(a64 >> 12) & 2047]++;
}
static inline uint32_t vram64_off(uint32_t a) {   /* 64-bit-path address -> offset in vram[] */
    a &= 0x7FFFFFu;
    return ((a & 0x7FFFF8u) >> 1) | (a & 3u) | (((a >> 2) & 1u) << 22);
}
