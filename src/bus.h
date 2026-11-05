/*
 * System bus: maps addresses to flash, SRAM and memory-mapped peripherals.
 * Memory layout follows the STM32F446RE (also matches the STM32F411CE).
 */
#ifndef BUS_H
#define BUS_H

#include <stdbool.h>
#include <stdint.h>

#define FLASH_BASE 0x08000000u
#define FLASH_SIZE 0x00080000u /* 512 KiB */
#define SRAM_BASE  0x20000000u
#define SRAM_SIZE  0x00020000u /* 128 KiB */

#define BUS_MAX_PERIPHS 32

/*
 * A memory-mapped peripheral. `offset` is relative to `base`; `size` is the
 * access width in bytes (1, 2 or 4).
 */
typedef struct {
    const char *name;
    uint32_t base;
    uint32_t length;
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t offset, int size);
    void (*write)(void *ctx, uint32_t offset, uint32_t value, int size);
} periph_t;

typedef struct bus bus_t;

bus_t *bus_new(void);
void bus_free(bus_t *bus);

int bus_add_periph(bus_t *bus, periph_t periph);

/* Copy an image into flash, bypassing the read-only check. Returns 0 on success. */
int bus_load_flash(bus_t *bus, uint32_t offset, const uint8_t *data, uint32_t len);

uint32_t bus_read32(bus_t *bus, uint32_t addr);
uint16_t bus_read16(bus_t *bus, uint32_t addr);
uint8_t bus_read8(bus_t *bus, uint32_t addr);
void bus_write32(bus_t *bus, uint32_t addr, uint32_t value);
void bus_write16(bus_t *bus, uint32_t addr, uint16_t value);
void bus_write8(bus_t *bus, uint32_t addr, uint8_t value);

/*
 * A fault is a bad access (write to flash, address outside every region).
 * The CPU should check this after each instruction and stop.
 */
bool bus_fault(const bus_t *bus);
const char *bus_fault_msg(const bus_t *bus);

/*
 * Debugger access to one byte of flash or SRAM: no peripheral side effects,
 * no fault, and flash is writable (like a programmer). Returns false for any
 * other address.
 */
bool bus_debug_byte(bus_t *bus, uint32_t addr, uint8_t *byte, bool write);

/* Accesses to the peripheral/system region with no model registered. */
uint32_t bus_unmapped_count(const bus_t *bus);
uint32_t bus_last_unmapped_addr(const bus_t *bus);

#endif
