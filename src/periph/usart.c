#include "usart.h"

#include <stdlib.h>

#define USART_LENGTH 0x400u
#define USART_SR_RESET (USART_SR_TXE | USART_SR_TC)
#define USART_BRR_WRITABLE 0xFFFFu
#define USART_CR1_WRITABLE 0xBFFFu
#define USART_CR1_SUPPORTED (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | \
                             USART_CR1_RXNEIE | USART_CR1_TXEIE)

struct usart {
    const rcc_t *rcc;
    const gpio_t *gpioa;
    uint32_t sr, brr, cr1;
    uint8_t rdr;
    usart_stats_t stats;
    usart_tx_fn observer;
    void *observer_ctx;
    usart_irq_fn irq;
    void *irq_ctx;
};

static bool irq_asserted(const usart_t *usart)
{
    return ((usart->cr1 & USART_CR1_TXEIE) && (usart->sr & USART_SR_TXE)) ||
           ((usart->cr1 & USART_CR1_RXNEIE) && (usart->sr & (USART_SR_RXNE | USART_SR_ORE)));
}

static void update_irq(usart_t *usart)
{
    if (usart->irq && irq_asserted(usart)) usart->irq(usart->irq_ctx);
}

static bool valid_access(uint32_t offset, int size)
{
    return size == 4 && (offset & 3u) == 0 &&
           (offset == USART_SR_OFFSET || offset == USART_DR_OFFSET ||
            offset == USART_BRR_OFFSET || offset == USART_CR1_OFFSET);
}

uint32_t usart_peek(const usart_t *usart, uint32_t offset)
{
    switch (offset) {
    case USART_SR_OFFSET:  return usart->sr;
    case USART_DR_OFFSET:  return usart->rdr; /* RDR is separate from TDR. */
    case USART_BRR_OFFSET: return usart->brr;
    case USART_CR1_OFFSET: return usart->cr1;
    default:              return 0;
    }
}

static uint32_t usart_read(void *ctx, uint32_t offset, int size)
{
    usart_t *usart = ctx;
    if (!valid_access(offset, size)) {
        usart->stats.bad_accesses++;
        return 0;
    }
    if (!rcc_apb1_clock_enabled(usart->rcc, RCC_USART2_ENABLE_BIT)) {
        return 0;
    }
    uint32_t value = usart_peek(usart, offset);
    if (offset == USART_DR_OFFSET) {
        /* The SR-then-DR sequence that clears ORE is not tracked: any DR
         * read clears RXNE and ORE. */
        usart->sr &= ~(USART_SR_RXNE | USART_SR_ORE);
    }
    return value;
}

static void usart_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    usart_t *usart = ctx;
    if (!valid_access(offset, size)) {
        usart->stats.bad_accesses++;
        return;
    }
    if (!rcc_apb1_clock_enabled(usart->rcc, RCC_USART2_ENABLE_BIT)) {
        usart->stats.clock_gated_writes++;
        return;
    }
    switch (offset) {
    case USART_SR_OFFSET:
        /* TC is rc_w0. Never set status bits or change TXE via SR writes. */
        if ((value & USART_SR_TC) == 0) {
            usart->sr &= ~USART_SR_TC;
        }
        return;
    case USART_BRR_OFFSET:
        usart->brr = value & USART_BRR_WRITABLE;
        return;
    case USART_CR1_OFFSET: {
        uint32_t control = value & USART_CR1_WRITABLE;
        if ((control & ~USART_CR1_SUPPORTED) != 0) {
            /* Do not silently claim support for RX, IRQs or other formats. */
            usart->stats.bad_accesses++;
            return;
        }
        usart->cr1 = control;
        update_irq(usart);
        return;
    }
    case USART_DR_OFFSET:
        if ((usart->cr1 & USART_CR1_UE) == 0) {
            usart->stats.ue_disabled_dr_writes++;
            return;
        }
        if ((usart->cr1 & USART_CR1_TE) == 0) {
            usart->stats.te_disabled_dr_writes++;
            return;
        }
        /* Accepted TX completes in this access. No observable busy interval,
         * baud clock or SR-read/DR-write clearing sequence is modeled.
         * Pin mux affects routing, not the USART's internal completion.
         */
        usart->sr |= USART_SR_TXE | USART_SR_TC;
        if (!gpio_pin_is_af(usart->gpioa, 2u, 7u)) {
            usart->stats.pin_mismatch_bytes++;
        } else if (usart->observer) {
            usart->observer(usart->observer_ctx, (uint8_t)(value & 0xFFu));
        }
        update_irq(usart);
        return;
    }
}

usart_t *usart_new(const rcc_t *rcc, const gpio_t *gpioa)
{
    if (!rcc || !gpioa) {
        return NULL;
    }
    usart_t *usart = calloc(1, sizeof *usart);
    if (usart) {
        usart->rcc = rcc;
        usart->gpioa = gpioa;
        usart_reset(usart);
    }
    return usart;
}

void usart_reset(usart_t *usart)
{
    /* RM0390 25.6.8: TXE and TC reset to one; all other modeled bits zero. */
    usart->sr = USART_SR_RESET;
    usart->brr = 0;
    usart->cr1 = 0;
    usart->rdr = 0;
    usart->stats = (usart_stats_t){0};
}

void usart_set_irq(usart_t *usart, usart_irq_fn fn, void *ctx)
{
    usart->irq = fn;
    usart->irq_ctx = ctx;
}

bool usart_receive(usart_t *usart, uint8_t byte)
{
    if (!rcc_apb1_clock_enabled(usart->rcc, RCC_USART2_ENABLE_BIT) ||
        (usart->cr1 & (USART_CR1_UE | USART_CR1_RE)) != (USART_CR1_UE | USART_CR1_RE) ||
        (usart->sr & USART_SR_RXNE)) {
        return false;
    }
    usart->rdr = byte;
    usart->sr |= USART_SR_RXNE;
    update_irq(usart);
    return true;
}

void usart_free(usart_t *usart)
{
    free(usart);
}

periph_t usart_as_periph(usart_t *usart)
{
    return (periph_t){
        .name = "USART2", .base = USART2_BASE, .length = USART_LENGTH,
        .ctx = usart, .read = usart_read, .write = usart_write,
    };
}

void usart_set_tx_observer(usart_t *usart, usart_tx_fn fn, void *ctx)
{
    usart->observer = fn;
    usart->observer_ctx = ctx;
}

usart_stats_t usart_get_stats(const usart_t *usart)
{
    return usart->stats;
}
