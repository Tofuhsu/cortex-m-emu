/* System Control Block (PM0214 Rev10 4.4, ARM DDI0403E B3.2): the registers
 * an RTOS port touches. State that the CPU's exception model uses (pending
 * PendSV/SysTick/NMI, active bits, SHPR priorities, PRIGROUP, VTOR, HFSR)
 * lives in cpu_t; this model is its register view.
 */
#ifndef PERIPH_SCB_H
#define PERIPH_SCB_H

#include "cpu.h"

#define SCB_BASE   0xE000ED00u
#define SCB_CPUID  0x00u
#define SCB_ICSR   0x04u
#define SCB_VTOR   0x08u
#define SCB_AIRCR  0x0Cu
#define SCB_SCR    0x10u
#define SCB_CCR    0x14u
#define SCB_SHPR1  0x18u
#define SCB_SHPR2  0x1Cu
#define SCB_SHPR3  0x20u
#define SCB_SHCSR  0x24u
#define SCB_CFSR   0x28u
#define SCB_HFSR   0x2Cu
#define SCB_CPACR  0x88u

typedef struct scb scb_t;
typedef void (*scb_reset_fn)(void *ctx);

/* cpu is borrowed and must outlive the model. */
scb_t *scb_new(cpu_t *cpu);
void scb_free(scb_t *scb);
/* Reset SCB-only registers (SCR, CCR, SHCSR enables, CPACR). CPU-held
 * state is reset by cpu_reset(). */
void scb_reset(scb_t *scb);
periph_t scb_as_periph(scb_t *scb);
/* AIRCR.SYSRESETREQ with the correct VECTKEY calls this; NULL ignores it. */
void scb_set_reset_request(scb_t *scb, scb_reset_fn fn, void *ctx);
uint64_t scb_bad_access_count(const scb_t *scb);

#endif
