#include "gpio.h"

#include <stdlib.h>
#include <string.h>

#define GPIO_LENGTH 0x400u

struct gpio {
    char *name;
    uint32_t base;
    const rcc_t *rcc;
    unsigned enable_bit;
    uint32_t moder, otyper, ospeedr, pupdr, afrl, afrh;
    uint16_t idr, odr;
    uint64_t gated_writes, bad_accesses;
    gpio_odr_change_fn observer;
    void *observer_ctx;
};

static bool valid_access(uint32_t offset, int size)
{
    if (size != 1 && size != 2 && size != 4) {
        return false;
    }
    if (offset >= GPIO_LENGTH || offset % (uint32_t)size != 0) {
        return false;
    }
    uint32_t reg = offset & ~3u;
    if (reg > GPIO_BSRR_OFFSET && reg != GPIO_AFRL_OFFSET && reg != GPIO_AFRH_OFFSET) {
        return false;
    }
    /* RM0390 specifies word-only accesses to IDR. */
    return reg != GPIO_IDR_OFFSET || size == 4;
}

static uint32_t width_mask(int size)
{
    return size == 4 ? UINT32_MAX : (1u << (8u * (unsigned)size)) - 1u;
}

static void set_odr(gpio_t *gpio, uint16_t value)
{
    uint16_t old = gpio->odr;
    gpio->odr = value;
    if (old != value && gpio->observer) {
        gpio->observer(gpio->observer_ctx, gpio->name, old, value);
    }
}

static uint32_t gpio_read(void *ctx, uint32_t offset, int size)
{
    gpio_t *gpio = ctx;
    if (!valid_access(offset, size)) {
        gpio->bad_accesses++;
        return 0;
    }
    if (!rcc_ahb1_clock_enabled(gpio->rcc, gpio->enable_bit)) {
        return 0;
    }

    uint32_t value = 0;
    switch (offset & ~3u) {
    case GPIO_MODER_OFFSET:   value = gpio->moder; break;
    case GPIO_OTYPER_OFFSET:  value = gpio->otyper; break;
    case GPIO_OSPEEDR_OFFSET: value = gpio->ospeedr; break;
    case GPIO_PUPDR_OFFSET:   value = gpio->pupdr; break;
    case GPIO_IDR_OFFSET:     value = gpio->idr; break;
    case GPIO_ODR_OFFSET:     value = gpio->odr; break;
    case GPIO_BSRR_OFFSET:    value = 0; break;
    case GPIO_AFRL_OFFSET:    value = gpio->afrl; break;
    case GPIO_AFRH_OFFSET:    value = gpio->afrh; break;
    }
    return (value >> (8u * (offset & 3u))) & width_mask(size);
}

static void gpio_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    gpio_t *gpio = ctx;
    if (!valid_access(offset, size)) {
        gpio->bad_accesses++;
        return;
    }
    if (!rcc_ahb1_clock_enabled(gpio->rcc, gpio->enable_bit)) {
        gpio->gated_writes++;
        return;
    }

    unsigned shift = 8u * (offset & 3u);
    uint32_t mask = width_mask(size) << shift;
    uint32_t command = (value & width_mask(size)) << shift;
    switch (offset & ~3u) {
    case GPIO_MODER_OFFSET:
        gpio->moder = (gpio->moder & ~mask) | command;
        break;
    case GPIO_OTYPER_OFFSET:
        gpio->otyper = ((gpio->otyper & ~mask) | command) & 0xFFFFu;
        break;
    case GPIO_OSPEEDR_OFFSET:
        gpio->ospeedr = (gpio->ospeedr & ~mask) | command;
        break;
    case GPIO_PUPDR_OFFSET:
        gpio->pupdr = (gpio->pupdr & ~mask) | command;
        break;
    case GPIO_AFRL_OFFSET:
        gpio->afrl = (gpio->afrl & ~mask) | command;
        break;
    case GPIO_AFRH_OFFSET:
        gpio->afrh = (gpio->afrh & ~mask) | command;
        break;
    case GPIO_IDR_OFFSET:
        /* Read-only: a valid write has no effect. */
        break;
    case GPIO_ODR_OFFSET:
        set_odr(gpio, (uint16_t)(((uint32_t)gpio->odr & ~mask) | command));
        break;
    case GPIO_BSRR_OFFSET: {
        uint32_t set = command & 0xFFFFu;
        uint32_t reset = command >> 16;
        /* Set wins if the same pin is both set and reset. */
        set_odr(gpio, (uint16_t)(((uint32_t)gpio->odr & ~reset) | set));
        break;
    }
    }
}

