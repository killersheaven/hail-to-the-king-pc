/* Character models read straight from the game's .DC files (launcher model viewer).
 *
 * .DC container: u32 count at 0x14, then at 0x18 a table of {u32 type, u32 flag,
 * u32 offset, u32 size}. flag 1 = a texture (16-byte prefix + "PVRT" block).
 * type = kind << 23 | object << 12 | extra. A character is a mesh chunk of an even
 * kind, a skeleton chunk of type + (1 << 23), and a texture with the same object.
 *
 * Mesh chunk: u32 nparts, nparts u32 offsets (from the chunk start). Part: u32 0,
 * u32 nv, u32 nv, u32 ntri; nv x {x, y, z, u, v} f32; nv x normal (3 f32);
 * nv x u16 bone; ntri x 3 u16 corner indices. Positions are relative to their bone.
 * Skeleton chunk: u32 type, u32 nbones, u32 ?, then per bone 16 f32 (4x4 local
 * matrix, row-vector), 3 f32 translation (replaces the matrix's row 3) and an i32
 * parent (-1 = root). Rest pose: world = local * world[parent].
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "models.h"

int disc_read_sector(uint32_t fad, uint8_t *dst);
int iso_find(const char *path, uint32_t *fad, uint32_t *size);
uint8_t *iso_load(const char *path, uint32_t *size_out);
void iso_list_cb(void (*fn)(const char *name, uint32_t size, void *ud), void *ud);

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* ---- scanning: chunk tables only (a few sectors per file) ---- */
typedef struct { model_ent *out; int max, n; } scan_ctx;
static void scan_file(const char *name, uint32_t size, void *ud) {
    scan_ctx *c = ud; size_t l = strlen(name);
    if (l < 4 || strcasecmp(name + l - 3, ".DC") || (l > 4 && (name[l - 4] == 'V' || name[l - 4] == 'v'))) return;
    uint32_t fad, sz; if (!iso_find(name, &fad, &sz) || sz < 2048) return;
    uint8_t s0[2048]; if (!disc_read_sector(fad, s0)) return;
    uint32_t cnt = rd32(s0 + 20); if (cnt == 0 || cnt > 4000) return;
    uint32_t tb = 0x18 + cnt * 16, ns = (tb + 2047) / 2048;
    uint8_t *t = malloc(ns * 2048);
    for (uint32_t k = 0; k < ns; k++) if (!disc_read_sector(fad + k, t + k * 2048)) { free(t); return; }
    for (uint32_t i = 0; i < cnt && c->n < c->max; i++) {
        uint32_t ty = rd32(t + 0x18 + 16 * i), fl = rd32(t + 0x1C + 16 * i);
        uint32_t kind = ty >> 23, obj = (ty >> 12) & 0x7FF;
        if (fl || (ty & 0xFFF) || (kind & 1)) continue;
        int skel = 0, tex = 0;
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t t2 = rd32(t + 0x18 + 16 * j), f2 = rd32(t + 0x1C + 16 * j);
            if (t2 == ty + 0x800000u && !f2) skel = 1;
            if (f2 == 1 && ((t2 >> 12) & 0x7FF) == obj) tex = 1;
        }
        if (!skel || !tex) continue;
        int dup = 0; for (int k = 0; k < c->n; k++) if (c->out[k].type == ty) dup = 1;
        if (dup) continue;
        model_ent *e = &c->out[c->n++];
        snprintf(e->file, sizeof e->file, "%s", name); e->type = ty;
    }
    free(t); (void)size;
}
int models_scan(model_ent *out, int max) {
    scan_ctx c = { out, max, 0 };
    iso_list_cb(scan_file, &c);
    return c.n;
}

