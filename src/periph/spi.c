#include "spi.h"

#include <stdlib.h>

#define CR1_SUPPORTED (SPI_CR1_CPHA | SPI_CR1_CPOL | SPI_CR1_MSTR | (7u << 3) | SPI_CR1_SPE | \
                       SPI_CR1_SSI | SPI_CR1_SSM)
#define CR2_SUPPORTED (SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN | SPI_CR2_RXNEIE | SPI_CR2_TXEIE)

struct spi {
    const rcc_t *rcc;
    const gpio_t *gpiob;
    uint32_t cr1, cr2, sr;
    uint8_t rx;
    spi_device_t device;
    bool attached, selected;
    gpio_t *cs_gpio;
    unsigned cs_pin;
    spi_stats_t stats;
    spi_irq_fn irq;
    void *irq_ctx;
};

static void update_irq(spi_t *s)
{
    if (!s->irq) return;
    if (((s->cr2 & SPI_CR2_RXNEIE) && (s->sr & (SPI_SR_RXNE | SPI_SR_OVR))) ||
        ((s->cr2 & SPI_CR2_TXEIE) && (s->sr & SPI_SR_TXE)))
        s->irq(s->irq_ctx);
}

static bool cs_low(const spi_t *s)
{
    return s->attached && gpio_pin_mode(s->cs_gpio, s->cs_pin) == 1u &&
           !(gpio_get_odr(s->cs_gpio) & (1u << s->cs_pin));
}

static void cs_observer(void *ctx, const char *name, uint16_t old_odr, uint16_t new_odr)
{
    spi_t *s = ctx;
    (void)name; (void)old_odr; (void)new_odr;
    bool low = cs_low(s);
    if (low != s->selected) {
        s->selected = low;
        if (s->device.select) s->device.select(s->device.ctx, low);
    }
}

static void frame(spi_t *s, uint8_t mosi)
{
    bool pins = gpio_pin_is_af(s->gpiob, 3u, 5u) && gpio_pin_is_af(s->gpiob, 4u, 5u) &&
                gpio_pin_is_af(s->gpiob, 5u, 5u);
    uint8_t miso = 0xFFu; /* nothing drives MISO */
    s->stats.frames++;
    if (!pins) s->stats.pin_mismatch_frames++;
    else if (!s->selected) s->stats.unselected_frames++;
    else miso = s->device.transfer(s->device.ctx, mosi);
    if (s->sr & SPI_SR_RXNE) {
        s->sr |= SPI_SR_OVR; /* the new frame is lost; DR keeps the old byte */
        s->stats.overruns++;
    } else {
        s->rx = miso;
        s->sr |= SPI_SR_RXNE;
    }
}

uint32_t spi_peek(const spi_t *s, uint32_t offset)
{
    switch (offset) {
    case SPI_CR1: return s->cr1;
    case SPI_CR2: return s->cr2;
    case SPI_SR: return s->sr;
    case SPI_DR: return s->rx;
    default: return 0;
    }
}

static bool valid(uint32_t offset, int size)
{
    return (size == 2 || size == 4) && !(offset & 3u) && offset <= SPI_DR;
}

static uint32_t spi_read(void *ctx, uint32_t offset, int size)
{
    spi_t *s = ctx;
    if (!valid(offset, size) && !(size == 1 && offset == SPI_DR)) { s->stats.bad_accesses++; return 0; }
    if (!rcc_apb2_clock_enabled(s->rcc, RCC_SPI1_ENABLE_BIT)) return 0;
    uint32_t value = spi_peek(s, offset);
    if (offset == SPI_DR) s->sr &= ~SPI_SR_RXNE;
    /* OVR clears on a DR read followed by an SR read; this model clears it
     * on the SR read after any DR read (the sequence is not tracked). */
    if (offset == SPI_SR && !(s->sr & SPI_SR_RXNE)) s->sr &= ~SPI_SR_OVR;
    return value;
}

