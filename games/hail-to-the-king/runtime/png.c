/* Minimal PNG writer (uncompressed deflate blocks) for screenshots; no dependencies. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_table[i] = x; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(0, (const uint8_t *)type, 4); c = crc(c, d, n);
    uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
/* rgba: w*h*4 bytes; bottom_up = rows stored bottom first (GL readback) */
int png_write(const char *path, const uint8_t *rgba, int w, int h, int bottom_up) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        int sy = bottom_up ? h - 1 - y : y;
        raw[y * (1 + w * 4)] = 0;
        memcpy(raw + y * (1 + w * 4) + 1, rgba + (size_t)sy * w * 4, (size_t)w * 4);
    }
    size_t nblk = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(z_n), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *p++ = off + n >= raw_n; *p++ = n & 0xFF; *p++ = n >> 8; *p++ = ~n & 0xFF; *p++ = (~n >> 8) & 0xFF;
        memcpy(p, raw + off, n); p += n;
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
    }
    be32(p, b << 16 | a); p += 4;
    chunk(f, "IDAT", z, (uint32_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z); fclose(f);
    return 1;
}
