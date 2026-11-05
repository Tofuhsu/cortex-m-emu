/* Minimal STM32F446 GPIOA register model; no electrical pin simulation. */
#ifndef PERIPH_GPIO_H
#define PERIPH_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "rcc.h"

#define GPIOA_BASE          0x40020000u
#define GPIOB_BASE          0x40020400u
#define GPIO_MODER_OFFSET   0x00u
#define GPIO_OTYPER_OFFSET  0x04u
#define GPIO_OSPEEDR_OFFSET 0x08u
#define GPIO_PUPDR_OFFSET   0x0Cu
#define GPIO_IDR_OFFSET     0x10u
#define GPIO_ODR_OFFSET     0x14u
#define GPIO_BSRR_OFFSET    0x18u
#define GPIO_AFRL_OFFSET    0x20u
#define GPIO_AFRH_OFFSET    0x24u

typedef struct gpio gpio_t;

/* Accepts GPIOA/B with the corresponding RCC enable bit.
 * The name is copied. rcc is borrowed and must outlive the GPIO model.
 * Returns NULL on allocation failure or unsupported arguments.
 */
gpio_t *gpio_new(const char *name, uint32_t base,
                 const rcc_t *rcc, unsigned enable_bit);
void gpio_reset(gpio_t *gpio);
void gpio_free(gpio_t *gpio);
periph_t gpio_as_periph(gpio_t *gpio);

/* Host-side inspection bypasses clock gating. */
uint16_t gpio_get_odr(const gpio_t *gpio);
/* Inspect saved MODER/AFRL/AFRH, bypassing clock gating. Pins 0-15;
 * unsupported pins/AF numbers return false.
 */
bool gpio_pin_is_open_drain(const gpio_t *gpio, unsigned pin);
bool gpio_pin_is_af(const gpio_t *gpio, unsigned pin, unsigned af);
uint64_t gpio_gated_write_count(const gpio_t *gpio);
uint64_t gpio_bad_access_count(const gpio_t *gpio);

/* IDR is explicitly injected, initially zero. It does not mirror ODR.
 * Pull resistors, output drivers and external circuits are not simulated.
 */
void gpio_set_inputs(gpio_t *gpio, uint16_t levels);

typedef void (*gpio_odr_change_fn)(void *ctx, const char *name,
                                   uint16_t old_odr, uint16_t new_odr);
void gpio_set_observer(gpio_t *gpio, gpio_odr_change_fn callback, void *ctx);

#endif
