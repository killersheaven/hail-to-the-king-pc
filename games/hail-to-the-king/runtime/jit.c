/* One-click build: compile the translated game code at launch with the built-in TinyCC.
 *
 * The executable ships no game code. The C that the recompiler made from the
 * player's own 1ST_READ.BIN sits in a cache folder ("gen" next to the program,
 * or DC_GEN_DIR); it is compiled into memory here, in about a second, and its
 * function tables are handed to the dispatcher. Generated code reaches the
 * runtime only through the symbols listed in add_runtime_symbols().
 */
#include "code.h"
#include "libtcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

extern const char sh4ctx_h_text[];          /* runtime/sh4ctx.h, embedded at build time */
extern uint8_t *dc_ram;
extern int sh4_poll_ctr;
extern uint32_t sh4_resume_pc;
void hle_ctx_restore(sh4ctx *c);

static void on_error(void *opaque, const char *msg) { (void)opaque; fprintf(stderr, "[jit] %s\n", msg); }

static void add_runtime_symbols(TCCState *s) {
#define SYM(x) tcc_add_symbol(s, #x, (const void *)&x)
    SYM(dc_ram); SYM(sh4_poll_ctr); SYM(sh4_resume_pc);
    SYM(mmio_read8); SYM(mmio_read16); SYM(mmio_read32);
    SYM(mmio_write8); SYM(mmio_write16); SYM(mmio_write32);
    SYM(sh4_sq_flush); SYM(sh4_dispatch); SYM(sh4_trapa); SYM(sh4_sleep); SYM(sh4_rte);
    SYM(sh4_unreachable); SYM(sh4_ldtlb); SYM(sh4_set_sr); SYM(sh4_poll_slow); SYM(sh4_code_write);
    SYM(hle_ctx_restore);
    SYM(memcpy); SYM(memset); SYM(memmove);
    SYM(sin); SYM(cos); SYM(sqrt); SYM(sqrtf);
#undef SYM
}

static void default_dir(char *out, size_t n, const char *argv0) {
    const char *e = getenv("DC_GEN_DIR");
    if (e) { snprintf(out, n, "%s", e); return; }
    snprintf(out, n, "%s", argv0 ? argv0 : "");
    char *sl = strrchr(out, '/'), *bs = strrchr(out, '\\');
    if (bs > sl) sl = bs;
    if (sl) snprintf(sl + 1, n - (size_t)(sl + 1 - out), "gen"); else snprintf(out, n, "gen");
}

/* the .c files of the cache folder, sorted so builds are reproducible */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int list_c_files(const char *dir, char ***out) {
    int n = 0, cap = 64; char **v = malloc(sizeof *v * cap);
#ifdef _WIN32
    char pat[1200]; snprintf(pat, sizeof pat, "%s\\*.c", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n == cap) v = realloc(v, sizeof *v * (cap *= 2)); v[n++] = _strdup(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir); struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 2 && !strcmp(e->d_name + l - 2, ".c")) { if (n == cap) v = realloc(v, sizeof *v * (cap *= 2)); v[n++] = strdup(e->d_name); }
    }
    if (d) closedir(d);
#endif
    qsort(v, (size_t)n, sizeof *v, cmp_str);
    *out = v; return n;
}

int sh4_code_load(sh4_code *code, const char *argv0) {
    char dir[1024], p[1200];
    default_dir(dir, sizeof dir, argv0);
    /* the header the generated files include: always the copy built into this program */
    snprintf(p, sizeof p, "%s/sh4ctx.h", dir);
    FILE *h = fopen(p, "wb");
    if (!h) { fprintf(stderr, "[jit] cannot write %s (is the translated program in %s?)\n", p, dir); return 0; }
    fputs(sh4ctx_h_text, h); fclose(h);

    char **files; int nf = list_c_files(dir, &files);
    if (nf == 0) { fprintf(stderr, "[jit] no translated program in %s\n", dir); return 0; }
    TCCState *s = tcc_new();
    if (!s) return 0;
    tcc_set_error_func(s, NULL, on_error);
    tcc_set_options(s, "-nostdlib -nostdinc -w");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_add_include_path(s, dir);
    tcc_define_symbol(s, "DC_JIT_GUEST", "1");
    add_runtime_symbols(s);
    for (int i = 0; i < nf; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        if (tcc_add_file(s, p) < 0) { fprintf(stderr, "[jit] compile failed: %s\n", p); tcc_delete(s); return 0; }
        free(files[i]);
    }
    free(files);
    if (tcc_relocate(s) < 0) { fprintf(stderr, "[jit] link failed\n"); tcc_delete(s); return 0; }
#define GET(name) tcc_get_symbol(s, name)
    const size_t *nfn = GET("sh4_fn_count"), *nres = GET("sh4_resume_count"), *nbk = GET("sh4_baked_count");
    code->fn = GET("sh4_fn_table"); code->resume = GET("sh4_resume_table"); code->baked = GET("sh4_baked_lits");
#undef GET
    if (!nfn || !nres || !nbk || !code->fn || !code->resume || !code->baked) { fprintf(stderr, "[jit] tables missing in %s\n", dir); return 0; }
    code->nfn = *nfn; code->nresume = *nres; code->nbaked = *nbk;
    fprintf(stderr, "[jit] compiled %d files from %s with the built-in TinyCC\n", nf, dir);
    return 1;                                       /* the state stays alive: it owns the code */
}
