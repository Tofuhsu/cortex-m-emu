#include "i2c.h"

#include <stdlib.h>
#include <string.h>

/* RM0390 24.3.3 master receiver and 24.6 register descriptions.
 * SB: START -> set; SR1 read observing SB then DR write -> clear.
 * ADDR: acknowledged address -> set; SR1 read then SR2 -> clear (see 24.6.7).
 * TXE/BTF: successful immediate write -> both set; next DR write -> clear/refill.
 * RXNE: at least one receive token; BTF: two tokens (DR + shift register).
 * AF: address/data NACK -> set; SR1 write-zero or PE=0 -> clear.
 * Receiver data is obtained from device.read at DR consumption, not prefetched:
 * the two tokens latch ACK decisions while avoiding speculative device reads.
 * This preserves 1/2/N-byte ACK/POS/STOP sequences without fabricating time.
 */
struct i2c {
    const rcc_t *rcc;
    const gpio_t *gpio;
    uint32_t cr1, cr2, oar1, oar2, sr1, sr2, ccr, trise, fltr;
    uint32_t sr1_seen;
    bool sr1_read;
    i2c_device_t devices[8];
    unsigned device_count;
    i2c_device_t *selected;
    bool reading, receiving, rx_end, stop_requested;
    bool rx_ack[2];
    unsigned rx_count;
    i2c_stats_t stats;
    i2c_observer_fn observer;
    void *observer_ctx;
};

static void emit(i2c_t *s, i2c_event_kind_t kind, uint8_t value,
                 bool ack, bool repeated)
{
    if (s->observer) s->observer(s->observer_ctx,
        (i2c_event_t){kind, value, ack, s->reading, repeated});
}

static void finish(i2c_t *s)
{
    if (s->selected && s->selected->stop) s->selected->stop(s->selected->ctx);
    if (s->sr2 & 2u) emit(s, I2C_EVENT_STOP, 0, false, false);
    s->selected = NULL;
    s->sr2 = 0;
    s->sr1 &= I2C_AF;
    s->sr1_seen = 0;
    s->sr1_read = false;
    s->cr1 &= ~(I2C_START | I2C_STOP);
    s->reading = s->receiving = s->stop_requested = false;
    s->rx_count = 0;
    s->rx_end = false;
}

static void rx_flags(i2c_t *s)
{
    s->sr1 &= ~(I2C_RXNE | I2C_BTF);
    if (s->rx_count) s->sr1 |= I2C_RXNE;
    if (s->rx_count == 2u) s->sr1 |= I2C_BTF;
}

static void refill(i2c_t *s)
{
    while (s->receiving && !s->rx_end && !s->stop_requested && s->rx_count < 2u) {
        bool ack = (s->cr1 & I2C_ACK) != 0;
        s->rx_ack[s->rx_count++] = ack;
        if (!ack) s->rx_end = true;
    }
    rx_flags(s);
}

static bool pins_ready(const i2c_t *s)
{
    return rcc_ahb1_clock_enabled(s->rcc, RCC_GPIOB_ENABLE_BIT) &&
           gpio_pin_is_af(s->gpio, 8, 4) && gpio_pin_is_af(s->gpio, 9, 4) &&
           gpio_pin_is_open_drain(s->gpio, 8) && gpio_pin_is_open_drain(s->gpio, 9);
}

static void start(i2c_t *s)
{
    if (!pins_ready(s)) {
        ++s->stats.pin_mismatch_starts;
        s->cr1 &= ~I2C_START;
        return;
    }
    bool repeated = (s->sr2 & 2u) != 0;
    s->sr1 = (s->sr1 & I2C_AF) | I2C_SB;
    s->sr2 = 3u; /* MSL | BUSY */
    s->sr1_seen = 0;
    s->rx_count = 0;
    s->receiving = s->rx_end = s->stop_requested = false;
    emit(s, I2C_EVENT_START, 0, false, repeated);
}

