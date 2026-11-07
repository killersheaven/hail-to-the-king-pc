/* Host coroutines for the game's threads.
 *
 * The game (via Sega's and CRI's libraries) switches threads inside interrupt
 * handlers: the handler saves the interrupted thread's registers, loads
 * another thread's, and executes RTE to a different PC. Recompiled code keeps
 * part of each thread's state on the host stack, so every guest thread gets
 * its own host stack, and a context switch in the guest is a stack switch on
 * the host.
 *
 * Symmetric switching: Windows fibers, otherwise POSIX ucontext.
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