gpio_t *gpio_new(const char *name, uint32_t base,
                 const rcc_t *rcc, unsigned enable_bit)
{
    if (!name || !*name || !rcc || !((base == GPIOA_BASE && enable_bit == RCC_GPIOA_ENABLE_BIT) ||
          (base == GPIOB_BASE && enable_bit == RCC_GPIOB_ENABLE_BIT))) {
        return NULL;
    }
    gpio_t *gpio = calloc(1, sizeof *gpio);
    if (!gpio) {
        return NULL;
    }
    gpio->name = malloc(strlen(name) + 1u);
    if (!gpio->name) {
        free(gpio);
        return NULL;
    }
    strcpy(gpio->name, name);
    gpio->base = base;
    gpio->rcc = rcc;
    gpio->enable_bit = enable_bit;
    gpio_reset(gpio);
    return gpio;
}

void gpio_reset(gpio_t *gpio)
{
    gpio->moder = gpio->base == GPIOA_BASE ? 0xA8000000u : 0x00000280u;
    gpio->otyper = 0;
    gpio->ospeedr = gpio->base == GPIOA_BASE ? 0x0C000000u : 0x000000C0u; /* RM0390 Rev 9, section 7.4.3 (GPIOA). */
    gpio->pupdr = gpio->base == GPIOA_BASE ? 0x64000000u : 0x00000100u;
    gpio->afrh = 0;
    gpio->afrl = 0; /* RM0390 7.4.9. */
    gpio->idr = 0; /* Deterministic model default; hardware reset is undefined. */
    gpio->odr = 0;
    gpio->gated_writes = 0;
    gpio->bad_accesses = 0;
    /* Keep the observer, but emit no instruction event during host reset. */
}

void gpio_free(gpio_t *gpio)
{
    if (gpio) {
        free(gpio->name);
        free(gpio);
    }
}

periph_t gpio_as_periph(gpio_t *gpio)
{
    return (periph_t){
        .name = gpio->name, .base = gpio->base, .length = GPIO_LENGTH,
        .ctx = gpio, .read = gpio_read, .write = gpio_write,
    };
}

uint16_t gpio_get_odr(const gpio_t *gpio)
{
    return gpio->odr;
}

bool gpio_pin_is_af(const gpio_t *gpio, unsigned pin, unsigned af)
{
    if (!gpio || pin >= 16u || af >= 16u) {
        return false;
    }
    return ((gpio->moder >> (2u * pin)) & 3u) == 2u &&
           (((pin < 8u ? gpio->afrl : gpio->afrh) >> (4u * (pin % 8u))) & 15u) == af;
}

uint64_t gpio_gated_write_count(const gpio_t *gpio)
{
    return gpio->gated_writes;
}

uint64_t gpio_bad_access_count(const gpio_t *gpio)
{
    return gpio->bad_accesses;
}

void gpio_set_inputs(gpio_t *gpio, uint16_t levels)
{
    gpio->idr = levels;
}

void gpio_set_observer(gpio_t *gpio, gpio_odr_change_fn callback, void *ctx)
{
    gpio->observer = callback;
    gpio->observer_ctx = ctx;
}

bool gpio_pin_is_open_drain(const gpio_t *gpio, unsigned pin)
{
    return gpio && pin < 16u && (gpio->otyper & (1u << pin)) != 0;
}
