#pragma once
#include <stdint.h>
typedef struct {
    uint32_t r[16];             /* r15 unused: see pc */
    uint32_t pc;
    uint32_t cpsr;
    uint32_t spsr[6];           /* per bank: 0 usr/sys (none), 1 fiq, 2 irq, 3 svc, 4 abt, 5 und */
    uint32_t b13[6], b14[6];
    uint32_t fiq8[5], usr8[5];  /* r8-r12 */
    int fiq_line, branched;
} arm7_t;
extern arm7_t arm;
extern uint64_t arm7_insns;
void arm7_reset(void);
void arm7_run(int n);
/* memory, provided by aica.c (ARM address space) */
uint32_t arm7_read32(uint32_t a);
uint16_t arm7_read16(uint32_t a);
uint8_t  arm7_read8(uint32_t a);
void arm7_write32(uint32_t a, uint32_t v);
void arm7_write16(uint32_t a, uint16_t v);
void arm7_write8(uint32_t a, uint8_t v);
