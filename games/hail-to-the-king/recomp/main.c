/* Command-line front end of the C recompiler.
 *   recomp <1ST_READ.BIN> <out dir> [game dir]   same output as tools/recomp.py
 *   recomp --interp-only <out dir>               just interp_ops.c
 * game dir (default ..) holds notes/ and tools/overlay_8C00F400.json. */
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); fclose(f); b[r] = 0;
    if (len) *len = r;
    return b;
}
static int spit(const char *dir, const char *name, const char *text, size_t len) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); return 0; }
    fwrite(text, 1, len, f); fclose(f); return 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--interp-only")) {
        size_t l; int n; char *t = rc_gen_interp(&l, &n);
        if (!spit(argv[2], "interp_ops.c", t, l)) return 1;
        printf("interpreter cases: %d\n", n); return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: recomp <1ST_READ.BIN> <out dir> [game dir]\n       recomp --interp-only <out dir>\n"); return 2; }
    const char *gd = argc > 3 ? argv[3] : "..";
    char p[1024];
    rc_input in = {0};
    size_t il; char *im = slurp(argv[1], &il);
    if (!im) { perror(argv[1]); return 1; }
    in.img = (const uint8_t *)im; in.img_len = il;
    snprintf(p, sizeof p, "%s/tools/overlay_8C00F400.json", gd);
    char *rj = slurp(p, NULL);
    if (rj) { uint32_t b; size_t l; uint8_t *o = rc_build_overlay(rj, in.img, il, &b, &l); if (o) { in.ovl = o; in.ovl_len = l; in.ovl_base = b; } }
    else fprintf(stderr, "note: no overlay recipe at %s\n", p);
    uint32_t *v;
    snprintf(p, sizeof p, "%s/notes/extra_entries.txt", gd); char *t = slurp(p, NULL);
    if (t) { in.nextra = rc_parse_hex_list(t, &v); in.extra = v; }
    snprintf(p, sizeof p, "%s/notes/live_literals.txt", gd); t = slurp(p, NULL);
    if (t) { in.nlive = rc_parse_hex_list(t, &v); in.live = v; }
    snprintf(p, sizeof p, "%s/notes/names.json", gd); t = slurp(p, NULL);
    if (t) { rc_name *nm; in.nnames = rc_parse_names(t, &nm); in.names = nm; }
    rc_output out;
    if (!rc_translate(&in, &out)) { fprintf(stderr, "translation failed\n"); return 1; }
    for (int i = 0; i < out.nfiles; i++) if (!spit(argv[2], out.files[i].name, out.files[i].text, out.files[i].len)) return 1;
    size_t l; int n; char *ip = rc_gen_interp(&l, &n);
    spit(argv[2], "interp_ops.c", ip, l);
    printf("interpreter cases: %d\nbaked literals: %d, live literals: %d\nfunctions: %d  unsupported: %d\n", n, out.nbaked, in.nlive, out.nfuncs, out.unsupported);
    return out.unsupported ? 1 : 0;
}