static void clear_addr(i2c_t *s)
{
    s->sr1 &= ~I2C_ADDR;
    s->sr1_seen = 0;
    if (!s->reading) {
        s->sr1 |= I2C_TXE;
        return;
    }
    s->receiving = true;
    if (s->cr1 & I2C_POS) {
        /* Two-byte receive: POS moves the NACK to the second byte. */
        s->rx_count = 2;
        s->rx_ack[0] = true;
        s->rx_ack[1] = (s->cr1 & I2C_ACK) != 0;
        s->rx_end = !s->rx_ack[1];
        rx_flags(s);
    } else refill(s);
}

uint32_t i2c_peek(const i2c_t *s, uint32_t off)
{
    switch (off) {
    case I2C_CR1: return s->cr1;
    case I2C_CR2: return s->cr2;
    case I2C_OAR1: return s->oar1;
    case I2C_OAR2: return s->oar2;
    case I2C_SR1: return s->sr1;
    case I2C_SR2: return s->sr2;
    case I2C_CCR: return s->ccr;
    case I2C_TRISE: return s->trise;
    case I2C_FLTR: return s->fltr;
    default: return 0;
    }
}

static bool valid(uint32_t off, int size)
{
    return (size == 2 || size == 4) && !(off & 3u) && off <= I2C_FLTR;
}

static uint32_t read_reg(void *ctx, uint32_t off, int size)
{
    i2c_t *s = ctx;
    if (!valid(off, size)) { ++s->stats.bad_accesses; return 0; }
    if (!rcc_apb1_clock_enabled(s->rcc, RCC_I2C1_ENABLE_BIT)) return 0;
    uint32_t value = i2c_peek(s, off);
    if (off == I2C_SR1) { s->sr1_seen = value; s->sr1_read = true; }
    if (off == I2C_SR2) {
        /* RM0390 24.6.7: an earlier SR1 read arms ADDR clearing even if
         * ADDR was raised AFTER that read. A peek must never arm this. */
        if (s->sr1_read && (s->sr1 & I2C_ADDR)) clear_addr(s);
        s->sr1_read = false;
    }
    if (off == I2C_DR) {
        if (!s->receiving || !s->rx_count || !s->selected) {
            ++s->stats.protocol_errors;
            return 0;
        }
        bool ack = s->rx_ack[0];
        value = s->selected->read(s->selected->ctx, ack);
        emit(s, I2C_EVENT_RX, (uint8_t)value, ack, false);
        s->rx_ack[0] = s->rx_ack[1];
        --s->rx_count;
        s->sr1_seen = 0;
        refill(s);
        if (s->stop_requested && !s->rx_count) finish(s);
    }
    return value;
}

static void write_dr(i2c_t *s, uint8_t byte)
{
    if (s->sr1 & I2C_SB) {
        if (!(s->sr1_seen & I2C_SB)) { ++s->stats.protocol_errors; return; }
        s->sr1 &= ~I2C_SB;
        s->cr1 &= ~I2C_START;
        s->sr1_seen = 0;
        s->reading = (byte & 1u) != 0;
        s->selected = NULL;
        for (unsigned i = 0; i < s->device_count; ++i)
            if (s->devices[i].address == (byte >> 1)) s->selected = &s->devices[i];
        bool ack = s->selected != NULL;
        emit(s, I2C_EVENT_ADDRESS, byte, ack, false);
        if (!ack) { s->sr1 |= I2C_AF; return; }
        s->selected->start(s->selected->ctx, s->reading);
        s->sr1 |= I2C_ADDR;
        if (s->reading) s->sr2 &= ~4u;
        else s->sr2 |= 4u; /* TRA */
    } else if (s->selected && !s->reading && !(s->sr1 & (I2C_ADDR | I2C_AF))) {
        s->sr1 &= ~(I2C_TXE | I2C_BTF);
        bool ack = s->selected->write(s->selected->ctx, byte);
        emit(s, I2C_EVENT_TX, byte, ack, false);
        s->sr1 |= ack ? I2C_TXE | I2C_BTF : I2C_AF | I2C_TXE;
    } else ++s->stats.protocol_errors;
}

