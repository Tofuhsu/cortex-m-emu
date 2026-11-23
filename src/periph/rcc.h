/* Minimal STM32F446 RCC: AHB1/APB1/APB2 peripheral clock enables and CSR
 * (LSI on/ready and reset flags). Clock tree and PLL are not modeled. */
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
#define RCC_GPIOC_ENABLE_BIT 2u
#define RCC_DMA1_ENABLE_BIT 21u
#define RCC_DMA2_ENABLE_BIT 22u
#define RCC_APB2ENR_OFFSET   0x44u
#define RCC_CSR_OFFSET       0x74u
#define RCC_ADC1_ENABLE_BIT  8u   /* APB2 */
#define RCC_SPI1_ENABLE_BIT  12u  /* APB2 */
#define RCC_SYSCFG_ENABLE_BIT 14u /* APB2 */
#define RCC_CSR_LSION   (1u << 0)
#define RCC_CSR_LSIRDY  (1u << 1)
#define RCC_CSR_RMVF    (1u << 24)
#define RCC_CSR_PINRSTF (1u << 26)
#define RCC_CSR_PORRSTF (1u << 27)
#define RCC_CSR_SFTRSTF (1u << 28)
#define RCC_CSR_IWDGRSTF (1u << 29)
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
bool rcc_apb2_clock_enabled(const rcc_t *rcc, unsigned bit);
bool rcc_lsi_on(const rcc_t *rcc);
/* System reset: enables cleared, LSION cleared, reset flags ORed with `flags`
 * (RCC_CSR_*RSTF). rcc_reset() is a power-on reset (PORRSTF|PINRSTF). */
void rcc_system_reset(rcc_t *rcc, uint32_t flags);
uint64_t rcc_bad_access_count(const rcc_t *rcc);

#endif