/* ---- textures ---- */
static uint32_t twid(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h, r = 0, bx = x / m, by = y / m; x %= m; y %= m;
    for (uint32_t b = 0, s = 1; s < m; b++, s <<= 1) r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return r + (bx + by) * m * m;
}
static uint32_t px16(uint16_t p, int f) {   /* -> RGBA bytes in memory order */
    uint32_t a, r, g, b;
    if (f == 0) { a = p & 0x8000 ? 255 : 0; r = (p >> 10 & 31) * 255 / 31; g = (p >> 5 & 31) * 255 / 31; b = (p & 31) * 255 / 31; }
    else if (f == 1) { a = 255; r = (p >> 11 & 31) * 255 / 31; g = (p >> 5 & 63) * 255 / 63; b = (p & 31) * 255 / 31; }
    else { a = (p >> 12 & 15) * 17; r = (p >> 8 & 15) * 17; g = (p >> 4 & 15) * 17; b = (p & 15) * 17; }
    return r | g << 8 | b << 16 | a << 24;
}
static uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static uint32_t *decode_pvrt(const uint8_t *p, size_t avail, int *w_out, int *h_out) {
    if (avail < 16 || memcmp(p, "PVRT", 4)) return NULL;
    int pf = p[8], df = p[9]; uint32_t w = p[12] | p[13] << 8, h = p[14] | p[15] << 8;
    if (w < 8 || h < 8 || w > 1024 || h > 1024 || pf > 3) return NULL;
    int tw = df == 1 || df == 13, rect = df == 9;
    if (!tw && !rect) return NULL;
    if (avail < 16 + (size_t)w * h * 2) return NULL;
    const uint8_t *d = p + 16;
    uint16_t *lin = malloc((size_t)w * h * 2);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint32_t i = tw ? twid(x, y, w, h) : y * w + x;
        lin[y * w + x] = (uint16_t)(d[i * 2] | d[i * 2 + 1] << 8);
    }
    uint32_t *o = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint16_t q = lin[y * w + x];
        if (pf < 3) { o[y * w + x] = px16(q, pf); continue; }
        uint16_t p0 = lin[y * w + (x & ~1u)], p1 = lin[y * w + (x | 1u)];
        float Y = (float)(q >> 8), U = (float)(p0 & 0xFF) - 128, V = (float)(p1 & 0xFF) - 128;
        o[y * w + x] = clamp8(Y + 1.402f * V) | clamp8(Y - 0.344f * U - 0.714f * V) << 8 | clamp8(Y + 1.772f * U) << 16 | 0xFFu << 24;
    }
    free(lin); *w_out = (int)w; *h_out = (int)h;
    return o;
}

