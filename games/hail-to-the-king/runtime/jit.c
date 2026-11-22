/* One-click build: translate and compile the game code at launch.
 *
 * The executable ships no game code. At every launch the recompiler
 * (recomp/recomp.c) translates the player's own 1ST_READ.BIN, read from their
 * disc, to C in memory, and the built-in TinyCC compiles that C into memory;
 * together about two seconds. Nothing is written to disk.
 * DC_GEN_DIR=<folder> compiles the .c files of a folder instead (recomp.py output).
 * DC_GEN_DUMP=<folder> writes the C that was translated (for comparing with recomp.py).
 * Generated code reaches the runtime only through add_runtime_symbols().
 */
#include "code.h"
#include "libtcc.h"
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* embedded at build time (CMake): the header the generated code is compiled against, and the notes */
extern const char sh4ctx_h_text[], extra_entries_text[], live_literals_text[], names_json_text[], overlay_json_text[];
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

/* the interpreter's instruction cases are compiled with the game code */
static int (*interp_exec)(sh4ctx *c, uint16_t op);
int sh4_interp_exec(sh4ctx *c, uint16_t op) { return interp_exec(c, op); }

static TCCState *new_state(void) {
    TCCState *s = tcc_new();
    if (!s) return NULL;
    tcc_set_error_func(s, NULL, on_error);
    tcc_set_options(s, "-nostdlib -nostdinc -w");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_define_symbol(s, "DC_JIT_GUEST", "1");
    add_runtime_symbols(s);
    return s;
}
static int finish(TCCState *s, sh4_code *code) {
    if (tcc_relocate(s) < 0) { fprintf(stderr, "[jit] link failed\n"); return 0; }
#define GET(name) tcc_get_symbol(s, name)
    const size_t *nfn = GET("sh4_fn_count"), *nres = GET("sh4_resume_count"), *nbk = GET("sh4_baked_count");
    code->fn = GET("sh4_fn_table"); code->resume = GET("sh4_resume_table"); code->baked = GET("sh4_baked_lits");
    interp_exec = (int (*)(sh4ctx *, uint16_t))GET("sh4_interp_exec");
#undef GET
    if (!nfn || !nres || !nbk || !code->fn || !code->resume || !code->baked || !interp_exec) { fprintf(stderr, "[jit] tables missing\n"); return 0; }
    code->nfn = *nfn; code->nresume = *nres; code->nbaked = *nbk;
    return 1;                                       /* the state stays alive: it owns the code */
}

/* development: compile recomp.py's output from a folder */
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
static int load_dir(sh4_code *code, const char *dir) {
    char p[1200];
    snprintf(p, sizeof p, "%s/sh4ctx.h", dir);      /* always the header built into this program */
    FILE *h = fopen(p, "wb");
    if (!h) { fprintf(stderr, "[jit] cannot write %s\n", p); return 0; }
    fputs(sh4ctx_h_text, h); fclose(h);
    char **files; int nf = list_c_files(dir, &files);
    if (nf == 0) { fprintf(stderr, "[jit] no .c files in %s\n", dir); return 0; }
    TCCState *s = new_state(); if (!s) return 0;
    tcc_add_include_path(s, dir);
    for (int i = 0; i < nf; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        if (tcc_add_file(s, p) < 0) { fprintf(stderr, "[jit] compile failed: %s\n", p); return 0; }
        free(files[i]);
    }
    free(files);
    if (!finish(s, code)) return 0;
    fprintf(stderr, "[jit] compiled %d files from %s\n", nf, dir);
    return 1;
}

static double now_s(void) {
#ifdef _WIN32
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
static void dump(const char *dir, const char *name, const char *text, size_t len) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb"); if (f) { fwrite(text, 1, len, f); fclose(f); }
}

/* compile one generated file: the embedded header (with the function declarations) replaces its #include */
static int compile_text(TCCState *s, const char *prelude, size_t plen, const char *text) {
    static const char inc[] = "#include \"sh4ctx.h\"";
    if (!strncmp(text, inc, sizeof inc - 1)) text += sizeof inc - 1;
    size_t tl = strlen(text);
    char *src = malloc(plen + tl + 2);
    memcpy(src, prelude, plen); src[plen] = '\n'; memcpy(src + plen + 1, text, tl + 1);
    int r = tcc_compile_string(s, src);
    free(src);
    return r >= 0;
}

