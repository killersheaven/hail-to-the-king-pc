/* Where the translated game code comes from.
 *  - ahead-of-time build (build.bat / build.sh): recomp.py's C is compiled with the
 *    rest of the program and its tables are linked in (code_aot.c).
 *  - one-click build (DC_JIT): the program ships no game code. At launch the C made
 *    from the player's own disc is compiled into memory by the built-in TinyCC (jit.c). */
#pragma once
#include "sh4ctx.h"
#include <stddef.h>
typedef struct { uint32_t a; sh4fn f; } fnent;
typedef struct {
    const fnent *fn; size_t nfn;            /* function entry points, sorted */
    const fnent *resume; size_t nresume;    /* return addresses -> containing function */
    const uint32_t *baked; size_t nbaked;   /* literals baked in as constants */
} sh4_code;
int sh4_code_load(sh4_code *code, const char *argv0);
