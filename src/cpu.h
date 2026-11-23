/*
 * ARMv7-M (Cortex-M4) CPU core: registers, flags and one-instruction stepping.
 * Reference: ARMv7-M Architecture Reference Manual (Arm DDI 0403E), chapters
 * A5, A7 (Thumb instructions) and B1.5 (exception model).
 */
#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

#define SP 13
#define LR 14
#define PC 15

#define CPU_EXCEPTION_NMI       2u
#define CPU_EXCEPTION_HARDFAULT 3u
#define CPU_EXCEPTION_SVCALL    11u
#define CPU_EXCEPTION_PENDSV    14u
#define CPU_EXCEPTION_SYSTICK   15u
#define CPU_EXCEPTION_COUNT     256u
#define CPU_EXTERNAL_IRQ_COUNT  240u

typedef bool (*cpu_irq_enabled_fn)(void *ctx, unsigned irq);
/* Raw 8-bit IPR byte of an external IRQ (unimplemented low bits read as 0). */
typedef uint8_t (*cpu_irq_priority_fn)(void *ctx, unsigned irq);

typedef void (*cpu_step_hook_fn)(void *ctx);
typedef void (*cpu_trace_fn)(void *ctx, uint32_t pc, uint16_t first_halfword);

typedef struct {
    uint32_t r[16];      /* r0-r12, SP (r13, the CURRENTLY selected stack), LR, PC */
    bool n, z, c, v, q;  /* APSR flags; GE[3:0] is not modeled (no SIMD) */
    bool primask;        /* Configurable-priority exception mask; reset = 0 */
    bool faultmask;      /* Raises execution priority to -1; reset = 0 */
    uint8_t basepri;     /* Raw BASEPRI byte; 0 = no masking */
    bool spsel;          /* CONTROL.SPSEL: Thread mode uses SP_process */
    bool npriv;          /* CONTROL.nPRIV: stored and readable, not enforced */
    uint32_t sp_main, sp_process; /* Banked copy of the stack that is NOT in r[SP] */
    uint8_t itstate;     /* EPSR.IT[7:0] for the NEXT instruction; 0 = outside an IT block */
    uint8_t it_exec;     /* IT state of the instruction being executed (decoder use) */
    uint16_t ipsr;       /* 0 = Thread mode; otherwise the current exception number */
    uint32_t pending_exceptions[8]; /* coalesced pending bits, by exception number */
    uint32_t active_exceptions[8];  /* active bits, by exception number */
    uint8_t shpr[16];    /* System handler priority bytes for exceptions 4..15 */
    uint8_t prigroup;    /* AIRCR.PRIGROUP */
    uint32_t vtor;       /* Vector table offset; reset 0 */
    bool sleeping;       /* Inside WFI/WFE, waiting for a wake-up event */
    bool event_register; /* WFE/SEV event register */
    bool exclusive;      /* Local exclusive monitor: open (true) after LDREX */
    uint32_t exclusive_address;
    uint64_t sleep_steps; /* Steps spent sleeping (a subset of steps) */
    uint64_t exceptions_taken;
    uint32_t hfsr;       /* HardFault status: FORCED (bit 30) on escalation */
    cpu_irq_enabled_fn irq_enabled;
    void *irq_enabled_ctx;
    cpu_irq_priority_fn irq_priority;
    void *irq_priority_ctx;
    uint64_t steps;      /* executed instructions plus sleep steps */
    bool halted;
    char halt_reason[160];
    cpu_step_hook_fn step_hook;
    void *step_hook_ctx;
    cpu_trace_fn trace_hook;
    void *trace_hook_ctx;
} cpu_t;

/* Reset core state, pending bits and hooks; load SP/PC from the vector table.
 * Install hooks AFTER cpu_reset(). Hook contexts are borrowed.
 */
void cpu_reset(cpu_t *cpu, bus_t *bus);

/* Called once after each steps increment (instruction or sleep step), including
 * a final halted attempt. No call for a failed pre-fetch dispatch/fetch, or a
 * call on a halted CPU.
 */
void cpu_set_step_hook(cpu_t *cpu, cpu_step_hook_fn hook, void *ctx);

/* Observe the instruction after pending dispatch and successful full fetch,
 * before PC advance/execution. The hook should not mutate the CPU or bus.
 */
void cpu_set_trace_hook(cpu_t *cpu, cpu_trace_fn trace, void *ctx);

/* External IRQs (exception 16+) require an installed enable query. Without a
 * priority query, external IRQs have priority 0.
 */
void cpu_set_irq_gate(cpu_t *cpu, cpu_irq_enabled_fn query, void *ctx);
void cpu_set_irq_priority(cpu_t *cpu, cpu_irq_priority_fn query, void *ctx);

/* Pending state for NMI (2), PendSV (14), SysTick (15) and 16..255. Pending bits
 * are acknowledged (cleared) only after a successful exception entry.
 */
bool cpu_pend_exception(cpu_t *cpu, unsigned number);
bool cpu_clear_pending_exception(cpu_t *cpu, unsigned number);
bool cpu_exception_pending(const cpu_t *cpu, unsigned number);
bool cpu_exception_active(const cpu_t *cpu, unsigned number);
uint32_t cpu_get_xpsr(const cpu_t *cpu);

/* Configured priority of an exception (-3..255; HardFault -1, NMI -2, Reset -3). */
int cpu_exception_priority(const cpu_t *cpu, unsigned number);
/* Current execution priority (B1.5.4), 256 when nothing raises it. */
int cpu_execution_priority(const cpu_t *cpu);
/* Highest-priority pending and enabled exception, or 0 if none (ignores masks). */
unsigned cpu_pending_winner(const cpu_t *cpu);

/* MSP and PSP regardless of which one is currently in r[SP]. */
uint32_t cpu_get_msp(const cpu_t *cpu);
uint32_t cpu_get_psp(const cpu_t *cpu);

/* Execute one instruction, or one sleep step. Returns 0, or -1 if the CPU is
 * (or just became) halted.
 */
int cpu_step(cpu_t *cpu, bus_t *bus);

/* Stop the CPU with a reason (printf-style). Used for BKPT, undefined instructions, bus faults. */
void cpu_halt(cpu_t *cpu, const char *fmt, ...);

#endif
