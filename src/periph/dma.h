/* STM32F446 DMA1/DMA2 controller (RM0390 Rev9 section 9): eight streams,
 * direct mode, peripheral-to-memory, memory-to-peripheral and
 * memory-to-memory (DMA2 only), normal and circular mode, TC/HT flags and
 * interrupts. Timing: each enabled stream whose request is active moves ONE
 * data item per emulator step, in stream priority order (PL, then lower
 * stream number). No FIFO/burst/double-buffer, no arbitration latency.
 */
#ifndef PERIPH_DMA_H
#define PERIPH_DMA_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "rcc.h"

#define DMA1_BASE 0x40026000u
#define DMA2_BASE 0x40026400u
#define DMA_SxCR(x)   (0x10u + 0x18u * (x))
#define DMA_SxNDTR(x) (0x14u + 0x18u * (x))
#define DMA_SxPAR(x)  (0x18u + 0x18u * (x))
#define DMA_SxM0AR(x) (0x1Cu + 0x18u * (x))
#define DMA_SxM1AR(x) (0x20u + 0x18u * (x))
#define DMA_SxFCR(x)  (0x24u + 0x18u * (x))
#define DMA_CR_EN    (1u << 0)
#define DMA_CR_TEIE  (1u << 2)
#define DMA_CR_HTIE  (1u << 3)
#define DMA_CR_TCIE  (1u << 4)
#define DMA_CR_CIRC  (1u << 8)
#define DMA_CR_PINC  (1u << 9)
#define DMA_CR_MINC  (1u << 10)

typedef struct dma dma_t;
typedef bool (*dma_request_fn)(void *ctx);
typedef void (*dma_irq_fn)(void *ctx, unsigned stream);

typedef struct {
    uint64_t items, completed, rejected_config, bad_accesses, clock_gated;
} dma_stats_t;

/* index 1 or 2. bus is borrowed: transfers use normal bus accesses, so a
 * peripheral DR access by DMA has the same side effects as a CPU access. */
dma_t *dma_new(unsigned index, const rcc_t *rcc, bus_t *bus);
void dma_free(dma_t *dma);
void dma_reset(dma_t *dma);
periph_t dma_as_periph(dma_t *dma);
/* Connect a request line: stream x, channel CHSEL. A stream only transfers
 * when CHSEL in SxCR matches the connected channel. */
int dma_connect(dma_t *dma, unsigned stream, unsigned channel, dma_request_fn fn, void *ctx);
void dma_set_irq(dma_t *dma, dma_irq_fn fn, void *ctx);
/* One emulator step. */
void dma_tick(dma_t *dma);
dma_stats_t dma_get_stats(const dma_t *dma);

#endif
