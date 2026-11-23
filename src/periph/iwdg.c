#include "iwdg.h"

#include <stdlib.h>

struct iwdg {
    uint32_t steps_per_lsi, pr, rlr, counter, lsi_phase, prescale_phase;
    bool running, access;
    iwdg_reset_fn reset;
    void *reset_ctx;
    uint64_t reloads, bad_accesses;
};

static uint32_t divider(const iwdg_t *w) { unsigned p = w->pr > 6u ? 6u : w->pr; return 4u << p; }

static uint32_t iwdg_read(void *ctx, uint32_t offset, int size)
{
    iwdg_t *w = ctx;
    if (size != 4 && size != 2) { w->bad_accesses++; return 0; }
    switch (offset) {
    case IWDG_PR: return w->pr;
    case IWDG_RLR: return w->rlr;
    case IWDG_SR: return 0; /* PVU/RVU: updates take effect at once in this model */
    default: return 0;
    }
}

static void iwdg_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    iwdg_t *w = ctx;
    if (size != 4 && size != 2) { w->bad_accesses++; return; }
    switch (offset) {
    case IWDG_KR:
        value &= 0xFFFFu;
        if (value == 0x5555u) w->access = true;
        else {
            w->access = false;
            if (value == 0xAAAAu) { w->counter = w->rlr; w->reloads++; }
            else if (value == 0xCCCCu && !w->running) {
                w->running = true;
                w->counter = w->rlr;
                w->lsi_phase = w->prescale_phase = 0;
            }
        }
        return;
    case IWDG_PR: if (w->access) w->pr = value & 7u; return;
    case IWDG_RLR: if (w->access) w->rlr = value & 0xFFFu; return;
    default: return;
    }
}

void iwdg_tick(iwdg_t *w)
{
    if (!w->running) return;
    if (++w->lsi_phase < w->steps_per_lsi) return;
    w->lsi_phase = 0;
    if (++w->prescale_phase < divider(w)) return;
    w->prescale_phase = 0;
    if (w->counter > 0) --w->counter;
    if (w->counter == 0 && w->reset) w->reset(w->reset_ctx);
}

iwdg_t *iwdg_new(uint32_t steps_per_lsi)
{
    if (steps_per_lsi == 0) return NULL;
    iwdg_t *w = calloc(1, sizeof *w);
    if (w) { w->steps_per_lsi = steps_per_lsi; iwdg_reset(w); }
    return w;
}

void iwdg_reset(iwdg_t *w)
{
    w->pr = 0; w->rlr = 0xFFFu; w->counter = 0xFFFu;
    w->lsi_phase = w->prescale_phase = 0;
    w->running = w->access = false;
    w->reloads = w->bad_accesses = 0;
}

void iwdg_free(iwdg_t *w) { free(w); }
void iwdg_set_reset(iwdg_t *w, iwdg_reset_fn fn, void *ctx) { w->reset = fn; w->reset_ctx = ctx; }
bool iwdg_running(const iwdg_t *w) { return w->running; }
uint32_t iwdg_counter(const iwdg_t *w) { return w->counter; }
uint64_t iwdg_reload_count(const iwdg_t *w) { return w->reloads; }

periph_t iwdg_as_periph(iwdg_t *w)
{
    return (periph_t){.name = "IWDG", .base = IWDG_BASE, .length = 0x400u,
                      .ctx = w, .read = iwdg_read, .write = iwdg_write};
}
