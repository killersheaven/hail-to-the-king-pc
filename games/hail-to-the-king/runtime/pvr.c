/* PowerVR2 (Holly) front end: Tile Accelerator input and render start.
 *
 * Written from public hardware documentation (dreamcast.wiki, KallistiOS
 * headers). No emulator source was used.
 *
 * The TA receives 32-byte units (from store queues or channel-2 DMA). We parse
 * them into a host-side display list per parameter buffer instead of building
 * the real object lists in VRAM; STARTRENDER then hands the matching list to
 * the renderer. List-end and render-done interrupts are raised as the real
 * chip would, which is what the game's graphics library (Kamui2) waits on.
 */
#include "sh4ctx.h"
#include "pvr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hw_raise_nrm(int bit);
void hw_set_reg(uint32_t a, uint32_t v);
uint32_t hw_reg(uint32_t a);
extern uint64_t pvr_render_count, pvr_ta_init;

/* ---------------- display lists ---------------- */
#define NFRAMES 4
static pvr_frame frames[NFRAMES];
static pvr_frame *cur;            /* list being filled by the TA */
static int next_slot;

static pvr_frame *frame_for(uint32_t base, int create) {
    base &= 0xF00000u;   /* the render selects a 1 MB parameter block */
    for (int i = 0; i < NFRAMES; i++) if (frames[i].used && frames[i].param_base == base) return &frames[i];
    if (!create) return NULL;
    pvr_frame *f = &frames[next_slot]; next_slot = (next_slot + 1) % NFRAMES;
    f->used = 1; f->param_base = base;
    return f;
}
static void frame_reset(pvr_frame *f) {
    f->nverts = 0; f->nstrips = 0; f->nparams = 0;
    for (int l = 0; l < 5; l++) f->list_first[l] = f->list_count[l] = 0;
}
static void *grow(void *p, uint32_t *cap, uint32_t need, size_t sz) {
    if (need <= *cap) return p;
    uint32_t n = *cap ? *cap : 1024; while (n < need) n *= 2;
    p = realloc(p, (size_t)n * sz); *cap = n; return p;
}

/* ---------------- TA parser ---------------- */
static int cur_list = -1;          /* list type of the current list, -1 = none open */
static int vtx_type;               /* vertex parameter type selected by the last global parameter */
static int vtx_words;              /* 8 or 16 */
static uint32_t pend[16]; static int npend, need_words;
static int strip_open;
static float face_col[4], face_ofs[4];  /* intensity-mode face colours (ARGB, 0..1) */
static uint32_t sprite_base, sprite_ofs;
static uint32_t ta_units, ta_bad;

static const int list_irq[5] = {7, 8, 9, 10, 21};

static uint32_t pack_f(float a, float r, float g, float b) {
    #define C8(x) ((uint32_t)((x) <= 0 ? 0 : (x) >= 1 ? 255 : (int)((x) * 255.0f + 0.5f)))
    return C8(a) << 24 | C8(r) << 16 | C8(g) << 8 | C8(b);
}
static uint32_t intensity(const float *fc, float i) {
    return pack_f(fc[0], fc[1] * i, fc[2] * i, fc[3] * i);
}
static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float uv16(uint32_t h) { return f_of(h << 16); }

static int is_modvol_list(int l) { return l == 1 || l == 3; }

static void close_strip(void) {
    if (!strip_open || !cur) return;
    strip_open = 0;
    pvr_strip *s = &cur->strips[cur->nstrips - 1];
    if (s->count < 3) cur->nstrips--;            /* degenerate */
    else cur->list_count[s->list]++;
}
static void open_strip(void) {
    cur->strips = grow(cur->strips, &cur->cap_strips, cur->nstrips + 1, sizeof(pvr_strip));
    pvr_strip *s = &cur->strips[cur->nstrips++];
    s->param = cur->nparams - 1; s->first = cur->nverts; s->count = 0; s->list = cur_list;
    strip_open = 1;
}
static pvr_vert *new_vert(void) {
    if (!strip_open) open_strip();
    cur->verts = grow(cur->verts, &cur->cap_verts, cur->nverts + 1, sizeof(pvr_vert));
    cur->strips[cur->nstrips - 1].count++;
    pvr_vert *v = &cur->verts[cur->nverts++];
    memset(v, 0, sizeof *v);
    return v;
}

