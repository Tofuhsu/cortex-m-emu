#include "bme280.h"

#include <stdlib.h>
#include <string.h>

/* Bosch BST-BME280-DS001-24, 4.2.3, 5.3-5.4, 6.2.
 * Chosen SYNTHETIC calibration, not a physical chip or a datasheet example:
 * T1=24000 T2=25600 T3=0; P1=31250 P2..P9=0;
 * H1=0 H2=256 H3=0 H4=256 H5=H6=0.
 * These make inversion demonstrably monotone over the entire raw domain:
 * T increases; P decreases linearly; H increases with saturation.
 * No claim that arbitrary real-chip calibration is globally monotone.
 */
struct bme280 {
    uint8_t address, reg[256], pointer, active_hum, snapshot[8];
    bool expect_register;
    int32_t temperature;
    uint32_t humidity, pressure;
};

static int64_t floor_div(int64_t n, int64_t d)
{
    return n / d - (n < 0 && n % d != 0);
}

/* Algebraic specialization of Bosch integer formulas for OUR calibration.
 * Tests use the full formulas and calibration read through I2C, independently.
 */
static int64_t evaluate(unsigned kind, uint32_t raw)
{
    if (kind == 0) {
        int64_t fine = floor_div(((int64_t)(raw >> 3) - 48000) * 25600, 2048);
        return floor_div(fine * 5 + 128, 256);
    }
    if (kind == 1) return (INT64_C(1048576) - raw) * 256 / 5;
    int64_t humidity = floor_div((int64_t)raw - 16383, 2) * 8;
    return humidity < 0 ? 0 : humidity > 102400 ? 102400 : humidity;
}

static uint32_t nearest(unsigned kind, int64_t target)
{
    uint32_t lo = 0, hi = kind == 2 ? 65535u : 1048575u;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        int64_t value = evaluate(kind, mid);
        if ((kind == 1 && value > target) || (kind != 1 && value < target)) lo = mid + 1u;
        else hi = mid;
    }
    if (lo && llabs(evaluate(kind, lo - 1u) - target) < llabs(evaluate(kind, lo) - target)) --lo;
    return lo;
}

static void pack20(uint8_t *p, uint32_t raw)
{
    p[0] = (uint8_t)(raw >> 12); p[1] = (uint8_t)(raw >> 4); p[2] = (uint8_t)(raw << 4);
}

static void measure(bme280_t *b)
{
    pack20(&b->reg[0xFA], (b->reg[0xF4] & 0xE0u) ? nearest(0, b->temperature) : 0x80000u);
    pack20(&b->reg[0xF7], (b->reg[0xF4] & 0x1Cu) ? nearest(1, (int64_t)b->pressure * 256) : 0x80000u);
    uint32_t hum = b->active_hum ? nearest(2, b->humidity) : 0x8000u;
    b->reg[0xFD] = (uint8_t)(hum >> 8); b->reg[0xFE] = (uint8_t)hum;
    /* measuring/im_update complete synchronously. No filtering or conversion time. */
    b->reg[0xF3] = 0;
}

static void changed(bme280_t *b)
{
    if ((b->reg[0xF4] & 3u) == 3u) measure(b);
}

bool bme280_set_temperature(bme280_t *b, int32_t value)
{
    if (value < -4000 || value > 8500) return false;
    b->temperature = value; changed(b); return true;
}

bool bme280_set_humidity(bme280_t *b, uint32_t value)
{
    if (value > 102400u) return false;
    b->humidity = value; changed(b); return true;
}

bool bme280_set_pressure(bme280_t *b, uint32_t value)
{
    if (value < 30000u || value > 110000u) return false;
    b->pressure = value; changed(b); return true;
}

static void put16(bme280_t *b, unsigned address, uint16_t value)
{
    b->reg[address] = (uint8_t)value; b->reg[address + 1u] = (uint8_t)(value >> 8);
}

void bme280_reset(bme280_t *b)
{
    memset(b->reg, 0, sizeof b->reg);
    b->reg[0xD0] = 0x60;
    put16(b, 0x88, 24000); put16(b, 0x8A, 25600); put16(b, 0x8E, 31250);
    put16(b, 0xE1, 256); b->reg[0xE4] = 0x10; /* H4=256, signed 12 bits */
    b->active_hum = b->pointer = 0;
    b->expect_register = true;
    measure(b); /* disabled channels return documented sentinel values */
    memcpy(b->snapshot, b->reg + 0xF7, sizeof b->snapshot);
}

static void start_device(void *ctx, bool reading)
{
    bme280_t *b = ctx;
    if (!reading) b->expect_register = true;
    else memcpy(b->snapshot, b->reg + 0xF7, sizeof b->snapshot);
}

static bool write_device(void *ctx, uint8_t byte)
{
    bme280_t *b = ctx;
    if (b->expect_register) { b->pointer = byte; b->expect_register = false; return true; }
    uint8_t reg = b->pointer;
    /* I2C multi-write sends register/data pairs; reads alone auto-increment. */
    b->expect_register = true;
    if (reg == 0xE0 && byte == 0xB6) bme280_reset(b);
    else if (reg == 0xF2) b->reg[reg] = byte & 7u;
    else if (reg == 0xF4) {
        b->reg[reg] = byte;
        b->active_hum = b->reg[0xF2];
        if (byte & 3u) {
            measure(b);
            if ((byte & 3u) != 3u) b->reg[reg] &= ~3u; /* forced -> sleep */
        }
    } else if (reg == 0xF5 && !(b->reg[0xF4] & 3u)) b->reg[reg] = byte & 0xFDu;
    return true; /* read-only writes ACK but do not modify calibration/id */
}

static uint8_t read_device(void *ctx, bool ack)
{
    bme280_t *b = ctx;
    (void)ack;
    uint8_t reg = b->pointer++;
    return reg >= 0xF7u && reg <= 0xFEu ? b->snapshot[reg - 0xF7u] : b->reg[reg];
}

static void stop_device(void *ctx) { ((bme280_t *)ctx)->expect_register = true; }

bme280_t *bme280_new(uint8_t address)
{
    if (address != 0x76u && address != 0x77u) return NULL;
    bme280_t *b = calloc(1, sizeof *b);
    if (b) {
        b->address = address; b->temperature = 2500; b->humidity = 51200; b->pressure = 101325;
        bme280_reset(b);
    }
    return b;
}

void bme280_free(bme280_t *b) { free(b); }
uint8_t bme280_peek(const bme280_t *b, uint8_t reg) { return b->reg[reg]; }
i2c_device_t bme280_as_device(bme280_t *b)
{
    return (i2c_device_t){b->address, b, start_device, write_device, read_device, stop_device};
}