/* ---- loading ---- */
static void mat_mul(const float *a, const float *b, float *c) {   /* c = a * b (4x4, row-major) */
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; t[i * 4 + j] = s; }
    memcpy(c, t, sizeof t);
}
static char cache_name[16]; static uint8_t *cache_d; static uint32_t cache_n;   /* last file read */
model_t *model_load(const model_ent *e) {
    uint32_t n = 0; uint8_t *d;
    if (cache_d && !strcmp(cache_name, e->file)) { d = cache_d; n = cache_n; }
    else {
        free(cache_d); cache_d = NULL; cache_name[0] = 0;
        d = iso_load(e->file, &n); if (!d) return NULL;
        cache_d = d; cache_n = n; snprintf(cache_name, sizeof cache_name, "%s", e->file);
    }
    if (n < 0x18) return NULL;
    uint32_t cnt = rd32(d + 20); if (0x18 + (size_t)cnt * 16 > n) { return NULL; }
    uint32_t mo = 0, ms = 0, so = 0, ss = 0, to = 0, ts = 0; int have_t = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t ty = rd32(d + 0x18 + 16 * i), fl = rd32(d + 0x1C + 16 * i), of = rd32(d + 0x20 + 16 * i), sz = rd32(d + 0x24 + 16 * i);
        if ((uint64_t)of + sz > n) continue;
        if (ty == e->type && !fl) { mo = of; ms = sz; }
        if (ty == e->type + 0x800000u && !fl) { so = of; ss = sz; }
        if (fl == 1 && !have_t && ((ty >> 12) & 0x7FF) == ((e->type >> 12) & 0x7FF)) { to = of; ts = sz; have_t = 1; }
    }
    if (!ms || !ss) { return NULL; }
    model_t *m = calloc(1, sizeof *m);
    /* skeleton -> rest-pose world matrices */
    uint32_t nb = rd32(d + so + 4);
    if (nb == 0 || nb > 128 || 12 + nb * 80 > ss) { free(m); return NULL; }
    float (*W)[16] = calloc(nb, sizeof *W);
    for (uint32_t j = 0; j < nb; j++) {
        const uint8_t *b = d + so + 4 * (3 + 20 * j);
        float L[16]; for (int k = 0; k < 16; k++) L[k] = rdf(b + 4 * k);
        L[12] = rdf(b + 64); L[13] = rdf(b + 68); L[14] = rdf(b + 72);
        int32_t par = (int32_t)rd32(b + 76);
        if (par >= 0 && (uint32_t)par < j) mat_mul(L, W[par], W[j]); else memcpy(W[j], L, sizeof L);
    }
    /* mesh parts */
    const uint8_t *c = d + mo;
    uint32_t np = rd32(c); if (np == 0 || np > MODEL_MAXPARTS || 4 + np * 4 > ms) np = 0;
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (uint32_t pi = 0; pi < np; pi++) {
        uint32_t off = rd32(c + 4 + 4 * pi); if (off + 16 > ms) break;
        const uint8_t *p = c + off;
        uint32_t nv = rd32(p + 4), nt = rd32(p + 12);
        if (nv == 0 || nv > 8000 || nt > 16000 || off + 16 + (uint64_t)nv * 34 + nt * 6 > ms) break;
        model_part *mp = &m->parts[m->nparts++];
        mp->nv = (int)nv; mp->nt = (int)nt;
        mp->v = malloc(nv * sizeof *mp->v); mp->idx = malloc(nt * 3 * sizeof *mp->idx);
        const uint8_t *pv = p + 16, *pn = pv + nv * 20, *pb = pn + nv * 12, *pt = pb + nv * 2;
        mp->bone0 = 0xFFFF; for (uint32_t k = 0; k < nv; k++) { uint16_t bi = (uint16_t)(pb[2 * k] | pb[2 * k + 1] << 8); if (bi < mp->bone0) mp->bone0 = bi; }
        mp->primary = 1; for (int q = 0; q + 1 < m->nparts; q++) if (m->parts[q].bone0 == mp->bone0) mp->primary = 0;
        for (uint32_t k = 0; k < nv; k++) {
            float x = rdf(pv + 20 * k), y = rdf(pv + 20 * k + 4), z = rdf(pv + 20 * k + 8);
            float nx = rdf(pn + 12 * k), ny = rdf(pn + 12 * k + 4), nz = rdf(pn + 12 * k + 8);
            uint32_t bi = pb[2 * k] | pb[2 * k + 1] << 8; if (bi >= nb) bi = 0;
            const float *M = W[bi];
            model_vert *v = &mp->v[k];
            for (int r = 0; r < 3; r++) {
                v->p[r] = x * M[r] + y * M[4 + r] + z * M[8 + r] + M[12 + r];
                v->n[r] = nx * M[r] + ny * M[4 + r] + nz * M[8 + r];
                if (v->p[r] < lo[r]) lo[r] = v->p[r]; if (v->p[r] > hi[r]) hi[r] = v->p[r];
            }
            float l = sqrtf(v->n[0] * v->n[0] + v->n[1] * v->n[1] + v->n[2] * v->n[2]); if (l > 1e-6f) for (int r = 0; r < 3; r++) v->n[r] /= l;
            v->uv[0] = rdf(pv + 20 * k + 12); v->uv[1] = rdf(pv + 20 * k + 16);
        }
        for (uint32_t k = 0; k < nt * 3; k++) { uint16_t ix = (uint16_t)(pt[2 * k] | pt[2 * k + 1] << 8); mp->idx[k] = ix < nv ? ix : 0; }
    }
    free(W);
    if (!m->nparts) { free(m); return NULL; }
    for (int r = 0; r < 3; r++) { m->center[r] = (lo[r] + hi[r]) / 2; }
    float rad = 0; for (int r = 0; r < 3; r++) if ((hi[r] - lo[r]) / 2 > rad) rad = (hi[r] - lo[r]) / 2;
    m->radius = rad > 1e-4f ? rad : 1;
    m->height = hi[1] - lo[1];
    /* the body is the biggest part; the rest are props and alternate hands. Frame the body. */
    for (int i = 1; i < m->nparts; i++) if (m->parts[i].nt > m->parts[m->body].nt) m->body = i;
    for (int r = 0; r < 3; r++) { lo[r] = 1e9f; hi[r] = -1e9f; }
    for (int k = 0; k < m->parts[m->body].nv; k++) for (int r = 0; r < 3; r++) {
        float q = m->parts[m->body].v[k].p[r]; if (q < lo[r]) lo[r] = q; if (q > hi[r]) hi[r] = q; }
    for (int r = 0; r < 3; r++) m->center[r] = (lo[r] + hi[r]) / 2;
    rad = 0; for (int r = 0; r < 3; r++) if ((hi[r] - lo[r]) / 2 > rad) rad = (hi[r] - lo[r]) / 2;
    m->radius = rad > 1e-4f ? rad : 1; m->height = hi[1] - lo[1];
    if (have_t) m->tex = decode_pvrt(d + to + 16, ts > 16 ? ts - 16 : 0, &m->tw, &m->th);
    return m;
}
void model_free(model_t *m) {
    if (!m) return;
    for (int i = 0; i < m->nparts; i++) { free(m->parts[i].v); free(m->parts[i].idx); }
    free(m->tex); free(m);
}
