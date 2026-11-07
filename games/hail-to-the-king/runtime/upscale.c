/* Edge-directed 2x image upscaler for menu and HUD art.
 *
 * Each 2x step fills the new pixels in two passes on the doubled grid:
 *   1. centre pixels (between four originals): measure how much the image
 *      changes along each diagonal over a 4x4 neighbourhood and interpolate
 *      (4-tap cubic) along the diagonal it changes least along, i.e. along
 *      the edge rather than across it; blend both when neither dominates;
 *   2. edge pixels (between two originals): the same on the 45-degree
 *      rotated lattice formed by the originals and the centre pixels.
 * Results are clamped to the range of the nearest known pixels (no ringing
 * halos). Works on premultiplied RGBA so transparent edges do not bleed.
 * Applying the step twice gives 4x.
 *
 * Written for this project; the approach is the classic edge-directed
 * interpolation idea (as in NEDI / xBR-family filters), no code was copied.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float c[4]; } px;   /* premultiplied r, g, b, a in 0..1 */

static inline float lum(const px *p) { return 0.299f * p->c[0] + 0.587f * p->c[1] + 0.114f * p->c[2] + 0.5f * p->c[3]; }

typedef struct { px *p; int w, h; uint8_t *known; } grid;   /* doubled grid under construction */

static inline const px *G(const grid *g, int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0; if (x >= g->w) x = g->w - 1; if (y >= g->h) y = g->h - 1;
    return &g->p[y * g->w + x];
}

/* cubic 4-tap along a line of samples a b [new] c d */
static inline void cubic(px *o, const px *a, const px *b, const px *c, const px *d) {
    for (int k = 0; k < 4; k++) o->c[k] = (-a->c[k] + 9.0f * b->c[k] + 9.0f * c->c[k] - d->c[k]) / 16.0f;
}

/* fill new pixel at (x, y); (dx1, dy1) and (dx2, dy2) are the two candidate edge directions in grid units
 * between already-known samples: samples sit at +-1 and +-3 steps along each direction */
static void fill(grid *g, int x, int y, int dx1, int dy1, int dx2, int dy2) {
    /* variation along each direction: differences between neighbouring known samples on parallel lines */
    float v1 = 0, v2 = 0;
    for (int s = -1; s <= 1; s += 2) {
        /* the lines parallel to each direction through the neighbours at +-1 step of the other direction;
         * on those lines the known samples sit at even steps */
        for (int t = -2; t <= 0; t += 2) {
            v1 += fabsf(lum(G(g, x + t * dx1 + s * dx2, y + t * dy1 + s * dy2)) - lum(G(g, x + (t + 2) * dx1 + s * dx2, y + (t + 2) * dy1 + s * dy2)));
            v2 += fabsf(lum(G(g, x + t * dx2 + s * dx1, y + t * dy2 + s * dy1)) - lum(G(g, x + (t + 2) * dx2 + s * dx1, y + (t + 2) * dy2 + s * dy1)));
        }
        /* the line through the new pixel itself: known samples at odd steps */
        v1 += fabsf(lum(G(g, x + s * dx1, y + s * dy1)) - lum(G(g, x + 3 * s * dx1, y + 3 * s * dy1)));
        v2 += fabsf(lum(G(g, x + s * dx2, y + s * dy2)) - lum(G(g, x + 3 * s * dx2, y + 3 * s * dy2)));
    }
    v1 += fabsf(lum(G(g, x - dx1, y - dy1)) - lum(G(g, x + dx1, y + dy1))) * 2;
    v2 += fabsf(lum(G(g, x - dx2, y - dy2)) - lum(G(g, x + dx2, y + dy2))) * 2;
    px a, b;
    cubic(&a, G(g, x - 3 * dx1, y - 3 * dy1), G(g, x - dx1, y - dy1), G(g, x + dx1, y + dy1), G(g, x + 3 * dx1, y + 3 * dy1));
    cubic(&b, G(g, x - 3 * dx2, y - 3 * dy2), G(g, x - dx2, y - dy2), G(g, x + dx2, y + dy2), G(g, x + 3 * dx2, y + 3 * dy2));
    /* weight: smooth along a direction -> trust interpolation along it */
    float w1 = 1.0f / (1e-4f + v1 * v1 * v1 * v1), w2 = 1.0f / (1e-4f + v2 * v2 * v2 * v2);
    px *o = &g->p[y * g->w + x];
    const px *n[4] = { G(g, x - dx1, y - dy1), G(g, x + dx1, y + dy1), G(g, x - dx2, y - dy2), G(g, x + dx2, y + dy2) };
    for (int k = 0; k < 4; k++) {
        float v = (w1 * a.c[k] + w2 * b.c[k]) / (w1 + w2);
        float lo = n[0]->c[k], hi = lo;
        for (int i = 1; i < 4; i++) { if (n[i]->c[k] < lo) lo = n[i]->c[k]; if (n[i]->c[k] > hi) hi = n[i]->c[k]; }
        o->c[k] = v < lo ? lo : v > hi ? hi : v;
    }
}

