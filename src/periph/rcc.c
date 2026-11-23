#include "rcc.h"

#include <stdlib.h>

#define RCC_LENGTH 0x400u
/* RM0390: GPIOA-H, CRC, BKPSRAM, DMA1/2, OTGHS and OTGHSULPI. */
#define AHB1ENR_WRITABLE 0x606410FFu
/* RM0390 6.3.13: bits 29:14, 11 and 8:0; all reset to zero. */
#define APB1ENR_WRITABLE 0x3FFFC9FFu
/* RM0390 6.3.14: TIM1/8, USART1/6, ADC1-3, SDIO, SPI1/4, SYSCFG, TIM9-11, SAI1/2. */
#define APB2ENR_WRITABLE 0x00C77F33u
#define CSR_RESET_FLAGS  0xFE000000u

struct rcc {
    uint32_t ahb1enr, apb1enr, apb2enr, csr;
    uint64_t bad_accesses;
};

static bool valid_access(uint32_t offset, int size)
{
    if (size != 1 && size != 2 && size != 4) {
        return false;
    }
    uint32_t reg = offset & ~3u;
    return offset < RCC_LENGTH &&
           offset % (uint32_t)size == 0 &&
           (reg == RCC_AHB1ENR_OFFSET || reg == RCC_APB1ENR_OFFSET ||
            reg == RCC_APB2ENR_OFFSET || reg == RCC_CSR_OFFSET);
}

static uint32_t width_mask(int size)
{
    return size == 4 ? UINT32_MAX : (1u << (8u * (unsigned)size)) - 1u;
}

static uint32_t *reg_ptr(rcc_t *rcc, uint32_t reg)
{
    switch (reg) {
    case RCC_AHB1ENR_OFFSET: return &rcc->ahb1enr;
    case RCC_APB1ENR_OFFSET: return &rcc->apb1enr;
    case RCC_APB2ENR_OFFSET: return &rcc->apb2enr;
    default: return &rcc->csr;
    }
}

static uint32_t rcc_read(void *ctx, uint32_t offset, int size)
{
    rcc_t *rcc = ctx;
    if (!valid_access(offset, size)) {
        rcc->bad_accesses++;
        return 0;
    }
    return (*reg_ptr(rcc, offset & ~3u) >> (8u * (offset & 3u))) & width_mask(size);
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
    uint32_t reg = offset & ~3u;
    if (reg == RCC_CSR_OFFSET) {
        /* LSION is RW and LSIRDY follows it at once (no start-up time);
         * RMVF clears every reset flag. */
        uint32_t next = (rcc->csr & ~(mask & RCC_CSR_LSION)) | (command & RCC_CSR_LSION);
        if (command & RCC_CSR_RMVF) next &= ~CSR_RESET_FLAGS;
        next = (next & ~RCC_CSR_LSIRDY) | ((next & RCC_CSR_LSION) ? RCC_CSR_LSIRDY : 0u);
        rcc->csr = next;
        return;
    }
    uint32_t writable = reg == RCC_AHB1ENR_OFFSET ? AHB1ENR_WRITABLE
                      : reg == RCC_APB1ENR_OFFSET ? APB1ENR_WRITABLE : APB2ENR_WRITABLE;
    uint32_t *target = reg_ptr(rcc, reg);
    *target = ((*target & ~mask) | command) & writable;
}

rcc_t *rcc_new(void)
{
    rcc_t *rcc = calloc(1, sizeof(rcc_t));
    if (rcc) rcc_reset(rcc);
    return rcc;
}

void rcc_reset(rcc_t *rcc)
{
    rcc->ahb1enr = 0;
    rcc->apb1enr = 0;
    rcc->apb2enr = 0;
    rcc->csr = RCC_CSR_PORRSTF | RCC_CSR_PINRSTF; /* RM0390 6.3.21: reset 0x0E00 0000 */
    rcc->bad_accesses = 0;
}

void rcc_system_reset(rcc_t *rcc, uint32_t flags)
{
    uint32_t kept = rcc->csr & CSR_RESET_FLAGS;
    rcc->ahb1enr = rcc->apb1enr = rcc->apb2enr = 0;
    rcc->csr = kept | (flags & CSR_RESET_FLAGS) | RCC_CSR_PINRSTF;
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

bool rcc_apb1_clock_enabled(const rcc_t *rcc, unsigned bit)
{
    return bit < 32u && (rcc->apb1enr & (UINT32_C(1) << bit)) != 0;
}

bool rcc_apb2_clock_enabled(const rcc_t *rcc, unsigned bit)
{
    return bit < 32u && (rcc->apb2enr & (UINT32_C(1) << bit)) != 0;
}

bool rcc_lsi_on(const rcc_t *rcc) { return (rcc->csr & RCC_CSR_LSION) != 0; }

uint64_t rcc_bad_access_count(const rcc_t *rcc)
{
    return rcc->bad_accesses;
}
