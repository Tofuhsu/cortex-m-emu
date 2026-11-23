#include "exti.h"

#include <stdlib.h>

#define EXTI_LINES_MASK 0x007FFFFFu /* 23 lines exist; only 0-15 get inputs */

struct exti {
    const rcc_t *rcc;
    uint32_t imr, emr, rtsr, ftsr, pr;
    uint32_t exticr[4];
    exti_irq_fn irq;
    void *irq_ctx;
    uint64_t bad_accesses, edges;
};

static unsigned line_irq(unsigned line)
{
    if (line <= 4u) return 6u + line;
    return line <= 9u ? 23u : 40u;
}

static void raise(exti_t *e, unsigned line)
{
    e->pr |= 1u << line;
    if ((e->imr & (1u << line)) && e->irq) e->irq(e->irq_ctx, line_irq(line));
}

static uint32_t exti_read(void *ctx, uint32_t offset, int size)
{
    exti_t *e = ctx;
    if (size != 4 || (offset & 3u) || offset > EXTI_PR) { e->bad_accesses++; return 0; }
    switch (offset) {
    case EXTI_IMR: return e->imr;
    case EXTI_EMR: return e->emr;
    case EXTI_RTSR: return e->rtsr;
    case EXTI_FTSR: return e->ftsr;
    case EXTI_SWIER: return 0;
    default: return e->pr;
    }
}

static void exti_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    exti_t *e = ctx;
    if (size != 4 || (offset & 3u) || offset > EXTI_PR) { e->bad_accesses++; return; }
    value &= EXTI_LINES_MASK;
    switch (offset) {
    case EXTI_IMR:
        e->imr = value;
        /* Unmasking a line that is already pending requests its IRQ. */
        for (unsigned line = 0; line < 23u; ++line)
            if ((value & e->pr & (1u << line)) && e->irq) e->irq(e->irq_ctx, line_irq(line));
        return;
    case EXTI_EMR: e->emr = value; return; /* events (WFE) are not routed */
    case EXTI_RTSR: e->rtsr = value; return;
    case EXTI_FTSR: e->ftsr = value; return;
    case EXTI_SWIER:
        for (unsigned line = 0; line < 23u; ++line)
            if (value & (1u << line)) raise(e, line);
        return;
    default:
        e->pr &= ~value; /* rc_w1. The NVIC pending bit is cleared separately. */
        return;
    }
}

static uint32_t syscfg_read(void *ctx, uint32_t offset, int size)
{
    exti_t *e = ctx;
    if (size != 4 || (offset & 3u)) { e->bad_accesses++; return 0; }
    if (!rcc_apb2_clock_enabled(e->rcc, RCC_SYSCFG_ENABLE_BIT)) return 0;
    if (offset >= 0x08u && offset <= 0x14u) return e->exticr[(offset - 0x08u) / 4u];
    return 0; /* MEMRMP, PMC, CMPCR, CFGR: read as zero, not modeled */
}

static void syscfg_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    exti_t *e = ctx;
    if (size != 4 || (offset & 3u)) { e->bad_accesses++; return; }
    if (!rcc_apb2_clock_enabled(e->rcc, RCC_SYSCFG_ENABLE_BIT)) { e->bad_accesses++; return; }
    if (offset >= 0x08u && offset <= 0x14u) e->exticr[(offset - 0x08u) / 4u] = value & 0xFFFFu;
    else e->bad_accesses++;
}

void exti_gpio_changed(exti_t *e, unsigned port, uint16_t old_levels, uint16_t new_levels)
{
    uint16_t changed = old_levels ^ new_levels;
    for (unsigned line = 0; line < 16u; ++line) {
        if (!(changed & (1u << line))) continue;
        unsigned selected = (e->exticr[line / 4u] >> (4u * (line % 4u))) & 0xFu;
        if (selected != port) continue;
        bool rising = (new_levels & (1u << line)) != 0;
        if ((rising && (e->rtsr & (1u << line))) || (!rising && (e->ftsr & (1u << line)))) {
            e->edges++;
            raise(e, line);
        }
    }
}

exti_t *exti_new(const rcc_t *rcc)
{
    if (!rcc) return NULL;
    exti_t *e = calloc(1, sizeof *e);
    if (e) { e->rcc = rcc; exti_reset(e); }
    return e;
}

void exti_reset(exti_t *e)
{
    e->imr = e->emr = e->rtsr = e->ftsr = e->pr = 0;
    for (unsigned i = 0; i < 4u; ++i) e->exticr[i] = 0;
    e->bad_accesses = e->edges = 0;
}

void exti_free(exti_t *e) { free(e); }
void exti_set_irq(exti_t *e, exti_irq_fn fn, void *ctx) { e->irq = fn; e->irq_ctx = ctx; }
uint64_t exti_bad_access_count(const exti_t *e) { return e->bad_accesses; }
uint64_t exti_edge_count(const exti_t *e) { return e->edges; }

periph_t exti_as_periph(exti_t *e)
{
    return (periph_t){.name = "EXTI", .base = EXTI_BASE, .length = 0x400u,
                      .ctx = e, .read = exti_read, .write = exti_write};
}

periph_t exti_syscfg_as_periph(exti_t *e)
{
    return (periph_t){.name = "SYSCFG", .base = SYSCFG_BASE, .length = 0x400u,
                      .ctx = e, .read = syscfg_read, .write = syscfg_write};
}
