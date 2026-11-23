/* STM32F446 SYSCFG EXTICR1-4 and EXTI lines 0-15 (RM0390 Rev9 sections 8
 * and 10.3). GPIO input edges come from injected GPIO input levels; lines
 * 16-22 (PVD, RTC, USB...) are not modeled.
 */
#ifndef PERIPH_EXTI_H
#define PERIPH_EXTI_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "rcc.h"

#define SYSCFG_BASE 0x40013800u
#define EXTI_BASE   0x40013C00u
#define EXTI_IMR   0x00u
#define EXTI_EMR   0x04u
#define EXTI_RTSR  0x08u
#define EXTI_FTSR  0x0Cu
#define EXTI_SWIER 0x10u
#define EXTI_PR    0x14u

typedef struct exti exti_t;
/* Request the NVIC line for an EXTI line: IRQ 6-10 for lines 0-4, 23 for
 * 5-9 and 40 for 10-15 (RM0390 table 38). */
typedef void (*exti_irq_fn)(void *ctx, unsigned irq);

exti_t *exti_new(const rcc_t *rcc);
void exti_free(exti_t *exti);
void exti_reset(exti_t *exti);
periph_t exti_as_periph(exti_t *exti);
periph_t exti_syscfg_as_periph(exti_t *exti);
void exti_set_irq(exti_t *exti, exti_irq_fn fn, void *ctx);
/* A GPIO input level change: port 0 = A, 1 = B, 2 = C ... Only the port
 * selected for that line in SYSCFG_EXTICRx produces edges. */
void exti_gpio_changed(exti_t *exti, unsigned port, uint16_t old_levels, uint16_t new_levels);
uint64_t exti_bad_access_count(const exti_t *exti);
/* Edges seen on a line with its trigger enabled (diagnostic). */
uint64_t exti_edge_count(const exti_t *exti);

#endif
