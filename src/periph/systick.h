/* Instruction-clocked SysTick. No CPU type or host clock dependency. */
#ifndef PERIPH_SYSTICK_H
#define PERIPH_SYSTICK_H

#include <stdint.h>

#include "bus.h"

#define SYSTICK_BASE         0xE000E010u
#define SYSTICK_CSR_OFFSET   0x00u
#define SYSTICK_RVR_OFFSET   0x04u
#define SYSTICK_CVR_OFFSET   0x08u

#define SYSTICK_ENABLE       (UINT32_C(1) << 0)
#define SYSTICK_TICKINT      (UINT32_C(1) << 1)
#define SYSTICK_CLKSOURCE    (UINT32_C(1) << 2)
#define SYSTICK_COUNTFLAG    (UINT32_C(1) << 16)
#define SYSTICK_COUNTER_MASK UINT32_C(0x00FFFFFF)

typedef struct systick systick_t;
typedef void (*systick_irq_fn)(void *ctx);

/* ctx is borrowed. A NULL callback is allowed for polling-only use.
 * The callback requests an exception; it must not enter it synchronously.
 */
systick_t *systick_new(systick_irq_fn request_irq, void *ctx);
/* Reset register state and diagnostics; preserve callback wiring. */
void systick_reset(systick_t *st);
void systick_free(systick_t *st);
periph_t systick_as_periph(systick_t *st);

/* One instruction tick. CLKSOURCE is stored but does not change this clock.
 * CVR=0 reloads without an event; only 1 -> 0 sets COUNTFLAG and requests IRQ.
 */
void systick_tick(systick_t *st);

/* Host observation: no read-to-clear or diagnostic side effects.
 * Offsets are relative to SYSTICK_BASE. An unsupported offset returns zero.
 */
uint32_t systick_peek(const systick_t *st, uint32_t offset);
uint64_t systick_bad_access_count(const systick_t *st);

#endif
