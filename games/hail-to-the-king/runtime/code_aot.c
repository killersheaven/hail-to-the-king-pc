/* Ahead-of-time build: the translated program is linked into the executable. */
#include "code.h"
extern const fnent sh4_fn_table[];      extern const size_t sh4_fn_count;
extern const fnent sh4_resume_table[];  extern const size_t sh4_resume_count;
extern const uint32_t sh4_baked_lits[]; extern const size_t sh4_baked_count;
volatile int sh4_code_progress = 1000;
int sh4_code_load(sh4_code *code, const char *argv0, const uint8_t *img, size_t len) {
    (void)argv0; (void)img; (void)len;
    code->fn = sh4_fn_table; code->nfn = sh4_fn_count;
    code->resume = sh4_resume_table; code->nresume = sh4_resume_count;
    code->baked = sh4_baked_lits; code->nbaked = sh4_baked_count;
    return 1;
}
