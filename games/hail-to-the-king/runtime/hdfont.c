/* HD font, made while the game runs (no game art is stored).
 *
 * The game's font sheet (256x256, white glyphs in alpha) holds a Courier-style
 * monospace font on a 14x6 grid, then the Y/B/A/X button icons and a tiny pixel
 * font. When the game uploads it, this redraws every glyph of the main font with
 * Courier Prime at 4x, keeping each glyph's cell, baseline and centre, so the
 * game's texture coordinates still fit. The icon rows get the edge-directed
 * upscale (or the chosen controller's 4x icons), the pixel font stays pixel-sharp.
 * Same method as tools/fontpack/fontpack.py, which made the test builds' sheet.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "third_party/courierprime_ttf.h"

uint32_t *upscale_rgba(const uint32_t *src, int w, int h, int factor);

enum { S = 4, W = 256, H = 256, MAIN_BOTTOM = 146, PIXEL_FONT_TOP = 167 };
static const char *rows[6] = { "ABCDEFGHIJKLMN", "OPQRSTUVWXYZab", "cdefghijklmnop",
                               "qrstuvwxyz0123", "456789?!.,;:+-", "=|&()%\"'_<>*[]" };
static const char *descenders = "gjpqyQ,;_|()[]";

typedef struct { int ok, ox0, oy0, ox1, oy1, cx0, cy0, cx1, cy1; } gl_box;
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
#define A(x, y) ((px[(y) * W + (x)] >> 24) & 0xFF)

/* px: the game's 256x256 sheet (RGBA bytes). icons4x: 288x96 RGBA icons to use instead
 * of the sheet's own (or NULL). Returns a 1024x1024 sheet, or NULL if it doesn't look right. */
