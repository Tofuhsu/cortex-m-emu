/* STM32F446 USART2: immediate, polling-only 8-bit TX routed through PA2/AF7.
 * No baud timing, RX, DMA or USART interrupts. No SR-read sequence state.
 */
#ifndef PERIPH_USART_H
#define PERIPH_USART_H

#include <stdint.h>

#include "bus.h"
#include "gpio.h"
#include "rcc.h"

#define USART2_BASE       0x40004400u
#define USART_SR_OFFSET  0x00u
#define USART_DR_OFFSET  0x04u
#define USART_BRR_OFFSET 0x08u
#define USART_CR1_OFFSET 0x0Cu

#define USART_SR_TC   (UINT32_C(1) << 6)
#define USART_SR_TXE  (UINT32_C(1) << 7)
#define USART_CR1_TE  (UINT32_C(1) << 3)
#define USART_CR1_UE  (UINT32_C(1) << 13)

typedef struct usart usart_t;
typedef void (*usart_tx_fn)(void *ctx, uint8_t byte);

typedef struct {
    uint64_t clock_gated_writes;      /* Any valid register write with APB1 off. */
    uint64_t ue_disabled_dr_writes;   /* APB1 on, UE off. */
    uint64_t te_disabled_dr_writes;   /* APB1 and UE on, TE off. */
    uint64_t pin_mismatch_bytes;      /* TX completed, PA2 not MODER=2/AF=7. */
    uint64_t bad_accesses;            /* Invalid access or unsupported CR1 mode. */
} usart_stats_t;

/* Fixed USART2/PA2 model. Both objects are borrowed and must outlive USART.
 * Returns NULL for NULL dependencies or allocation failure.
 */
usart_t *usart_new(const rcc_t *rcc, const gpio_t *gpioa);
/* Reset registers and stats without emitting bytes; preserve observer wiring. */
void usart_reset(usart_t *usart);
void usart_free(usart_t *usart);
/* Bus borrows this model; it accepts only aligned 32-bit SR/DR/BRR/CR1 access. */
periph_t usart_as_periph(usart_t *usart);

/* Called once per routed byte, after status is committed. ctx is borrowed.
 * NULL disables observation. Callback must not destroy or mutate the model.
 */
void usart_set_tx_observer(usart_t *usart, usart_tx_fn fn, void *ctx);

/* No side effects or diagnostics; bypass APB1 gating. Unknown offset -> 0.
 * DR reads return zero (RX is absent), not the last transmitted byte.
 * BRR stores its low 16 bits; it does not affect transmission timing.
 * CR1 supports UE/TE only. Reserved bits are ignored; unsupported RW bits
 * reject the complete CR1 write and increment bad_accesses.
 */
uint32_t usart_peek(const usart_t *usart, uint32_t offset);
usart_stats_t usart_get_stats(const usart_t *usart);

#endif
