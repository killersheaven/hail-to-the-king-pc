#pragma once
#include <stddef.h>
typedef struct coro coro_t;
coro_t *coro_main(void);                                   /* the host's main stack */
coro_t *coro_new(void (*fn)(void *), void *arg, size_t stack);
void    coro_switch(coro_t *to);                           /* symmetric switch */
coro_t *coro_current(void);
void    coro_free(coro_t *c);                              /* not the running one */
