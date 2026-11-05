#ifndef PERIPH_I2C_H
#define PERIPH_I2C_H

#include "gpio.h"

#define I2C1_BASE 0x40005400u
#define I2C_CR1 0x00u
#define I2C_CR2 0x04u
#define I2C_OAR1 0x08u
#define I2C_OAR2 0x0Cu
#define I2C_DR 0x10u
#define I2C_SR1 0x14u
#define I2C_SR2 0x18u
#define I2C_CCR 0x1Cu
#define I2C_TRISE 0x20u
#define I2C_FLTR 0x24u
#define I2C_PE (1u << 0)
#define I2C_START (1u << 8)
#define I2C_STOP (1u << 9)
#define I2C_ACK (1u << 10)
#define I2C_POS (1u << 11)
#define I2C_SWRST (1u << 15)
#define I2C_SB (1u << 0)
#define I2C_ADDR (1u << 1)
#define I2C_BTF (1u << 2)
#define I2C_RXNE (1u << 6)
#define I2C_TXE (1u << 7)
#define I2C_AF (1u << 10)

typedef struct i2c i2c_t;
typedef struct {
    uint8_t address;
    void *ctx;
    void (*start)(void *ctx, bool read);
    bool (*write)(void *ctx, uint8_t byte);
    uint8_t (*read)(void *ctx, bool ack);
    void (*stop)(void *ctx);
} i2c_device_t;

typedef enum { I2C_EVENT_START, I2C_EVENT_ADDRESS, I2C_EVENT_TX,
               I2C_EVENT_RX, I2C_EVENT_STOP } i2c_event_kind_t;
typedef struct {
    i2c_event_kind_t kind;
    uint8_t value;
    bool ack, read, repeated;
} i2c_event_t;
typedef void (*i2c_observer_fn)(void *ctx, i2c_event_t event);
typedef struct {
    uint64_t clock_gated_writes, pe_disabled_transfers, pin_mismatch_starts;
    uint64_t bad_accesses, protocol_errors;
} i2c_stats_t;

/* Polling, seven-bit master; borrowed RCC/GPIOB and device contexts.
 * GPIOB PB8/PB9 must be AF4, open-drain, with its clock enabled.
 * Halfword/word register accesses only. No interrupts, DMA or bit timing.
 */
i2c_t *i2c_new(const rcc_t *rcc, const gpio_t *gpiob);
void i2c_free(i2c_t *i2c);
void i2c_reset(i2c_t *i2c); /* preserve attachments/observer; abort active transfer */
int i2c_attach(i2c_t *i2c, i2c_device_t device);
periph_t i2c_as_periph(i2c_t *i2c);
uint32_t i2c_peek(const i2c_t *i2c, uint32_t offset); /* no flag side effects */
i2c_stats_t i2c_get_stats(const i2c_t *i2c);
void i2c_set_observer(i2c_t *i2c, i2c_observer_fn observer, void *ctx);

#endif