static void global_param(const uint32_t *d, int words) {
    uint32_t pcw = d[0];
    int para = pcw >> 29;
    if (cur_list < 0) cur_list = (pcw >> 24) & 7;
    if (cur_list > 4) cur_list = 0;
    close_strip();
    if (!cur) return;
    cur->params = grow(cur->params, &cur->cap_params, cur->nparams + 1, sizeof(pvr_param));
    pvr_param *p = &cur->params[cur->nparams++];
    memset(p, 0, sizeof *p);
    p->pcw = pcw; p->isp = d[1]; p->tsp = d[2]; p->tcw = d[3]; p->list = cur_list;
    p->stride = (uint8_t)(hw_reg(0x005F80E4u) & 31);   /* the stride register can change between objects */
    p->sprite = para == 5;
    if (is_modvol_list(cur_list)) { vtx_type = 17; vtx_words = 16; p->modvol = 1; return; }
    int tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, uv = pcw & 1, vol = (pcw >> 6) & 1, ofs = (pcw >> 2) & 1;
    /* the TA copies the object-control bits into the ISP word */
    p->isp = (p->isp & ~0x03C00000u) | (uint32_t)tex << 25 | (uint32_t)ofs << 24 | ((pcw >> 1) & 1u) << 23 | (uint32_t)uv << 22;
    if (para == 5) {
        sprite_base = d[4]; sprite_ofs = d[5];
        vtx_type = tex ? 16 : 15; vtx_words = 16; return;
    }
    if (vol) { p->tsp = d[2]; p->tcw = d[3]; }   /* volume 0 only; volume 1 ignored for now */
    if (col == 2) {                                /* intensity mode 1: face colour in this parameter */
        if (words == 16 || (!vol && !ofs)) {
            const uint32_t *fc = vol ? d + 8 : (words == 16 ? d + 8 : d + 4);
            for (int i = 0; i < 4; i++) face_col[i] = f_of(fc[i]);
            if (words == 16 && !vol) for (int i = 0; i < 4; i++) face_ofs[i] = f_of(d[12 + i]);
        }
    }
    if (!vol) {
        if (!tex) vtx_type = col == 0 ? 0 : col == 1 ? 1 : 2;
        else vtx_type = col == 0 ? (uv ? 4 : 3) : col == 1 ? (uv ? 6 : 5) : (uv ? 8 : 7);
    } else {
        if (!tex) vtx_type = col >= 2 ? 10 : 9;
        else vtx_type = col >= 2 ? (uv ? 14 : 13) : (uv ? 12 : 11);
    }
    vtx_words = (vtx_type == 5 || vtx_type == 6 || (vtx_type >= 11 && vtx_type <= 14)) ? 16 : 8;
}
static int global_words(uint32_t pcw) {
    int para = pcw >> 29;
    int list = cur_list >= 0 ? cur_list : (int)((pcw >> 24) & 7);
    if (para != 4 || is_modvol_list(list)) return 8;
    int col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, ofs = (pcw >> 2) & 1;
    if (col == 2 && (vol || ofs)) return 16;     /* polygon types 2 and 4 */
    return 8;
}

