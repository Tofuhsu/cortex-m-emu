#include "systick.h"

#include <stdbool.h>
#include <stdlib.h>

#define SYSTICK_LENGTH 0x0Cu
#define SYSTICK_CONTROL_MASK (SYSTICK_ENABLE | SYSTICK_TICKINT | SYSTICK_CLKSOURCE)

struct systick {
    uint32_t control, reload, current;
    bool countflag;
    uint64_t bad_accesses;
    systick_irq_fn request_irq;
    void *irq_ctx;
};

static bool valid_access(uint32_t offset, int size)
{
    return size == 4 && (offset == SYSTICK_CSR_OFFSET ||
                        offset == SYSTICK_RVR_OFFSET ||
                        offset == SYSTICK_CVR_OFFSET);
}

uint32_t systick_peek(const systick_t *st, uint32_t offset)
{
    switch (offset) {
    case SYSTICK_CSR_OFFSET:
        return st->control | (st->countflag ? SYSTICK_COUNTFLAG : 0u);
    case SYSTICK_RVR_OFFSET:
        return st->reload;
    case SYSTICK_CVR_OFFSET:
        return st->current;
    default:
        return 0;
    }
}

static uint32_t systick_read(void *ctx, uint32_t offset, int size)
{
    systick_t *st = ctx;
    if (!valid_access(offset, size)) {
        st->bad_accesses++;
        return 0;
    }
    uint32_t value = systick_peek(st, offset);
    if (offset == SYSTICK_CSR_OFFSET) {
        st->countflag = false;
    }
    return value;
}

static void systick_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    systick_t *st = ctx;
    if (!valid_access(offset, size)) {
        st->bad_accesses++;
        return;
    }
    switch (offset) {
    case SYSTICK_CSR_OFFSET:
        st->control = value & SYSTICK_CONTROL_MASK;
        break; /* COUNTFLAG is read-only; CPU pending state is independent. */
    case SYSTICK_RVR_OFFSET:
        st->reload = value & SYSTICK_COUNTER_MASK;
        break; /* The current countdown completes before loading the new value. */
    case SYSTICK_CVR_OFFSET:
        st->current = 0;
        st->countflag = false;
        break; /* No interrupt request and no pending cancellation. */
    }
}

systick_t *systick_new(systick_irq_fn request_irq, void *ctx)
{
    systick_t *st = calloc(1, sizeof *st);
    if (st) {
        st->request_irq = request_irq;
        st->irq_ctx = ctx;
    }
    return st;
}

void systick_reset(systick_t *st)
{
    st->control = 0;
    /* Architecture RVR/CVR reset is UNKNOWN; zero is this model's policy. */
    st->reload = 0;
    st->current = 0;
    st->countflag = false;
    st->bad_accesses = 0;
}

void systick_free(systick_t *st)
{
    free(st);
}

periph_t systick_as_periph(systick_t *st)
{
    return (periph_t){
        .name = "SysTick", .base = SYSTICK_BASE, .length = SYSTICK_LENGTH,
        .ctx = st, .read = systick_read, .write = systick_write,
    };
}

void systick_tick(systick_t *st)
{
    if ((st->control & SYSTICK_ENABLE) == 0) {
        return;
    }
    if (st->current == 0) {
        st->current = st->reload;
        return; /* RVR=0 stays at zero: there is no 1 -> 0 event. */
    }
    st->current--;
    if (st->current == 0) {
        st->countflag = true;
        if ((st->control & SYSTICK_TICKINT) != 0 && st->request_irq) {
            st->request_irq(st->irq_ctx);
        }
    }
}

uint64_t systick_bad_access_count(const systick_t *st)
{
    return st->bad_accesses;
}
