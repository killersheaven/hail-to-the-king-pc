/* Ahead-of-time build: the translated program is linked into the executable. */
#include "code.h"
extern const fnent sh4_fn_table[];      extern const size_t sh4_fn_count;
extern const fnent sh4_resume_table[];  extern const size_t sh4_resume_count;
extern const uint32_t sh4_baked_lits[]; extern const size_t sh4_baked_count;
int sh4_code_load(sh4_code *code, const char *argv0) {
    (void)argv0;
    code->fn = sh4_fn_table; code->nfn = sh4_fn_count;
    code->resume = sh4_resume_table; code->nresume = sh4_resume_count;
    code->baked = sh4_baked_lits; code->nbaked = sh4_baked_count;
    return 1;
}