static void vertex_param(const uint32_t *d) {
    if (!cur || cur->nparams == 0) return;
    pvr_param *p = &cur->params[cur->nparams - 1];
    if (p->modvol) { if (d[0] & 0x10000000u) close_strip(); return; }   /* modifier volumes: not drawn yet */
    if (p->sprite) {
        close_strip();
        float A[3] = {f_of(d[1]), f_of(d[2]), f_of(d[3])}, B[3] = {f_of(d[4]), f_of(d[5]), f_of(d[6])};
        float C[3] = {f_of(d[7]), f_of(d[8]), f_of(d[9])}, D[3] = {f_of(d[10]), f_of(d[11]), 0};
        /* D's depth from the plane through A, B, C */
        float ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2], vx = C[0] - A[0], vy = C[1] - A[1], vz = C[2] - A[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        D[2] = nz != 0 ? A[2] - (nx * (D[0] - A[0]) + ny * (D[1] - A[1])) / nz : A[2];
        float ua = 0, va = 0, ub = 0, vb = 0, uc = 0, vc = 0;
        if (vtx_type == 16) { ua = uv16(d[13] >> 16); va = uv16(d[13] & 0xFFFF); ub = uv16(d[14] >> 16); vb = uv16(d[14] & 0xFFFF); uc = uv16(d[15] >> 16); vc = uv16(d[15] & 0xFFFF); }
        /* strip order A B D C makes the quad */
        const float *P[4] = {A, B, D, C};
        float U[4] = {ua, ub, ua + uc - ub, uc}, V[4] = {va, vb, va + vc - vb, vc};
        for (int i = 0; i < 4; i++) {
            pvr_vert *v = new_vert();
            v->x = P[i][0]; v->y = P[i][1]; v->z = P[i][2]; v->u = U[i]; v->v = V[i];
            v->col = sprite_base; v->ofs = sprite_ofs;
        }
        close_strip();
        return;
    }
    pvr_vert *v = new_vert();
    v->x = f_of(d[1]); v->y = f_of(d[2]); v->z = f_of(d[3]);
    switch (vtx_type) {
    case 0: case 9:  v->col = d[6]; break;
    case 1:          v->col = pack_f(f_of(d[4]), f_of(d[5]), f_of(d[6]), f_of(d[7])); break;
    case 2: case 10: v->col = intensity(face_col, f_of(d[6])); break;
    case 3: case 11: v->u = f_of(d[4]); v->v = f_of(d[5]); v->col = d[6]; v->ofs = d[7]; break;
    case 4: case 12: v->u = uv16(d[4] >> 16); v->v = uv16(d[4] & 0xFFFF); v->col = d[6]; v->ofs = d[7]; break;
    case 5:          v->u = f_of(d[4]); v->v = f_of(d[5]);
                     v->col = pack_f(f_of(d[8]), f_of(d[9]), f_of(d[10]), f_of(d[11]));
                     v->ofs = pack_f(f_of(d[12]), f_of(d[13]), f_of(d[14]), f_of(d[15])); break;
    case 6:          v->u = uv16(d[4] >> 16); v->v = uv16(d[4] & 0xFFFF);
                     v->col = pack_f(f_of(d[8]), f_of(d[9]), f_of(d[10]), f_of(d[11]));
                     v->ofs = pack_f(f_of(d[12]), f_of(d[13]), f_of(d[14]), f_of(d[15])); break;
    case 7: case 13: v->u = f_of(d[4]); v->v = f_of(d[5]);
                     v->col = intensity(face_col, f_of(d[6])); v->ofs = intensity(face_ofs, f_of(d[7])); break;
    case 8: case 14: v->u = uv16(d[4] >> 16); v->v = uv16(d[4] & 0xFFFF);
                     v->col = intensity(face_col, f_of(d[6])); v->ofs = intensity(face_ofs, f_of(d[7])); break;
    }
    if (d[0] & 0x10000000u) close_strip();     /* end of strip */
}

void pvr_ta_unit(const uint32_t *d) {
    ta_units++;
    if (npend) {                               /* second half of a 64-byte parameter */
        memcpy(pend + 8, d, 32); npend = 0;
        if (need_words == 16 && (pend[0] >> 29) == 7) vertex_param(pend);
        else global_param(pend, 16);
        return;
    }
    uint32_t pcw = d[0];
    switch (pcw >> 29) {
    case 0:                                    /* end of list */
        close_strip();
        if (cur_list >= 0) {
            if (cur) cur->list_done |= 1u << cur_list;
            hw_raise_nrm(list_irq[cur_list]);
        }
        cur_list = -1;
        return;
    case 1: case 2: return;                    /* user tile clip, object list set */
    case 4: case 5: {
        int w = (pcw >> 29) == 5 ? 8 : global_words(pcw);
        if (w == 16) { memcpy(pend, d, 32); npend = 1; need_words = 16; return; }
        global_param(d, 8);
        return;
    }
    case 7:
        if (vtx_words == 16) { memcpy(pend, d, 32); npend = 1; need_words = 16; return; }
        vertex_param(d);
        return;
    default:
        ta_bad++;
        return;
    }
}

/* bytes arriving at the TA polygon FIFO; always whole 32-byte units in practice */
void pvr_ta_write(const uint8_t *src, uint32_t len) {
    static uint32_t part[8]; static uint32_t have;
    while (len) {
        uint32_t n = 32 - have; if (n > len) n = len;
        memcpy((uint8_t *)part + have, src, n); have += n; src += n; len -= n;
        if (have == 32) { pvr_ta_unit(part); have = 0; }
    }
}

