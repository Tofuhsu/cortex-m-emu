#include "rcc.h"

#include <stdlib.h>

#define RCC_LENGTH 0x400u
/* RM0390: GPIOA-H, CRC, BKPSRAM, DMA1/2, OTGHS and OTGHSULPI. */
#define AHB1ENR_WRITABLE 0x606410FFu

struct rcc {
    uint32_t ahb1enr;
    uint64_t bad_accesses;
};

static bool valid_access(uint32_t offset, int size)
{
    if (size != 1 && size != 2 && size != 4) {
        return false;
    }
    return offset < RCC_LENGTH &&
           offset % (uint32_t)size == 0 &&
           (offset & ~3u) == RCC_AHB1ENR_OFFSET;
}

static uint32_t width_mask(int size)
{
    return size == 4 ? UINT32_MAX : (1u << (8u * (unsigned)size)) - 1u;
}

static uint32_t rcc_read(void *ctx, uint32_t offset, int size)
{
    rcc_t *rcc = ctx;
    if (!valid_access(offset, size)) {
        rcc->bad_accesses++;
        return 0;
    }
    return (rcc->ahb1enr >> (8u * (offset & 3u))) & width_mask(size);
}

static void rcc_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    rcc_t *rcc = ctx;
    if (!valid_access(offset, size)) {
        rcc->bad_accesses++;
        return;
    }
    unsigned shift = 8u * (offset & 3u);
    uint32_t mask = width_mask(size) << shift;
    uint32_t command = (value & width_mask(size)) << shift;
    rcc->ahb1enr = ((rcc->ahb1enr & ~mask) | command) & AHB1ENR_WRITABLE;
}

rcc_t *rcc_new(void)
{
    return calloc(1, sizeof(rcc_t));
}

void rcc_reset(rcc_t *rcc)
{
    rcc->ahb1enr = 0;
    rcc->bad_accesses = 0;
}

void rcc_free(rcc_t *rcc)
{
    free(rcc);
}

periph_t rcc_as_periph(rcc_t *rcc)
{
    return (periph_t){
        .name = "RCC", .base = RCC_BASE, .length = RCC_LENGTH,
        .ctx = rcc, .read = rcc_read, .write = rcc_write,
    };
}

bool rcc_ahb1_clock_enabled(const rcc_t *rcc, unsigned bit)
{
    return bit < 32u && (rcc->ahb1enr & (UINT32_C(1) << bit)) != 0;
}

uint64_t rcc_bad_access_count(const rcc_t *rcc)
{
    return rcc->bad_accesses;
}
