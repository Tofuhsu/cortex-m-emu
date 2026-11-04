/*
 * ARMv7-M (Cortex-M4) CPU core: registers, flags and one-instruction stepping.
 * Reference: ARMv7-M Architecture Reference Manual (Arm DDI 0403E), chapters A5 and A7.
 */
#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

#define SP 13
#define LR 14
#define PC 15

typedef struct {
    uint32_t r[16];      /* r0-r12, SP (r13), LR (r14), PC (r15) */
    bool n, z, c, v;     /* APSR condition flags */
    uint16_t ipsr;       /* 0 = Thread mode; otherwise active exception number */
    uint64_t steps;      /* instructions executed */
    bool halted;
    char halt_reason[128];
} cpu_t;

/* Load SP and PC from the vector table, as the hardware does on reset. */
void cpu_reset(cpu_t *cpu, bus_t *bus);

/* Execute one instruction. Returns 0, or -1 if the CPU is (or just became) halted. */
int cpu_step(cpu_t *cpu, bus_t *bus);

/* Stop the CPU with a reason (printf-style). Used for BKPT, undefined instructions, bus faults. */
void cpu_halt(cpu_t *cpu, const char *fmt, ...);

#endif