static px *step2x(const px *src, int w, int h) {
    grid g = { calloc((size_t)w * h * 4, sizeof(px)), w * 2, h * 2, NULL };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) g.p[(2 * y) * g.w + 2 * x] = src[y * w + x];
    /* pass 1: centres (odd, odd); known samples lie along the diagonals */
    for (int y = 1; y < g.h; y += 2) for (int x = 1; x < g.w; x += 2) fill(&g, x, y, 1, 1, 1, -1);
    /* pass 2: edge pixels (odd, even) and (even, odd); known samples lie horizontally and vertically */
    for (int y = 0; y < g.h; y++) for (int x = (y & 1) ? 0 : 1; x < g.w; x += 2) fill(&g, x, y, 1, 0, 0, 1);
    return g.p;
}

/* Each 2x step keeps the original samples on the even grid positions, which puts
 * them half an output pixel off their true centres: after all steps the image
 * sits (factor-1)/2 output pixels too far left and up. Shift it back
 * (integer part exactly, the remaining half pixel with a 4-tap cubic clamped to
 * its two middle samples), so glyphs line up with the game's texture
 * coordinates and are not clipped on the right or bled into from the row above. */
static void recentre(px *p, int w, int h, int factor) {
    int k = (factor - 1) / 2;                       /* shift = k + 0.5 */
    px *tmp = malloc((size_t)w * h * sizeof(px));
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? h : w, lines = pass ? w : h;
        for (int l = 0; l < lines; l++) for (int i = 0; i < n; i++) {
            #define AT(j) (pass ? &p[(size_t)((j) < 0 ? 0 : (j) >= n ? n - 1 : (j)) * w + l] : &p[(size_t)l * w + ((j) < 0 ? 0 : (j) >= n ? n - 1 : (j))])
            const px *a = AT(i - k - 2), *b = AT(i - k - 1), *c = AT(i - k), *d = AT(i - k + 1);
            #undef AT
            px *o = pass ? &tmp[(size_t)i * w + l] : &tmp[(size_t)l * w + i];
            for (int q = 0; q < 4; q++) {
                float v = (-a->c[q] + 9.0f * b->c[q] + 9.0f * c->c[q] - d->c[q]) / 16.0f;
                float lo = fminf(b->c[q], c->c[q]), hi = fmaxf(b->c[q], c->c[q]);
                o->c[q] = v < lo ? lo : v > hi ? hi : v;
            }
        }
        memcpy(p, tmp, (size_t)w * h * sizeof(px));
    }
    free(tmp);
}

/* src: RGBA8 (bytes R,G,B,A), returns a malloc'ed RGBA8 image of (w*factor) x (h*factor); factor 2 or 4 */
uint32_t *upscale_rgba(const uint32_t *src, int w, int h, int factor) {
    px *cur = malloc((size_t)w * h * sizeof(px));
    for (int i = 0; i < w * h; i++) {
        uint32_t v = src[i]; float a = (v >> 24) / 255.0f;
        cur[i].c[0] = (v & 0xFF) / 255.0f * a; cur[i].c[1] = (v >> 8 & 0xFF) / 255.0f * a;
        cur[i].c[2] = (v >> 16 & 0xFF) / 255.0f * a; cur[i].c[3] = a;
    }
    int cw = w, ch = h;
    for (int f = 1; f < factor; f *= 2) { px *n = step2x(cur, cw, ch); free(cur); cur = n; cw *= 2; ch *= 2; }
    if (factor > 1) recentre(cur, cw, ch, factor);
    uint32_t *out = malloc((size_t)cw * ch * 4);
    for (int i = 0; i < cw * ch; i++) {
        float a = cur[i].c[3], inv = a > 1e-6f ? 1.0f / a : 0;
        uint32_t r = (uint32_t)(fminf(1.0f, cur[i].c[0] * inv) * 255.0f + 0.5f), gg = (uint32_t)(fminf(1.0f, cur[i].c[1] * inv) * 255.0f + 0.5f);
        uint32_t b = (uint32_t)(fminf(1.0f, cur[i].c[2] * inv) * 255.0f + 0.5f), A = (uint32_t)(a * 255.0f + 0.5f);
        out[i] = r | gg << 8 | b << 16 | A << 24;
    }
    free(cur);
    return out;
}
