#include "nvic.h"

#include <stdlib.h>
#include <string.h>

/* PM0214 4.3.2-4.3.7 / ARM ARM B3.4: architectural capacity, 240 IRQs.
 * This is not a claim that all these vectors exist on STM32F446.
 * Priority bytes retain four implemented high bits, but do not arbitrate yet.
 */
struct nvic {
    cpu_t *cpu;
    uint32_t enabled[8];
    uint8_t priority[CPU_EXTERNAL_IRQ_COUNT];
    uint64_t bad_accesses;
};

static bool enabled(void *ctx, unsigned irq)
{
    const nvic_t *n = ctx;
    return irq < CPU_EXTERNAL_IRQ_COUNT &&
           (n->enabled[irq / 32u] & (UINT32_C(1) << (irq % 32u))) != 0;
}

static uint32_t access_reg(nvic_t *n, uint32_t offset, uint32_t value, int size, bool write)
{
    if (offset >= 0x300u && offset < 0x3F0u &&
        (size == 1 || size == 2 || size == 4) && offset % (unsigned)size == 0) {
        uint32_t result = 0;
        for (unsigned i = 0; i < (unsigned)size; ++i) {
            unsigned irq = offset - 0x300u + i;
            if (write) n->priority[irq] = (uint8_t)(value >> (i * 8u)) & 0xF0u;
            result |= (uint32_t)n->priority[irq] << (i * 8u);
        }
        return result;
    }
    unsigned group = offset & ~0x7Fu;
    unsigned bank = (offset & 0x7Fu) / 4u;
    if (size != 4 || (offset & 3u) || bank >= 8u || group > 0x200u) {
        ++n->bad_accesses;
        return 0;
    }
    uint32_t mask = bank == 7u ? 0xFFFFu : UINT32_MAX;
    value &= mask;
    if (group == 0 || group == 0x80u) {
        if (write) {
            if (group == 0) n->enabled[bank] |= value;
            else n->enabled[bank] &= ~value;
        }
        return n->enabled[bank];
    }
    uint32_t result = 0;
    for (unsigned bit = 0; bit < 32u && bank * 32u + bit < CPU_EXTERNAL_IRQ_COUNT; ++bit) {
        unsigned exception = bank * 32u + bit + 16u;
        uint32_t flag = UINT32_C(1) << bit;
        if (group == 0x200u) {
            if (n->cpu->ipsr == exception) result |= flag;
        } else {
            if (write && (value & flag)) {
                if (group == 0x100u) cpu_pend_exception(n->cpu, exception);
                else cpu_clear_pending_exception(n->cpu, exception);
            }
            if (cpu_exception_pending(n->cpu, exception)) result |= flag;
        }
    }
    return result;
}

static uint32_t read_reg(void *ctx, uint32_t offset, int size)
{
    return access_reg(ctx, offset, 0, size, false);
}

static void write_reg(void *ctx, uint32_t offset, uint32_t value, int size)
{
    (void)access_reg(ctx, offset, value, size, true);
}

nvic_t *nvic_new(cpu_t *cpu)
{
    if (!cpu) return NULL;
    nvic_t *n = calloc(1, sizeof *n);
    if (n) {
        n->cpu = cpu;
        cpu_set_irq_gate(cpu, enabled, n);
    }
    return n;
}

void nvic_reset(nvic_t *n)
{
    memset(n->enabled, 0, sizeof n->enabled);
    memset(n->priority, 0, sizeof n->priority);
    for (unsigned irq = 0; irq < CPU_EXTERNAL_IRQ_COUNT; ++irq)
        cpu_clear_pending_exception(n->cpu, 16u + irq);
    n->bad_accesses = 0;
    cpu_set_irq_gate(n->cpu, enabled, n);
}

void nvic_free(nvic_t *n)
{
    if (n && n->cpu->irq_enabled_ctx == n) cpu_set_irq_gate(n->cpu, NULL, NULL);
    free(n);
}

bool nvic_pend(nvic_t *n, unsigned irq)
{
    return irq < CPU_EXTERNAL_IRQ_COUNT && cpu_pend_exception(n->cpu, 16u + irq);
}

periph_t nvic_as_periph(nvic_t *n)
{
    return (periph_t){.name = "NVIC", .base = NVIC_BASE, .length = 0x400u,
                      .ctx = n, .read = read_reg, .write = write_reg};
}

uint64_t nvic_bad_access_count(const nvic_t *n) { return n->bad_accesses; }