void pvr_ta_list_init(void) {
    pvr_ta_init++;
    uint32_t base = hw_reg(0x005F8124u);     /* TA_ISP_BASE */
    cur = frame_for(base, 1);
    frame_reset(cur);
    { static uint32_t seq; cur->seq = ++seq; }
    cur->list_done = 0;
    cur_list = -1; npend = 0; strip_open = 0;
}
void pvr_ta_list_cont(void) { cur_list = -1; npend = 0; strip_open = 0; }

/* ---------------- YUV converter ----------------
 * Input: YUV420 macroblocks of 384 bytes (U 8x8, V 8x8, then Y as four 8x8
 * blocks: top-left, top-right, bottom-left, bottom-right) or YUV422 (512 bytes).
 * Output: a YUV422 texture at TA_YUV_TEX_BASE, row pitch = width in macroblocks * 16,
 * each texel pair stored as (U | Y0 << 8), (V | Y1 << 8). ISTNRM bit 6 when all
 * macroblocks of the frame have arrived. */
static uint8_t yuv_mb[512]; static uint32_t yuv_have, yuv_count;
uint64_t yuv_frames;
static void yuv_put(uint32_t a64, uint16_t v) {
    uint32_t o = vram64_off(a64); memcpy(vram + o, &v, 2); vram_touch_lin(o);
}
static void yuv_macroblock(void) {
    uint32_t ctrl = hw_reg(0x005F814Cu), base = hw_reg(0x005F8148u) & 0xFFFFF8u;
    uint32_t wmb = (ctrl & 0x3F) + 1, hmb = ((ctrl >> 8) & 0x3F) + 1;
    int fmt422 = (ctrl >> 24) & 1;
    uint32_t mx = yuv_count % wmb, my = yuv_count / wmb, pitch = wmb * 16;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x += 2) {
        uint8_t U, V, Y0, Y1;
        if (!fmt422) {
            U = yuv_mb[(y / 2) * 8 + x / 2]; V = yuv_mb[64 + (y / 2) * 8 + x / 2];
            const uint8_t *Yb = yuv_mb + 128 + ((y >= 8) * 2 + (x >= 8)) * 64;
            Y0 = Yb[(y & 7) * 8 + (x & 7)]; Y1 = Yb[(y & 7) * 8 + (x & 7) + 1];
        } else {                                    /* 422 input: U 8x16, V 8x16, Y 4 blocks */
            U = yuv_mb[y * 8 + x / 2]; V = yuv_mb[128 + y * 8 + x / 2];
            const uint8_t *Yb = yuv_mb + 256 + ((y >= 8) * 2 + (x >= 8)) * 64;
            Y0 = Yb[(y & 7) * 8 + (x & 7)]; Y1 = Yb[(y & 7) * 8 + (x & 7) + 1];
        }
        uint32_t a = base + ((my * 16 + y) * pitch + mx * 16 + x) * 2;
        yuv_put(a, (uint16_t)(U | Y0 << 8)); yuv_put(a + 2, (uint16_t)(V | Y1 << 8));
    }
    if (++yuv_count >= wmb * hmb) { yuv_count = 0; yuv_frames++; hw_raise_nrm(6); }
    hw_set_reg(0x005F8150u, yuv_count);
}
void pvr_yuv_write(const uint8_t *src, uint32_t len) {
    uint32_t mbsize = (hw_reg(0x005F814Cu) >> 24) & 1 ? 512 : 384;
    while (len) {
        uint32_t n = mbsize - yuv_have; if (n > len) n = len;
        memcpy(yuv_mb + yuv_have, src, n); yuv_have += n; src += n; len -= n;
        if (yuv_have == mbsize) { yuv_have = 0; yuv_macroblock(); }
    }
}
void pvr_yuv_reset(void) { yuv_have = 0; yuv_count = 0; }

/* ---------------- render ---------------- */
static pvr_render_fn render_fn;
void pvr_set_renderer(pvr_render_fn fn) { render_fn = fn; }

