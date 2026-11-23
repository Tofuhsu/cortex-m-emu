#include "scb.h"

#include <stdlib.h>

/* CPUID of a Cortex-M4 r0p1 (PM0214 4.4.1), the STM32F446's core revision. */
#define CPUID_VALUE 0x410FC241u
#define CCR_STKALIGN (1u << 9)
/* NONBASETHRDENA, USERSETMPEND, UNALIGN_TRP, DIV_0_TRP, BFHFNMIGN: stored, not applied. */
#define CCR_WRITABLE 0x31Bu
#define SCR_WRITABLE 0x16u

struct scb {
    cpu_t *cpu;
    uint32_t scr, ccr, shcsr_enables, cpacr;
    scb_reset_fn reset_request;
    void *reset_ctx;
    uint64_t bad_accesses;
};

static uint32_t width_mask(int size)
{
    return size == 4 ? UINT32_MAX : (1u << (8u * (unsigned)size)) - 1u;
}

/* SHPR bytes exist for MemManage(4), BusFault(5), UsageFault(6), SVCall(11),
 * DebugMonitor(12), PendSV(14) and SysTick(15); the rest are RAZ/WI. Four
 * priority bits are implemented, as on STM32F4.
 */
static bool shpr_implemented(unsigned number)
{
    return number == 4u || number == 5u || number == 6u || number == 11u ||
           number == 12u || number == 14u || number == 15u;
}

static uint32_t read_word(scb_t *s, uint32_t reg)
{
    cpu_t *c = s->cpu;
    switch (reg) {
    case SCB_CPUID: return CPUID_VALUE;
    case SCB_ICSR: {
        unsigned active_count = 0;
        for (unsigned n = 1; n < CPU_EXCEPTION_COUNT; ++n) active_count += cpu_exception_active(c, n);
        unsigned winner = cpu_pending_winner(c);
        bool external_pending = false;
        for (unsigned n = 16; n < CPU_EXCEPTION_COUNT && !external_pending; ++n)
            external_pending = cpu_exception_pending(c, n);
        uint32_t value = (uint32_t)(c->ipsr & 0x1FFu);
        if (active_count <= 1u) value |= 1u << 11;            /* RETTOBASE */
        value |= (uint32_t)(winner & 0x1FFu) << 12;            /* VECTPENDING */
        if (external_pending) value |= 1u << 22;               /* ISRPENDING */
        if (cpu_exception_pending(c, CPU_EXCEPTION_SYSTICK)) value |= 1u << 26;
        if (cpu_exception_pending(c, CPU_EXCEPTION_PENDSV)) value |= 1u << 28;
        if (cpu_exception_pending(c, CPU_EXCEPTION_NMI)) value |= 1u << 31;
        return value;
    }
    case SCB_VTOR: return c->vtor;
    case SCB_AIRCR: return 0xFA050000u | ((uint32_t)c->prigroup << 8);
    case SCB_SCR: return s->scr;
    case SCB_CCR: return s->ccr;
    case SCB_SHPR1: case SCB_SHPR2: case SCB_SHPR3: {
        unsigned first = 4u + (reg - SCB_SHPR1);
        uint32_t value = 0;
        for (unsigned i = 0; i < 4u; ++i) value |= (uint32_t)c->shpr[first + i] << (8u * i);
        return value;
    }
    case SCB_SHCSR: {
        uint32_t value = s->shcsr_enables;
        static const struct { unsigned number, bit; } act[] = {
            {4, 0}, {5, 1}, {6, 3}, {11, 7}, {12, 8}, {14, 10}, {15, 11}};
        for (unsigned i = 0; i < sizeof act / sizeof act[0]; ++i)
            if (cpu_exception_active(c, act[i].number)) value |= 1u << act[i].bit;
        if (cpu_exception_pending(c, CPU_EXCEPTION_SVCALL)) value |= 1u << 15;
        return value;
    }
    case SCB_CFSR: return 0; /* configurable faults are never raised: the model halts */
    case SCB_HFSR: return c->hfsr;
    case SCB_CPACR: return s->cpacr;
    default: return 0;
    }
}