int sh4_code_load(sh4_code *code, const char *argv0, const uint8_t *img, size_t len) {
    (void)argv0;
    if (getenv("DC_GEN_DIR")) return load_dir(code, getenv("DC_GEN_DIR"));
    double t0 = now_s();
    rc_input in = {0};
    in.img = img; in.img_len = len;
    uint32_t b; size_t ol; uint8_t *o = rc_build_overlay(overlay_json_text, img, len, &b, &ol);
    if (o) { in.ovl = o; in.ovl_len = ol; in.ovl_base = b; }
    uint32_t *v;
    in.nextra = rc_parse_hex_list(extra_entries_text, &v); in.extra = v;
    in.nlive = rc_parse_hex_list(live_literals_text, &v); in.live = v;
    rc_name *nm; in.nnames = rc_parse_names(names_json_text, &nm); in.names = nm;
    rc_output out;
    if (!rc_translate(&in, &out)) { fprintf(stderr, "[jit] translation failed\n"); return 0; }
    size_t il; int ncases; char *interp = rc_gen_interp(&il, &ncases);
    double t1 = now_s();
    const char *dd = getenv("DC_GEN_DUMP");
    if (dd) { for (int i = 0; i < out.nfiles; i++) dump(dd, out.files[i].name, out.files[i].text, out.files[i].len); dump(dd, "interp_ops.c", interp, il); }
    /* prelude: sh4ctx.h with funcs.h pasted in */
    const char *fh = NULL; size_t fhl = 0;
    for (int i = 0; i < out.nfiles; i++) if (!strcmp(out.files[i].name, "funcs.h")) { fh = out.files[i].text; fhl = out.files[i].len; }
    static const char finc[] = "#include \"funcs.h\"";
    const char *at = strstr(sh4ctx_h_text, finc);
    if (!fh || !at) { fprintf(stderr, "[jit] header pieces missing\n"); return 0; }
    size_t hl = strlen(sh4ctx_h_text), pre = (size_t)(at - sh4ctx_h_text), post = hl - pre - (sizeof finc - 1);
    size_t plen = pre + fhl + post; char *prelude = malloc(plen + 1);
    memcpy(prelude, sh4ctx_h_text, pre); memcpy(prelude + pre, fh, fhl); memcpy(prelude + pre + fhl, at + sizeof finc - 1, post); prelude[plen] = 0;
    TCCState *s = new_state(); if (!s) return 0;
    int ok = 1;
    for (int i = 0; ok && i < out.nfiles; i++) {
        size_t l = strlen(out.files[i].name);
        if (l > 2 && !strcmp(out.files[i].name + l - 2, ".c")) ok = compile_text(s, prelude, plen, out.files[i].text);
    }
    if (ok) ok = compile_text(s, prelude, plen, interp);
#if defined(_WIN32) && defined(__x86_64__)
    /* TinyCC calls __chkstk for stack frames over 4 KB on Windows (its lib/chkstk.S) */
    if (ok) ok = tcc_compile_string(s, "__asm__(\".globl __chkstk\\n__chkstk:\\n xchg (%rsp),%rbp\\n push %rbp\\n lea 8(%rsp),%rbp\\n"
        " push %rcx\\n mov %rbp,%rcx\\n movslq %eax,%rax\\nP0:\\n sub $4096,%rcx\\n test %rax,(%rcx)\\n sub $4096,%rax\\n cmp $4096,%rax\\n"
        " jge P0\\n sub %rax,%rcx\\n test %rax,(%rcx)\\n mov %rsp,%rax\\n mov %rcx,%rsp\\n mov (%rax),%rcx\\n jmp *8(%rax)\\n\");") >= 0;
#endif
    int nf = out.nfiles, nfun = out.nfuncs;
    rc_free(&out); free(interp); free(prelude); free(o); free((void *)in.extra); free((void *)in.live);
    for (int i = 0; i < in.nnames; i++) free((void *)nm[i].name);
    free(nm);
    if (!ok) { fprintf(stderr, "[jit] compile failed\n"); return 0; }
    if (!finish(s, code)) return 0;
    fprintf(stderr, "[jit] translated %d functions in %.2f s, compiled %d files with the built-in TinyCC in %.2f s\n", nfun, t1 - t0, nf, now_s() - t1);
    return 1;
}
