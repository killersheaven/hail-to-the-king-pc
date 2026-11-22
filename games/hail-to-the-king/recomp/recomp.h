/* SH-4 -> C static recompiler (C version of tools/analyze.py + tools/recomp.py).
 *
 * Used by the one-click build at launch, and by the recomp command-line tool
 * (recomp/main.c), whose output must match recomp.py file for file. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t addr; const char *name; } rc_name;

typedef struct {
    const uint8_t *img; size_t img_len;                     /* 1ST_READ.BIN, loaded at 0x8C010000 */
    const uint8_t *ovl; size_t ovl_len; uint32_t ovl_base;  /* code the game copies into RAM (optional) */
    const uint32_t *extra; int nextra;                      /* notes/extra_entries.txt */
    const uint32_t *live; int nlive;                        /* notes/live_literals.txt */
    const rc_name *names; int nnames;                       /* notes/names.json (comments only) */
} rc_input;

typedef struct { char name[32]; char *text; size_t len; } rc_file;
typedef struct {
    rc_file *files; int nfiles;
    int nfuncs, unsupported, nbaked;
} rc_output;

/* Translates the program: funcs.h, funcs_NNN.c, fntable.c, resume.c, litmap.c
 * (no interp_ops.c). Returns 0 on failure. */
int rc_translate(const rc_input *in, rc_output *out);
/* interp_ops.c: one case per non-branch opcode (holds no game code) */
char *rc_gen_interp(size_t *len, int *ncases);
void rc_free(rc_output *out);

/* text helpers for the notes files */
int rc_parse_hex_list(const char *text, uint32_t **out);          /* one hex address per line, '#' comments */
int rc_parse_names(const char *json, rc_name **out);              /* {"8c010172": "mwMng_1", ...} */
/* overlay recipe (tools/overlay_8C00F400.json) + the program -> overlay bytes */
uint8_t *rc_build_overlay(const char *recipe_json, const uint8_t *img, size_t img_len, uint32_t *base, size_t *len);