static void write_word(scb_t *s, uint32_t reg, uint32_t value, uint32_t mask)
{
    cpu_t *c = s->cpu;
    switch (reg) {
    case SCB_ICSR:
        if (value & (1u << 31)) cpu_pend_exception(c, CPU_EXCEPTION_NMI);
        if (value & (1u << 28)) cpu_pend_exception(c, CPU_EXCEPTION_PENDSV);
        else if (value & (1u << 27)) cpu_clear_pending_exception(c, CPU_EXCEPTION_PENDSV);
        if (value & (1u << 26)) cpu_pend_exception(c, CPU_EXCEPTION_SYSTICK);
        else if (value & (1u << 25)) cpu_clear_pending_exception(c, CPU_EXCEPTION_SYSTICK);
        return;
    case SCB_VTOR:
        c->vtor = ((c->vtor & ~mask) | (value & mask)) & 0x3FFFFF80u;
        return;
    case SCB_AIRCR:
        if (mask != UINT32_MAX || (value >> 16) != 0x05FAu) { s->bad_accesses++; return; }
        c->prigroup = (uint8_t)((value >> 8) & 7u);
        if ((value & (1u << 2)) && s->reset_request) s->reset_request(s->reset_ctx);
        return;
    case SCB_SCR: s->scr = ((s->scr & ~mask) | (value & mask)) & SCR_WRITABLE; return;
    case SCB_CCR: s->ccr = (((s->ccr & ~mask) | (value & mask)) & CCR_WRITABLE) | CCR_STKALIGN; return;
    case SCB_SHPR1: case SCB_SHPR2: case SCB_SHPR3: {
        unsigned first = 4u + (reg - SCB_SHPR1);
        for (unsigned i = 0; i < 4u; ++i) {
            if (!((mask >> (8u * i)) & 0xFFu) || !shpr_implemented(first + i)) continue;
            c->shpr[first + i] = (uint8_t)(value >> (8u * i)) & 0xF0u;
        }
        return;
    }
    case SCB_SHCSR:
        /* Enable bits 16-18 are stored; the model never raises these faults. */
        s->shcsr_enables = ((s->shcsr_enables & ~mask) | (value & mask)) & 0x70000u;
        return;
    case SCB_HFSR: c->hfsr &= ~(value & mask); return; /* write-one-to-clear */
    case SCB_CFSR: return;
    case SCB_CPACR: s->cpacr = ((s->cpacr & ~mask) | (value & mask)) & 0x00F00000u; return;
    default: return; /* CPUID is read-only */
    }
}

static bool known(uint32_t reg)
{
    return reg <= SCB_HFSR || reg == SCB_CPACR || reg == 0x34u || reg == 0x38u;
}

static uint32_t scb_read(void *ctx, uint32_t offset, int size)
{
    scb_t *s = ctx;
    uint32_t reg = offset & ~3u;
    if ((offset % (uint32_t)size) != 0 || !known(reg)) { s->bad_accesses++; return 0; }
    return (read_word(s, reg) >> (8u * (offset & 3u))) & width_mask(size);
}

static void scb_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    scb_t *s = ctx;
    uint32_t reg = offset & ~3u;
    if ((offset % (uint32_t)size) != 0 || !known(reg)) { s->bad_accesses++; return; }
    unsigned shift = 8u * (offset & 3u);
    write_word(s, reg, (value & width_mask(size)) << shift, width_mask(size) << shift);
}

scb_t *scb_new(cpu_t *cpu)
{
    if (!cpu) return NULL;
    scb_t *s = calloc(1, sizeof *s);
    if (s) { s->cpu = cpu; scb_reset(s); }
    return s;
}

void scb_reset(scb_t *s)
{
    s->scr = 0;
    s->ccr = CCR_STKALIGN;
    s->shcsr_enables = 0;
    s->cpacr = 0;
    s->bad_accesses = 0;
}

void scb_free(scb_t *s) { free(s); }

void scb_set_reset_request(scb_t *s, scb_reset_fn fn, void *ctx)
{
    s->reset_request = fn;
    s->reset_ctx = ctx;
}

periph_t scb_as_periph(scb_t *s)
{
    return (periph_t){.name = "SCB", .base = SCB_BASE, .length = 0x90u,
                      .ctx = s, .read = scb_read, .write = scb_write};
}

uint64_t scb_bad_access_count(const scb_t *s) { return s->bad_accesses; }
