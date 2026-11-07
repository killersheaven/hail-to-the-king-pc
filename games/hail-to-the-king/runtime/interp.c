/* Fallback SH-4 interpreter for code the static analysis never found.
 *
 * sh4_dispatch() lands here for an indirect target that is neither a
 * recompiled function nor a known resume point. The interpreter follows the
 * same calling model as recompiled code: it returns to its C caller on RTS,
 * calls (JSR/BSR/BSRF) go through sh4_dispatch (so known functions run
 * natively), and jumps/branches are followed directly. Non-branch
 * instructions come from interp_ops.c, generated from the recompiler's own
 * instruction templates. Every target that needed interpreting is logged to
 * missing_entries.txt so the next build can recompile it.
 */
#include "sh4ctx.h"
#include <stdio.h>

int sh4_interp_exec(sh4ctx *c, uint16_t op);     /* generated */
int sh4_known_target(uint32_t t);
void hle_halt(const char *why, uint32_t a);
uint64_t interp_insns, interp_entries;

/* one non-branch instruction at pc (also used for delay slots) */
static void exec1(sh4ctx *c, uint32_t pc) {
    uint16_t op = R16(pc);
    interp_insns++;
    if ((op & 0xF000) == 0x9000) { c->r[(op >> 8) & 15] = (uint32_t)(int16_t)R16(pc + 4 + (op & 0xFF) * 2); return; }
    if ((op & 0xF000) == 0xD000) { c->r[(op >> 8) & 15] = R32(((pc + 4) & ~3u) + (op & 0xFF) * 4); return; }
    if ((op & 0xFF00) == 0xC700) { c->r[0] = ((pc + 4) & ~3u) + (op & 0xFF) * 4; return; }
    if (sh4_interp_exec(c, op) < 0) {
        fprintf(stderr, "[interp] illegal or unsupported instruction %04X at %08X\n", op, pc);
        hle_halt("illegal instruction in interpreted code", pc);
    }
}

void sh4_interp(sh4ctx *c, uint32_t pc) {
    interp_entries++;
    for (;;) {
        uint16_t op = R16(pc);
        int32_t d8 = (int8_t)(op & 0xFF), d12 = (op & 0x800) ? (int32_t)(op & 0xFFF) - 0x1000 : (int32_t)(op & 0xFFF);
        int n = (op >> 8) & 15;
        uint32_t tgt;
        switch (op & 0xFF00) {
        case 0x8900: case 0x8B00: {                               /* bt / bf */
            int take = (op & 0x0200) ? !c->t : c->t;
            interp_insns++;
            if (take) { tgt = pc + 4 + d8 * 2; if (tgt <= pc) SH4_POLL(c); pc = tgt; } else pc += 2;
            continue;
        }
        case 0x8D00: case 0x8F00: {                               /* bt/s / bf/s */
            int take = (op & 0x0200) ? !c->t : c->t;
            interp_insns++; exec1(c, pc + 2);
            if (take) { tgt = pc + 4 + d8 * 2; if (tgt <= pc) SH4_POLL(c); pc = tgt; } else pc += 4;
            continue;
        }
        }
        switch (op & 0xF000) {
        case 0xA000:                                              /* bra */
            tgt = pc + 4 + d12 * 2; interp_insns++; exec1(c, pc + 2);
            if (tgt <= pc) SH4_POLL(c);
            pc = tgt; continue;
        case 0xB000:                                              /* bsr */
            tgt = pc + 4 + d12 * 2; interp_insns++; c->pr = pc + 4; exec1(c, pc + 2);
            sh4_dispatch(c, tgt); pc += 4; continue;
        }
        switch (op & 0xF0FF) {
        case 0x0003:                                              /* bsrf Rn */
            tgt = pc + 4 + c->r[n]; interp_insns++; c->pr = pc + 4; exec1(c, pc + 2);
            sh4_dispatch(c, tgt); pc += 4; continue;
        case 0x0023:                                              /* braf Rn */
            tgt = pc + 4 + c->r[n]; interp_insns++; exec1(c, pc + 2);
            if (tgt <= pc) SH4_POLL(c);
            pc = tgt; continue;
        case 0x400B:                                              /* jsr @Rn */
            tgt = c->r[n]; interp_insns++; c->pr = pc + 4; exec1(c, pc + 2);
            sh4_dispatch(c, tgt); pc += 4; continue;
        case 0x402B:                                              /* jmp @Rn: tail jump */
            tgt = c->r[n]; interp_insns++; exec1(c, pc + 2);
            SH4_POLL(c);
            sh4_dispatch(c, tgt); return;                         /* tail call: BIOS, recompiled or interpreted */
        }
        if (op == 0x000B) { interp_insns++; exec1(c, pc + 2); return; }                 /* rts */
        if (op == 0x002B) { interp_insns++; exec1(c, pc + 2); sh4_rte(c, pc); return; } /* rte */
        if ((op & 0xFF00) == 0xC300) { interp_insns++; sh4_trapa(c, op & 0xFF, pc); pc += 2; continue; }
        if (op == 0x001B) { interp_insns++; sh4_sleep(c, pc); pc += 2; continue; }      /* sleep */
        exec1(c, pc);
        pc += 2;
    }
}
