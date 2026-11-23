/* STM32F446 embedded-flash interface (RM0390 Rev9 section 3): KEYR unlock
 * sequence, LOCK, PG programming with PSIZE, sector erase (SER/SNB/STRT) and
 * mass erase (MER). Operations complete inside the triggering access: BSY is
 * never observed set. Programming ANDs the written bits into the old content
 * (a cell only goes 1 -> 0); program operations that would need a 0 -> 1
 * change are counted (overwrite_nonerased), one per write access.
 * Sector map (512 KiB device): 0-3 16 KiB, 4 64 KiB, 5-7 128 KiB.
 */
#ifndef PERIPH_FLASH_H
#define PERIPH_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

#define FLASH_IF_BASE 0x40023C00u
#define FLASH_ACR  0x00u
#define FLASH_KEYR 0x04u
#define FLASH_OPTKEYR 0x08u
#define FLASH_SR   0x0Cu
#define FLASH_CR   0x10u
#define FLASH_SR_EOP    (1u << 0)
#define FLASH_SR_OPERR  (1u << 1)
#define FLASH_SR_WRPERR (1u << 4)
#define FLASH_SR_PGAERR (1u << 5)
#define FLASH_SR_PGPERR (1u << 6)
#define FLASH_SR_PGSERR (1u << 7)
#define FLASH_SR_BSY    (1u << 16)
#define FLASH_CR_PG   (1u << 0)
#define FLASH_CR_SER  (1u << 1)
#define FLASH_CR_MER  (1u << 2)
#define FLASH_CR_STRT (1u << 16)
#define FLASH_CR_EOPIE (1u << 24)
#define FLASH_CR_LOCK (1u << 31)

typedef struct flash_if flash_if_t;

typedef struct {
    uint64_t programmed_bytes, erased_sectors, overwrite_nonerased, errors, bad_accesses;
} flash_stats_t;

/* Installs itself as the bus flash-write handler. bus is borrowed. */
flash_if_t *flash_if_new(bus_t *bus);
void flash_if_free(flash_if_t *f);
void flash_if_reset(flash_if_t *f); /* registers only; flash content is kept */
periph_t flash_if_as_periph(flash_if_t *f);
flash_stats_t flash_if_get_stats(const flash_if_t *f);
/* Sector number and size for a flash offset; false past the end. */
bool flash_sector_of(uint32_t offset, unsigned *sector, uint32_t *start, uint32_t *size);

#endif
