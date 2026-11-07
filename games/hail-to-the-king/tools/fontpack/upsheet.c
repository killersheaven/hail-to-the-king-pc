/* upscale a PNG 4x with the runtime's edge-directed filter; writes raw RGBA */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
uint32_t *upscale_rgba(const uint32_t *src, int w, int h, int factor);
int main(int argc, char **argv) {
    int w, h, n; unsigned char *img = stbi_load(argv[1], &w, &h, &n, 4);
    if (!img) return 1;
    uint32_t *up = upscale_rgba((uint32_t *)img, w, h, 4);
    FILE *f = fopen(argv[2], "wb"); fwrite(up, 4, (size_t)w * h * 16, f); fclose(f);
    return 0;
}
