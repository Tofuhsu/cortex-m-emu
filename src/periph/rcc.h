/* Minimal STM32F446 RCC: AHB1 and APB1 peripheral clock enable registers. */
#ifndef PERIPH_RCC_H
#define PERIPH_RCC_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

#define RCC_BASE             0x40023800u
#define RCC_AHB1ENR_OFFSET    0x30u
#define RCC_APB1ENR_OFFSET    0x40u
#define RCC_GPIOA_ENABLE_BIT 0u
#define RCC_GPIOB_ENABLE_BIT 1u
/* RM0390 Rev9 6.3.13, p145: APB1ENR offset40, reset0, I2C1EN bit21. */
#define RCC_I2C1_ENABLE_BIT 21u
#define RCC_USART2_ENABLE_BIT 17u

typedef struct rcc rcc_t;

rcc_t *rcc_new(void);
void rcc_reset(rcc_t *rcc);
void rcc_free(rcc_t *rcc);

/* The descriptor borrows rcc; bus_free() does not free it. */
periph_t rcc_as_periph(rcc_t *rcc);
bool rcc_ahb1_clock_enabled(const rcc_t *rcc, unsigned bit);
bool rcc_apb1_clock_enabled(const rcc_t *rcc, unsigned bit);
uint64_t rcc_bad_access_count(const rcc_t *rcc);

#endif
