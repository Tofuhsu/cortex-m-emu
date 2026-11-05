#include "bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Peripheral region (APB/AHB) and the Private Peripheral Bus (NVIC, SysTick, SCB). */
#define PERIPH_BASE 0x40000000u
#define PERIPH_END  0x60000000u
#define PPB_BASE    0xE0000000u
#define PPB_END     0xE0100000u

struct bus {
    uint8_t flash[FLASH_SIZE];
    uint8_t sram[SRAM_SIZE];
    periph_t periphs[BUS_MAX_PERIPHS];
    int nperiphs;
    bool fault;
    char fault_msg[128];
    uint32_t unmapped_count;
    uint32_t last_unmapped_addr;
};

bus_t *bus_new(void)
{
    return calloc(1, sizeof(bus_t));
}

void bus_free(bus_t *bus)
{
    free(bus);
}

int bus_add_periph(bus_t *bus, periph_t periph)
{
    if (bus->nperiphs == BUS_MAX_PERIPHS) {
        return -1;
    }
    bus->periphs[bus->nperiphs++] = periph;
    return 0;
}

int bus_load_flash(bus_t *bus, uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (offset > FLASH_SIZE || len > FLASH_SIZE - offset) {
        return -1;
    }
    memcpy(bus->flash + offset, data, len);
    return 0;
}

static void set_fault(bus_t *bus, const char *what, uint32_t addr)
{
    if (!bus->fault) {
        bus->fault = true;
        snprintf(bus->fault_msg, sizeof bus->fault_msg, "%s at 0x%08x", what, addr);
    }
}

/*
 * Returns a pointer to backing RAM/flash for [addr, addr+size), or NULL.
 * The STM32 boots from main flash, which is also aliased at 0x00000000;
 * that is where the CPU reads the vector table on reset.
 */
static uint8_t *memory_ptr(bus_t *bus, uint32_t addr, int size, bool *is_flash)
{
    *is_flash = false;
    if (addr < FLASH_SIZE && addr + (uint32_t)size <= FLASH_SIZE) {
        *is_flash = true;
        return bus->flash + addr;
    }
    if (addr >= FLASH_BASE && addr - FLASH_BASE + (uint32_t)size <= FLASH_SIZE) {
        *is_flash = true;
        return bus->flash + (addr - FLASH_BASE);
    }
    if (addr >= SRAM_BASE && addr - SRAM_BASE + (uint32_t)size <= SRAM_SIZE) {
        return bus->sram + (addr - SRAM_BASE);
    }
    return NULL;
}

static periph_t *find_periph(bus_t *bus, uint32_t addr)
{
    for (int i = 0; i < bus->nperiphs; i++) {
        periph_t *p = &bus->periphs[i];
        if (addr >= p->base && addr - p->base < p->length) {
            return p;
        }
    }
    return NULL;
}

static bool in_io_region(uint32_t addr)
{
    return (addr >= PERIPH_BASE && addr < PERIPH_END) || (addr >= PPB_BASE && addr < PPB_END);
}

/* Little-endian: the byte at the lowest address is the least significant. */
static uint32_t read_le(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = size - 1; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void write_le(uint8_t *p, uint32_t v, int size)
{
    for (int i = 0; i < size; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t bus_read(bus_t *bus, uint32_t addr, int size)
{
    bool is_flash;
    uint8_t *mem = memory_ptr(bus, addr, size, &is_flash);
    if (mem) {
        return read_le(mem, size);
    }
    periph_t *p = find_periph(bus, addr);
    if (p && p->read) {
        return p->read(p->ctx, addr - p->base, size);
    }
    if (in_io_region(addr)) {
        bus->unmapped_count++;
        bus->last_unmapped_addr = addr;
        return 0;
    }
    set_fault(bus, "read from unmapped address", addr);
    return 0;
}

static void bus_write(bus_t *bus, uint32_t addr, uint32_t value, int size)
{
    bool is_flash;
    uint8_t *mem = memory_ptr(bus, addr, size, &is_flash);
    if (mem) {
        if (is_flash) {
            set_fault(bus, "write to flash", addr);
            return;
        }
        write_le(mem, value, size);
        return;
    }
    periph_t *p = find_periph(bus, addr);
    if (p && p->write) {
        p->write(p->ctx, addr - p->base, value, size);
        return;
    }
    if (in_io_region(addr)) {
        bus->unmapped_count++;
        bus->last_unmapped_addr = addr;
        return;
    }
    set_fault(bus, "write to unmapped address", addr);
}

uint32_t bus_read32(bus_t *bus, uint32_t addr) { return bus_read(bus, addr, 4); }
uint16_t bus_read16(bus_t *bus, uint32_t addr) { return (uint16_t)bus_read(bus, addr, 2); }
uint8_t bus_read8(bus_t *bus, uint32_t addr) { return (uint8_t)bus_read(bus, addr, 1); }
void bus_write32(bus_t *bus, uint32_t addr, uint32_t value) { bus_write(bus, addr, value, 4); }
void bus_write16(bus_t *bus, uint32_t addr, uint16_t value) { bus_write(bus, addr, value, 2); }
void bus_write8(bus_t *bus, uint32_t addr, uint8_t value) { bus_write(bus, addr, value, 1); }

bool bus_debug_byte(bus_t *bus, uint32_t addr, uint8_t *byte, bool write)
{
    bool is_flash;
    uint8_t *mem = memory_ptr(bus, addr, 1, &is_flash);
    if (!mem) return false;
    if (write) *mem = *byte;
    else *byte = *mem;
    return true;
}

bool bus_fault(const bus_t *bus) { return bus->fault; }
const char *bus_fault_msg(const bus_t *bus) { return bus->fault_msg; }
uint32_t bus_unmapped_count(const bus_t *bus) { return bus->unmapped_count; }
uint32_t bus_last_unmapped_addr(const bus_t *bus) { return bus->last_unmapped_addr; }
