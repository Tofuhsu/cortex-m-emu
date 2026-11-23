#include "flash.h"

#include <stdlib.h>
#include <string.h>

#define KEY1 0x45670123u
#define KEY2 0xCDEF89ABu
#define SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

struct flash_if {
    bus_t *bus;
    uint32_t acr, sr, cr;
    unsigned key_stage;  /* 0: expect KEY1, 1: expect KEY2 */
    bool key_error;      /* wrong key: locked until reset (3.5.1) */
    flash_stats_t stats;
};

bool flash_sector_of(uint32_t offset, unsigned *sector, uint32_t *start, uint32_t *size)
{
    static const uint32_t starts[9] = {0, 0x4000u, 0x8000u, 0xC000u, 0x10000u,
                                      0x20000u, 0x40000u, 0x60000u, 0x80000u};
    for (unsigned i = 0; i < 8u; ++i) {
        if (offset >= starts[i] && offset < starts[i + 1]) {
            *sector = i; *start = starts[i]; *size = starts[i + 1] - starts[i];
            return true;
        }
    }
    return false;
}

static void erase(flash_if_t *f)
{
    uint8_t *data = bus_flash_data(f->bus);
    if (f->cr & FLASH_CR_MER) {
        memset(data, 0xFF, FLASH_SIZE);
        f->stats.erased_sectors += 8u;
    } else {
        unsigned snb = (f->cr >> 3) & 0xFu;
        if (snb >= 8u) { f->sr |= FLASH_SR_WRPERR; f->stats.errors++; return; }
        static const uint32_t starts[9] = {0, 0x4000u, 0x8000u, 0xC000u, 0x10000u,
                                          0x20000u, 0x40000u, 0x60000u, 0x80000u};
        memset(data + starts[snb], 0xFF, starts[snb + 1] - starts[snb]);
        f->stats.erased_sectors++;
    }
    if (f->cr & FLASH_CR_EOPIE) f->sr |= FLASH_SR_EOP; /* EOP needs EOPIE (3.7.5) */
}

static bool program(void *ctx, uint32_t offset, uint32_t value, int size)
{
    flash_if_t *f = ctx;
    if ((f->cr & FLASH_CR_LOCK) || !(f->cr & FLASH_CR_PG) || (f->cr & (FLASH_CR_SER | FLASH_CR_MER))) {
        f->sr |= FLASH_SR_PGSERR; /* write without PG: sequence error, data ignored */
        f->stats.errors++;
        return true;
    }
    unsigned psize = (f->cr >> 8) & 3u;
    if (psize == 3u || size != (1 << psize)) {
        f->sr |= FLASH_SR_PGPERR; /* access width differs from PSIZE */
        f->stats.errors++;
        return true;
    }
    if (offset % (uint32_t)size) { f->sr |= FLASH_SR_PGAERR; f->stats.errors++; return true; }
    uint8_t *data = bus_flash_data(f->bus) + offset;
    bool raises_bits = false; /* would need a 0 -> 1 change: not possible without erase */
    for (int i = 0; i < size; ++i) {
        uint8_t byte = (uint8_t)(value >> (8 * i));
        if ((uint8_t)(byte & ~data[i]) != 0) raises_bits = true;
        data[i] &= byte;
    }
    if (raises_bits) f->stats.overwrite_nonerased++;
    f->stats.programmed_bytes += (uint64_t)size;
    if (f->cr & FLASH_CR_EOPIE) f->sr |= FLASH_SR_EOP;
    return true;
}

static uint32_t flash_read(void *ctx, uint32_t offset, int size)
{
    flash_if_t *f = ctx;
    if (size != 4 || (offset & 3u)) { f->stats.bad_accesses++; return 0; }
    switch (offset) {
    case FLASH_ACR: return f->acr;
    case FLASH_SR: return f->sr;
    case FLASH_CR: return f->cr;
    case 0x14u: return 0x0FFFAAEDu; /* OPTCR reset value; option bytes not modeled */
    default: return 0;
    }
}

static void flash_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    flash_if_t *f = ctx;
    if (size != 4 || (offset & 3u)) { f->stats.bad_accesses++; return; }
    switch (offset) {
    case FLASH_ACR: f->acr = value & 0x1F0Fu; return; /* latency/caches: stored only */
    case FLASH_KEYR:
        if (!(f->cr & FLASH_CR_LOCK) || f->key_error) return;
        if (f->key_stage == 0 && value == KEY1) f->key_stage = 1;
        else if (f->key_stage == 1 && value == KEY2) { f->cr &= ~FLASH_CR_LOCK; f->key_stage = 0; }
        else f->key_error = true; /* bus error in hardware; locked until reset */
        return;
    case FLASH_SR:
        f->sr &= ~(value & (FLASH_SR_EOP | SR_ERRORS)); /* rc_w1 */
        return;
    case FLASH_CR:
        if (f->cr & FLASH_CR_LOCK) {
            if (value & FLASH_CR_LOCK) return;
            f->stats.errors++; /* writes while locked are ignored */
            return;
        }
        if (value & FLASH_CR_LOCK) { f->cr |= FLASH_CR_LOCK; return; }
        f->cr = value & (FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_MER | (0xFu << 3) | (3u << 8) |
                         FLASH_CR_EOPIE | (1u << 25));
        if ((value & FLASH_CR_STRT) && (f->cr & (FLASH_CR_SER | FLASH_CR_MER))) erase(f);
        return;
    default:
        f->stats.bad_accesses++;
        return;
    }
}

flash_if_t *flash_if_new(bus_t *bus)
{
    if (!bus) return NULL;
    flash_if_t *f = calloc(1, sizeof *f);
    if (f) {
        f->bus = bus;
        flash_if_reset(f);
        bus_set_flash_write_handler(bus, program, f);
    }
    return f;
}

void flash_if_reset(flash_if_t *f)
{
    f->acr = 0;
    f->sr = 0;
    f->cr = FLASH_CR_LOCK;
    f->key_stage = 0;
    f->key_error = false;
}

void flash_if_free(flash_if_t *f)
{
    if (f) bus_set_flash_write_handler(f->bus, NULL, NULL);
    free(f);
}

periph_t flash_if_as_periph(flash_if_t *f)
{
    return (periph_t){.name = "FLASH", .base = FLASH_IF_BASE, .length = 0x400u,
                      .ctx = f, .read = flash_read, .write = flash_write};
}

flash_stats_t flash_if_get_stats(const flash_if_t *f) { return f->stats; }
