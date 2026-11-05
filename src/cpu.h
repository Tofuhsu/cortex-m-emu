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

#define CPU_EXCEPTION_SYSTICK 15u
#define CPU_EXCEPTION_COUNT 256u
#define CPU_EXTERNAL_IRQ_COUNT 240u

typedef bool (*cpu_irq_enabled_fn)(void *ctx, unsigned irq);

typedef void (*cpu_step_hook_fn)(void *ctx);
typedef void (*cpu_trace_fn)(void *ctx, uint32_t pc, uint16_t first_halfword);

typedef struct {
    uint32_t r[16];      /* r0-r12, SP (r13), LR (r14), PC (r15) */
    bool n, z, c, v;     /* APSR condition flags */
    bool primask;        /* Configurable-priority exception mask; reset = 0 */
    uint16_t ipsr;       /* 0 = Thread mode; otherwise active exception number */
    uint32_t pending_exceptions[8]; /* coalesced exception bits, including IRQ 38+16 */
    cpu_irq_enabled_fn irq_enabled;
    void *irq_enabled_ctx;
    uint64_t steps;      /* instructions executed */
    bool halted;
    char halt_reason[128];
    cpu_step_hook_fn step_hook;
    void *step_hook_ctx;
    cpu_trace_fn trace_hook;
    void *trace_hook_ctx;
} cpu_t;

/* Reset core state, pending bits and hooks; load SP/PC from the vector table.
 * Install hooks AFTER cpu_reset(). Hook contexts are borrowed.
 */
void cpu_reset(cpu_t *cpu, bus_t *bus);

/* Called once after each steps increment, including a final halted attempt.
 * No call for a failed pre-fetch dispatch/fetch, or a call on a halted CPU.
 */
void cpu_set_step_hook(cpu_t *cpu, cpu_step_hook_fn hook, void *ctx);

/* Observe the instruction after pending dispatch and successful full fetch,
 * before PC advance/execution. The hook should not mutate the CPU or bus.
 */
void cpu_set_trace_hook(cpu_t *cpu, cpu_trace_fn trace, void *ctx);

/* SysTick and external exceptions 16..255 coalesce. External IRQs require an
 * installed enable query. Acknowledged only after successful Thread-mode entry.
 */
void cpu_set_irq_gate(cpu_t *cpu, cpu_irq_enabled_fn query, void *ctx);
bool cpu_clear_pending_exception(cpu_t *cpu, unsigned number);
uint32_t cpu_get_xpsr(const cpu_t *cpu);
bool cpu_pend_exception(cpu_t *cpu, unsigned number);
bool cpu_exception_pending(const cpu_t *cpu, unsigned number);

/* Execute one instruction. Returns 0, or -1 if the CPU is (or just became) halted. */
int cpu_step(cpu_t *cpu, bus_t *bus);

/* Stop the CPU with a reason (printf-style). Used for BKPT, undefined instructions, bus faults. */
void cpu_halt(cpu_t *cpu, const char *fmt, ...);

#endif