void pvr_start_render(void) {
    pvr_render_count++;
    uint32_t base = hw_reg(0x005F8020u);     /* PARAM_BASE */
    pvr_frame *f = frame_for(base, 0);
    /* a match with an old, stale list (e.g. the legal screen's, built in the same
     * 1 MB block long ago) would flash that picture: only the newest two count */
    if (f && cur && f != cur && f->seq + 1 < cur->seq) { static int n; if (n++ < 5) fprintf(stderr, "[pvr] ignored stale list #%u (newest #%u) at render %llu\n", f->seq, cur->seq, (unsigned long long)pvr_render_count); f = NULL; }
    static int dbg; if (dbg < 4) { dbg++; fprintf(stderr, "[pvr] STARTRENDER PARAM_BASE %08X REGION_BASE %08X FB_W_SOF1 %08X; TA base was %06X -> %s\n",
        base, hw_reg(0x005F802Cu), hw_reg(0x005F8060u), cur ? cur->param_base : 0, f ? "match" : "no list"); }
    if (!f) f = cur;                         /* fall back to the most recent TA list */
    { static long wantp = -2; if (wantp == -2) wantp = getenv("DC_DUMPPARAMS") ? atol(getenv("DC_DUMPPARAMS")) : -1;
      if (f && (long)pvr_render_count == wantp) for (uint32_t i = 0; i < f->nparams; i++) {
        float zl = 1e30f, zh = -1e30f, xl = 1e30f, xh = -1e30f, yl = 1e30f, yh = -1e30f; int ns = 0;
        for (uint32_t k = 0; k < f->nstrips; k++) if (f->strips[k].param == i) { ns++;
            for (uint32_t q = 0; q < f->strips[k].count; q++) { const pvr_vert *v = &f->verts[f->strips[k].first + q];
                if (v->z < zl) zl = v->z; if (v->z > zh) zh = v->z; if (v->x < xl) xl = v->x; if (v->x > xh) xh = v->x; if (v->y < yl) yl = v->y; if (v->y > yh) yh = v->y; } }
        fprintf(stderr, "[params] %u list %d depthfn %u zw %u tcw %08X strips %d z %.5f..%.5f box %.0f,%.0f-%.0f,%.0f\n", i, f->params[i].list,
                f->params[i].isp >> 29, (f->params[i].isp >> 26) & 1, f->params[i].tcw, ns, zl, zh, xl, yl, xh, yh); } }
    { static long want = -2; if (want == -2) want = getenv("DC_DUMPLIST") ? atol(getenv("DC_DUMPLIST")) : -1;
      /* DC_DUMPLISTF=<field>: dump the first render at or after that display field instead */
      static long wantf = -2; static int donef; if (wantf == -2) wantf = getenv("DC_DUMPLISTF") ? atol(getenv("DC_DUMPLISTF")) : -1;
      uint64_t dc_frame_count(void);
      int byf = wantf >= 0 && !donef && (long)dc_frame_count() >= wantf; if (byf) donef = 1;
      if (f && ((long)pvr_render_count == want || byf)) {
        for (uint32_t i = 0; i < f->nparams; i++) fprintf(stderr, "[list] param %u: pcw %08X isp %08X tsp %08X tcw %08X list %d spr %d\n", i, f->params[i].pcw, f->params[i].isp, f->params[i].tsp, f->params[i].tcw, f->params[i].list, f->params[i].sprite);
        long sel = getenv("DC_DUMPSEL") ? atol(getenv("DC_DUMPSEL")) : -1; int shown = 0;
        for (uint32_t i = 0; i < f->nstrips && shown < (getenv("DC_DUMPMAX") ? atoi(getenv("DC_DUMPMAX")) : 40); i++) { const pvr_strip *st = &f->strips[i];
          if (sel >= 0 && (long)st->param != sel) continue; shown++;
          fprintf(stderr, "[list] strip %u param %u list %d:", i, st->param, st->list);
          for (uint32_t k = 0; k < st->count; k++) { const pvr_vert *v = &f->verts[st->first + k]; fprintf(stderr, " (%.1f,%.1f,%.4f uv %.3f,%.3f c %08X)", v->x, v->y, v->z, v->u, v->v, v->col); }
          fprintf(stderr, "\n"); } } }
    if (f && render_fn) render_fn(f);
    static int logged;
    if (f && logged < 3) { logged++;
        fprintf(stderr, "[pvr] render #%llu: base %06X, %u params, %u strips, %u verts (op %u, pt %u, tr %u)\n",
                (unsigned long long)pvr_render_count, base, f->nparams, f->nstrips, f->nverts,
                f->list_count[0], f->list_count[4], f->list_count[2]); }
    hw_raise_nrm(2); hw_raise_nrm(1); hw_raise_nrm(0);   /* render done: TSP, ISP, video */
}

void pvr_report(void) {
    fprintf(stderr, "TA: %u units parsed, %u unknown; YUV frames %llu\n", ta_units, ta_bad, (unsigned long long)yuv_frames);
}