static void write_reg(void *ctx, uint32_t off, uint32_t v, int size)
{
    i2c_t *s = ctx;
    if (!valid(off, size)) { ++s->stats.bad_accesses; return; }
    if (!rcc_apb1_clock_enabled(s->rcc, RCC_I2C1_ENABLE_BIT)) {
        ++s->stats.clock_gated_writes; return;
    }
    v &= 0xFFFFu;
    if (off == I2C_CR1) {
        const uint32_t supported = I2C_PE | I2C_START | I2C_STOP | I2C_ACK | I2C_POS | I2C_SWRST;
        if (v & ~supported) { ++s->stats.bad_accesses; return; }
        if (v & I2C_SWRST) { i2c_reset(s); s->cr1 = I2C_SWRST; return; }
        uint32_t old = s->cr1;
        s->cr1 = v;
        if (!(v & I2C_PE)) {
            if (v & (I2C_START | I2C_STOP)) ++s->stats.pe_disabled_transfers;
            finish(s); s->sr1 = 0; return;
        }
        if (s->receiving && (old & I2C_ACK) && !(v & I2C_ACK) &&
            (v & I2C_POS) && s->rx_count == 2u) {
            s->rx_ack[1] = false;
            s->rx_end = true;
        }
        if (v & I2C_START) start(s);
        if (v & I2C_STOP) {
            s->stop_requested = true;
            if (!s->receiving || !s->rx_count) finish(s);
            else if (!s->rx_end) ++s->stats.protocol_errors;
        }
    } else if (off == I2C_DR) {
        if (!(s->cr1 & I2C_PE)) ++s->stats.pe_disabled_transfers;
        else write_dr(s, (uint8_t)v);
    } else if (off == I2C_SR1) {
        if (!(v & I2C_AF)) s->sr1 &= ~I2C_AF; /* rc_w0 */
    } else if (off == I2C_CR2) {
        if (v & ~0x3Fu) ++s->stats.bad_accesses;
        else s->cr2 = v;
    } else if (off == I2C_OAR1) s->oar1 = v & 0xC3FFu;
    else if (off == I2C_OAR2) s->oar2 = v & 0xFFu;
    else if (off == I2C_CCR) s->ccr = v & 0xCFFFu;
    else if (off == I2C_TRISE) s->trise = v & 0x3Fu;
    else if (off == I2C_FLTR) s->fltr = v & 0x1Fu;
    /* SR2 is read-only. Clock settings are stored, not used to model time. */
}

i2c_t *i2c_new(const rcc_t *rcc, const gpio_t *gpio)
{
    if (!rcc || !gpio) return NULL;
    i2c_t *s = calloc(1, sizeof *s);
    if (s) { s->rcc = rcc; s->gpio = gpio; s->trise = 2; }
    return s;
}

void i2c_reset(i2c_t *s)
{
    finish(s);
    s->cr1 = s->cr2 = s->oar1 = s->oar2 = s->sr1 = s->sr2 = s->ccr = s->fltr = 0;
    s->trise = 2;
    s->stats = (i2c_stats_t){0};
}

void i2c_free(i2c_t *s) { free(s); }

int i2c_attach(i2c_t *s, i2c_device_t dev)
{
    if (dev.address > 0x7Fu || !dev.start || !dev.write || !dev.read || s->device_count == 8u) return -1;
    for (unsigned i = 0; i < s->device_count; ++i)
        if (s->devices[i].address == dev.address) return -1;
    s->devices[s->device_count++] = dev;
    return 0;
}

periph_t i2c_as_periph(i2c_t *s)
{
    return (periph_t){.name = "I2C1", .base = I2C1_BASE, .length = 0x400u,
                      .ctx = s, .read = read_reg, .write = write_reg};
}

i2c_stats_t i2c_get_stats(const i2c_t *s) { return s->stats; }
void i2c_set_observer(i2c_t *s, i2c_observer_fn fn, void *ctx)
{
    s->observer = fn; s->observer_ctx = ctx;
}
