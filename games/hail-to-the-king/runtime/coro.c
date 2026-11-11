/* Host coroutines for the game's threads.
 *
 * The game (via Sega's and CRI's libraries) switches threads inside interrupt
 * handlers: the handler saves the interrupted thread's registers, loads
 * another thread's, and executes RTE to a different PC. Recompiled code keeps
 * part of each thread's state on the host stack, so every guest thread gets
 * its own host stack, and a context switch in the guest is a stack switch on
 * the host.
 *
 * Symmetric switching: Windows fibers, a small assembly switch on ARM64 (Android),
 * otherwise POSIX ucontext.
 */
#include "coro.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
struct coro { void *fiber; void (*fn)(void *); void *arg; };
static coro_t main_coro;
static coro_t *current;
static void WINAPI fiber_entry(void *p) { coro_t *c = p; c->fn(c->arg); fprintf(stderr, "[coro] thread function returned\n"); exit(3); }
coro_t *coro_main(void) {
    if (!main_coro.fiber) { main_coro.fiber = ConvertThreadToFiber(NULL); current = &main_coro; }
    return &main_coro;
}
coro_t *coro_new(void (*fn)(void *), void *arg, size_t stack) {
    coro_main();
    coro_t *c = calloc(1, sizeof *c);
    c->fn = fn; c->arg = arg;
    c->fiber = CreateFiber(stack, fiber_entry, c);
    return c->fiber ? c : NULL;
}
void coro_switch(coro_t *to) { coro_main(); current = to; SwitchToFiber(to->fiber); }
void coro_free(coro_t *c) { if (c && c != &main_coro && c != current) { DeleteFiber(c->fiber); free(c); } }
#elif defined(__aarch64__)
/* ARM64 (Android's libc has no ucontext functions): a small stack switch that saves the
 * registers a function call must preserve (x19-x30, sp, d8-d15). */
#include <stdint.h>
struct coro { uint64_t regs[22]; void (*fn)(void *); void *arg; void *stack; };
static coro_t main_coro;
static coro_t *current;
void coro_swap_a64(uint64_t *save, const uint64_t *load);
__asm__(
    ".text\n.global coro_swap_a64\n.type coro_swap_a64, %function\n.p2align 2\n"
    "coro_swap_a64:\n"
    "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n"
    "  mov x9, sp\n  str x9, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
    "  ldp x19, x20, [x1, #0]\n  ldp x21, x22, [x1, #16]\n  ldp x23, x24, [x1, #32]\n"
    "  ldp x25, x26, [x1, #48]\n  ldp x27, x28, [x1, #64]\n  ldp x29, x30, [x1, #80]\n"
    "  ldr x9, [x1, #96]\n  mov sp, x9\n"
    "  ldp d8, d9, [x1, #104]\n  ldp d10, d11, [x1, #120]\n  ldp d12, d13, [x1, #136]\n  ldp d14, d15, [x1, #152]\n"
    "  ret\n"
    ".size coro_swap_a64, .-coro_swap_a64\n");
static void entry(void) { coro_t *c = current; c->fn(c->arg); fprintf(stderr, "[coro] thread function returned\n"); exit(3); }
coro_t *coro_main(void) { if (!current) current = &main_coro; return &main_coro; }
coro_t *coro_new(void (*fn)(void *), void *arg, size_t stack) {
    coro_main();
    coro_t *c = calloc(1, sizeof *c);
    c->fn = fn; c->arg = arg; c->stack = malloc(stack);
    if (!c->stack) { free(c); return NULL; }
    uintptr_t top = ((uintptr_t)c->stack + stack) & ~(uintptr_t)15;
    c->regs[11] = (uint64_t)(uintptr_t)entry;       /* x30: first switch "returns" into entry */
    c->regs[10] = 0;                                /* x29: end of the frame chain */
    c->regs[12] = top;                              /* sp */
    return c;
}
void coro_switch(coro_t *to) {
    coro_main();
    coro_t *from = current; if (from == to) return;
    current = to;
    coro_swap_a64(from->regs, to->regs);
}
void coro_free(coro_t *c) { if (c && c != &main_coro && c != current) { free(c->stack); free(c); } }
#else
#include <ucontext.h>
struct coro { ucontext_t ctx; void (*fn)(void *); void *arg; void *stack; };
static coro_t main_coro;
static coro_t *current;
static int main_init;
static coro_t *starting;
static void entry(void) { coro_t *c = starting; c->fn(c->arg); fprintf(stderr, "[coro] thread function returned\n"); exit(3); }
coro_t *coro_main(void) { if (!main_init) { main_init = 1; current = &main_coro; } return &main_coro; }
coro_t *coro_new(void (*fn)(void *), void *arg, size_t stack) {
    coro_main();
    coro_t *c = calloc(1, sizeof *c);
    c->fn = fn; c->arg = arg; c->stack = malloc(stack);
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp = c->stack; c->ctx.uc_stack.ss_size = stack; c->ctx.uc_link = NULL;
    makecontext(&c->ctx, entry, 0);
    return c;
}
void coro_switch(coro_t *to) {
    coro_t *from = current; current = to;
    starting = to;
    swapcontext(&from->ctx, &to->ctx);
}
void coro_free(coro_t *c) { if (c && c != &main_coro && c != current) { free(c->stack); free(c); } }
#endif
coro_t *coro_current(void) { coro_main(); return current; }