uint32_t *hdfont_build(const uint32_t *px, const uint8_t *icons4x) {
    /* ---- rows of the main font ---- */
    int band[16][2], nb = 0;
    for (int y = 0; y < MAIN_BOTTOM; ) {
        int any = 0; for (int x = 0; x < W; x++) if (A(x, y)) { any = 1; break; }
        if (!any) { y++; continue; }
        int s = y;
        for (; y < MAIN_BOTTOM; y++) { any = 0; for (int x = 0; x < W; x++) if (A(x, y)) { any = 1; break; } if (!any) break; }
        if (nb < 16) { band[nb][0] = s; band[nb][1] = y; } nb++; y++;
    }
    if (nb != 6) return NULL;
    static gl_box g[128]; memset(g, 0, sizeof g);
    int row_base[6]; const float cw = W / 14.0f;
    for (int r = 0; r < 6; r++) {
        int top = r == 0 ? 0 : (band[r - 1][1] + band[r][0]) / 2;
        int bot = r == 5 ? MAIN_BOTTOM : (band[r][1] + band[r + 1][0]) / 2;
        uint8_t colany[W];
        for (int x = 0; x < W; x++) { colany[x] = 0; for (int y = top; y < bot; y++) if (A(x, y)) { colany[x] = 1; break; } }
        int cuts[16]; int n = (int)strlen(rows[r]); cuts[0] = 0;
        for (int k = 1; k < n; k++) {      /* split at the empty column nearest each nominal boundary */
            float nom = k * cw; int best = -1; float bd = 1e9f;
            for (int x = (int)nom - 6; x <= (int)nom + 6; x++) if (x >= 0 && x < W && !colany[x]) { float d = fabsf(x + 0.5f - nom); if (d < bd) { bd = d; best = x; } }
            cuts[k] = best >= 0 ? best : (int)lroundf(nom);
        }
        cuts[n] = W;
        int bottoms[16], nbt = 0;
        for (int k = 0; k < n; k++) {
            unsigned char ch = (unsigned char)rows[r][k];
            int x0 = cuts[k], x1 = cuts[k + 1], mnx = W, mny = H, mxx = -1, mxy = -1;
            for (int y = top; y < bot; y++) for (int x = x0; x < x1; x++) if (A(x, y)) {
                if (x < mnx) mnx = x; if (x > mxx) mxx = x; if (y < mny) mny = y; if (y > mxy) mxy = y; }
            if (mxx < 0) continue;
            gl_box *b = &g[ch]; b->ok = 1;
            b->ox0 = mnx; b->oy0 = mny; b->ox1 = mxx + 1; b->oy1 = mxy + 1; b->cx0 = x0; b->cy0 = top; b->cx1 = x1; b->cy1 = bot;
            if (!strchr(descenders, ch) && (isalnum(ch) || strchr("&%?!", ch))) bottoms[nbt++] = b->oy1;
        }
        if (nbt) { qsort(bottoms, (size_t)nbt, sizeof *bottoms, cmp_int); row_base[r] = nbt & 1 ? bottoms[nbt / 2] : (bottoms[nbt / 2 - 1] + bottoms[nbt / 2] + 1) / 2; }
        else row_base[r] = band[r][1];
    }
    if (!g['A'].ok) return NULL;

    /* ---- size Courier Prime so capitals match the original cap height ---- */
    stbtt_fontinfo f;
    if (!stbtt_InitFont(&f, courierprime_ttf, stbtt_GetFontOffsetForIndex(courierprime_ttf, 0))) return NULL;
    int cap_h = g['A'].oy1 - g['A'].oy0; float scale = 0;
    for (int size = 10; size < 400; size++) {
        float sc = stbtt_ScaleForMappingEmToPixels(&f, (float)size); int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&f, 'A', sc, sc, &x0, &y0, &x1, &y1);
        if (y1 - y0 >= cap_h * S) { scale = sc; break; }
    }
    if (!scale) return NULL;
    float ratios[5]; int nr = 0;                   /* horizontal fit from the widest glyphs */
    for (const char *c = "MWHNA"; *c; c++) {
        int x0, y0, x1, y1; stbtt_GetCodepointBitmapBox(&f, *c, scale, scale, &x0, &y0, &x1, &y1);
        gl_box *b = &g[(unsigned char)*c]; if (!b->ok) continue;
        ratios[nr++] = (float)((b->ox1 - b->ox0) * S) / (float)(x1 - x0 > 1 ? x1 - x0 : 1);
    }
    for (int i = 0; i < nr; i++) for (int j = i + 1; j < nr; j++) if (ratios[j] < ratios[i]) { float t = ratios[i]; ratios[i] = ratios[j]; ratios[j] = t; }
    float xs = nr ? ratios[nr / 2] : 1; if (xs < 0.85f) xs = 0.85f; if (xs > 1.15f) xs = 1.15f;

    /* ---- compose ---- */
    uint32_t *out = calloc((size_t)W * S * H * S, 4);
    uint32_t *up = upscale_rgba(px, W, H, S);
    if (!out || !up) { free(out); free(up); return NULL; }
    memcpy(out + (size_t)MAIN_BOTTOM * S * W * S, up + (size_t)MAIN_BOTTOM * S * W * S, (size_t)(H - MAIN_BOTTOM) * S * W * S * 4);
    free(up);
    for (int y = PIXEL_FONT_TOP * S; y < H * S; y++) for (int x = 0; x < W * S; x++) out[(size_t)y * W * S + x] = px[(y / S) * W + x / S];
    if (icons4x) {        /* the chosen controller's icons: 4 cells of 18x24 at (0..72, 144..168) */
        for (int y = 0; y < 96; y++) for (int x = 0; x < 76 * S; x++) {
            uint32_t t = x < 288 ? icons4x[(y * 288 + x) * 4 + 3] : 0;
            out[(size_t)(144 * S + y) * W * S + x] = t | t << 8 | t << 16 | t << 24;
        }
    }
    for (int r = 0; r < 6; r++) {
        int base_y = row_base[r] * S;
        for (const char *c = rows[r]; *c; c++) {
            gl_box *b = &g[(unsigned char)*c]; if (!b->ok) continue;
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&f, *c, scale, scale, &x0, &y0, &x1, &y1);
            int iw = x1 - x0; if (iw < 1) continue;
            float sx = scale * xs; int maxw = (b->ox1 - b->ox0) * S + 2 * S;
            if (iw * xs > maxw) sx = scale * (float)maxw / (float)iw;   /* never wider than the original glyph */
            int gw, gh, gx, gy;
            unsigned char *bm = stbtt_GetCodepointBitmap(&f, sx, scale, *c, &gw, &gh, &gx, &gy);
            if (!bm) continue;
            float cx = (b->ox0 + b->ox1) / 2.0f * S;
            int x = (int)lroundf(cx - gw / 2.0f), y = base_y + gy;
            int ya = b->cy0 * S > y ? b->cy0 * S : y, yb = b->cy1 * S < y + gh ? b->cy1 * S : y + gh;
            int xa = b->cx0 * S; if (b->ox0 * S - S > xa) xa = b->ox0 * S - S; if (x > xa) xa = x;
            int xb = b->cx1 * S; if (b->ox1 * S + S < xb) xb = b->ox1 * S + S; if (x + gw < xb) xb = x + gw;
            for (int yy = ya; yy < yb; yy++) for (int xx = xa; xx < xb; xx++) {
                uint32_t t = bm[(yy - y) * gw + (xx - x)], *o = &out[(size_t)yy * W * S + xx];
                uint32_t a = *o >> 24; if (t > a) a = t;
                *o = a | a << 8 | a << 16 | a << 24;
            }
            stbtt_FreeBitmap(bm, NULL);
        }
    }
    return out;
}