static void spi_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    spi_t *s = ctx;
    if (!valid(offset, size) && !(size == 1 && offset == SPI_DR)) { s->stats.bad_accesses++; return; }
    if (!rcc_apb2_clock_enabled(s->rcc, RCC_SPI1_ENABLE_BIT)) { s->stats.clock_gated++; return; }
    switch (offset) {
    case SPI_CR1:
        value &= 0xFFFFu;
        if (value & ~CR1_SUPPORTED) { s->stats.unsupported_config++; return; }
        s->cr1 = value;
        /* Master with SSM=1 and SSI=0 sees NSS low: mode fault (26.3.10). */
        if ((value & SPI_CR1_MSTR) && (value & SPI_CR1_SSM) && !(value & SPI_CR1_SSI)) {
            s->sr |= SPI_SR_MODF;
            s->cr1 &= ~(SPI_CR1_MSTR | SPI_CR1_SPE);
        } else if ((value & SPI_CR1_SPE) && !(value & SPI_CR1_SSM)) {
            s->stats.unsupported_config++; /* hardware NSS is not modeled */
        }
        update_irq(s);
        return;
    case SPI_CR2:
        if ((value & 0xFFu) & ~CR2_SUPPORTED) { s->stats.unsupported_config++; return; }
        s->cr2 = value & CR2_SUPPORTED;
        update_irq(s);
        return;
    case SPI_SR:
        return; /* CRCERR is the only rc_w0 bit; CRC is not modeled */
    default:
        if ((s->cr1 & (SPI_CR1_SPE | SPI_CR1_MSTR)) != (SPI_CR1_SPE | SPI_CR1_MSTR)) {
            s->stats.unsupported_config++;
            return;
        }
        frame(s, (uint8_t)value);
        update_irq(s);
        return;
    }
}

spi_t *spi_new(const rcc_t *rcc, const gpio_t *gpiob)
{
    if (!rcc || !gpiob) return NULL;
    spi_t *s = calloc(1, sizeof *s);
    if (s) { s->rcc = rcc; s->gpiob = gpiob; spi_reset(s); }
    return s;
}

void spi_reset(spi_t *s)
{
    s->cr1 = s->cr2 = 0;
    s->sr = SPI_SR_TXE;
    s->rx = 0;
    s->stats = (spi_stats_t){0};
    if (s->selected && s->device.select) s->device.select(s->device.ctx, false);
    s->selected = false;
}

void spi_free(spi_t *s) { free(s); }

int spi_attach(spi_t *s, spi_device_t device, gpio_t *cs_gpio, unsigned pin)
{
    if (s->attached || !device.transfer || !cs_gpio || pin >= 16u) return -1;
    if (!gpio_add_observer(cs_gpio, cs_observer, s)) return -1;
    s->device = device;
    s->cs_gpio = cs_gpio;
    s->cs_pin = pin;
    s->attached = true;
    return 0;
}

void spi_set_irq(spi_t *s, spi_irq_fn fn, void *ctx) { s->irq = fn; s->irq_ctx = ctx; }

bool spi_dma_rx_request(void *ctx)
{
    spi_t *s = ctx;
    return (s->cr2 & SPI_CR2_RXDMAEN) && (s->sr & SPI_SR_RXNE);
}

bool spi_dma_tx_request(void *ctx)
{
    spi_t *s = ctx;
    return (s->cr2 & SPI_CR2_TXDMAEN) && (s->sr & SPI_SR_TXE) &&
           (s->cr1 & (SPI_CR1_SPE | SPI_CR1_MSTR)) == (SPI_CR1_SPE | SPI_CR1_MSTR);
}

spi_stats_t spi_get_stats(const spi_t *s) { return s->stats; }

periph_t spi_as_periph(spi_t *s)
{
    return (periph_t){.name = "SPI1", .base = SPI1_BASE, .length = 0x400u,
                      .ctx = s, .read = spi_read, .write = spi_write};
}
