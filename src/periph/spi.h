/* STM32F446 SPI1 as a full-duplex master with 8-bit frames (RM0390 Rev9
 * section 26). A frame completes inside the DR write: TXE never drops, BSY
 * never rises and BR (baud rate) is stored only. Devices are selected by a
 * GPIO output pin (software NSS); SSM=1/SSI=1 is the supported NSS setup.
 * DMA requests: RXNE with RXDMAEN, TXE with TXDMAEN.
 */
#ifndef PERIPH_SPI_H
#define PERIPH_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "gpio.h"
#include "rcc.h"

#define SPI1_BASE 0x40013000u
#define SPI_CR1 0x00u
#define SPI_CR2 0x04u
#define SPI_SR  0x08u
#define SPI_DR  0x0Cu
#define SPI_CR1_CPHA (1u << 0)
#define SPI_CR1_CPOL (1u << 1)
#define SPI_CR1_MSTR (1u << 2)
#define SPI_CR1_SPE  (1u << 6)
#define SPI_CR1_LSBFIRST (1u << 7)
#define SPI_CR1_SSI  (1u << 8)
#define SPI_CR1_SSM  (1u << 9)
#define SPI_CR1_RXONLY (1u << 10)
#define SPI_CR1_DFF  (1u << 11)
#define SPI_CR1_BIDIMODE (1u << 15)
#define SPI_CR2_RXDMAEN (1u << 0)
#define SPI_CR2_TXDMAEN (1u << 1)
#define SPI_CR2_RXNEIE  (1u << 6)
#define SPI_CR2_TXEIE   (1u << 7)
#define SPI_SR_RXNE (1u << 0)
#define SPI_SR_TXE  (1u << 1)
#define SPI_SR_MODF (1u << 5)
#define SPI_SR_OVR  (1u << 6)
#define SPI_SR_BSY  (1u << 7)

typedef struct {
    void *ctx;
    void (*select)(void *ctx, bool selected); /* chip select edge */
    uint8_t (*transfer)(void *ctx, uint8_t mosi); /* one frame while selected */
} spi_device_t;

typedef struct {
    uint64_t clock_gated, pin_mismatch_frames, unselected_frames, overruns;
    uint64_t unsupported_config, bad_accesses, frames;
} spi_stats_t;

typedef struct spi spi_t;
typedef void (*spi_irq_fn)(void *ctx);

/* gpiob is borrowed. SCK/MISO/MOSI must be PB3/PB4/PB5 in AF5 for frames to
 * reach a device; otherwise MISO reads 0xFF and pin_mismatch_frames counts. */
spi_t *spi_new(const rcc_t *rcc, const gpio_t *gpiob);
void spi_free(spi_t *spi);
void spi_reset(spi_t *spi);
periph_t spi_as_periph(spi_t *spi);
/* Attach a device whose active-low chip select is `pin` of `cs_gpio` (output
 * mode). The model watches that pin through a GPIO observer. One device. */
int spi_attach(spi_t *spi, spi_device_t device, gpio_t *cs_gpio, unsigned pin);
void spi_set_irq(spi_t *spi, spi_irq_fn fn, void *ctx);
bool spi_dma_rx_request(void *spi);
bool spi_dma_tx_request(void *spi);
uint32_t spi_peek(const spi_t *spi, uint32_t offset);
spi_stats_t spi_get_stats(const spi_t *spi);

#endif
