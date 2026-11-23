#include "dma.h"

#include <stdlib.h>

typedef struct {
    uint32_t cr, ndtr, par, m0ar, m1ar, fcr;
    uint32_t initial_ndtr, cur_p, cur_m; /* internal copies latched on enable */
    dma_request_fn request[8];
    void *request_ctx[8];
} stream_t;

struct dma {
    unsigned index;
    const rcc_t *rcc;
    bus_t *bus;
    stream_t s[8];
    uint32_t isr[2];
    dma_irq_fn irq;
    void *irq_ctx;
    dma_stats_t stats;
};

/* Flag offset of stream x inside LISR/HISR: 0, 6, 16, 22. */
static unsigned flag_shift(unsigned x) { static const unsigned sh[4] = {0, 6, 16, 22}; return sh[x % 4u]; }
#define FLAG_TEIF 3u
#define FLAG_HTIF 4u
#define FLAG_TCIF 5u

static bool clock_on(const dma_t *d)
{
    return rcc_ahb1_clock_enabled(d->rcc, d->index == 1u ? RCC_DMA1_ENABLE_BIT : RCC_DMA2_ENABLE_BIT);
}

static void set_flag(dma_t *d, unsigned x, unsigned flag)
{
    d->isr[x / 4u] |= 1u << (flag_shift(x) + flag);
    stream_t *st = &d->s[x];
    bool enabled_irq = (flag == FLAG_TCIF && (st->cr & DMA_CR_TCIE)) ||
                       (flag == FLAG_HTIF && (st->cr & DMA_CR_HTIE)) ||
                       (flag == FLAG_TEIF && (st->cr & DMA_CR_TEIE));
    if (enabled_irq && d->irq) d->irq(d->irq_ctx, x);
}

static unsigned item_size(uint32_t cr, unsigned shift)
{
    unsigned code = (cr >> shift) & 3u;
    return code == 0 ? 1u : code == 1 ? 2u : 4u;
}

/* Validate configuration on enable; reject what this model does not do. */
static bool supported(const dma_t *d, uint32_t cr, uint32_t fcr)
{
    unsigned dir = (cr >> 6) & 3u;
    if (dir == 3u || (dir == 2u && d->index == 1u)) return false;   /* M2M is DMA2 only */
    if (cr & ((1u << 5) | (1u << 18) | (1u << 15) | (0xFu << 21))) return false; /* PFCTRL, DBM, PINCOS, bursts */
    if (fcr & (1u << 2)) return false;                                /* FIFO mode (DMDIS) */
    if (((cr >> 11) & 3u) != ((cr >> 13) & 3u)) return false;         /* direct mode: PSIZE == MSIZE */
    if (((cr >> 11) & 3u) == 3u) return false;
    return true;
}

static uint32_t reg_read(dma_t *d, uint32_t offset)
{
    if (offset == 0x00u) return d->isr[0];
    if (offset == 0x04u) return d->isr[1];
    if (offset < 0x10u) return 0; /* IFCRs read as zero */
    unsigned x = (offset - 0x10u) / 0x18u, r = (offset - 0x10u) % 0x18u;
    if (x >= 8u) return 0;
    stream_t *st = &d->s[x];
    switch (r) {
    case 0x00u: return st->cr;
    case 0x04u: return st->ndtr;
    case 0x08u: return st->par;
    case 0x0Cu: return st->m0ar;
    case 0x10u: return st->m1ar;
    default: return st->fcr | (1u << 5); /* FS = 100: FIFO empty (direct mode) */
    }
}

static void reg_write(dma_t *d, uint32_t offset, uint32_t value)
{
    if (offset == 0x08u || offset == 0x0Cu) { d->isr[(offset - 0x08u) / 4u] &= ~(value & 0x0F7D0F7Du); return; }
    if (offset < 0x10u) return;
    unsigned x = (offset - 0x10u) / 0x18u, r = (offset - 0x10u) % 0x18u;
    if (x >= 8u) return;
    stream_t *st = &d->s[x];
    bool enabled = (st->cr & DMA_CR_EN) != 0;
    if (r == 0x00u) {
        if (enabled) {
            /* While enabled, only EN can be cleared (9.5.5); the stream stops. */
            if (!(value & DMA_CR_EN)) st->cr &= ~DMA_CR_EN;
            return;
        }
        st->cr = value & 0x0FFFFFFFu;
        if (value & DMA_CR_EN) {
            if (!supported(d, st->cr, st->fcr) || st->ndtr == 0) {
                d->stats.rejected_config++;
                st->cr &= ~DMA_CR_EN;
                return;
            }
            st->initial_ndtr = st->ndtr;
            st->cur_p = st->par;
            st->cur_m = st->m0ar;
        }
        return;
    }
    if (enabled) return; /* other registers are write-protected while EN=1 */
    switch (r) {
    case 0x04u: st->ndtr = value & 0xFFFFu; break;
    case 0x08u: st->par = value; break;
    case 0x0Cu: st->m0ar = value; break;
    case 0x10u: st->m1ar = value; break;
    default: st->fcr = value & 0xBFu; break;
    }
}

static uint32_t dma_read(void *ctx, uint32_t offset, int size)
{
    dma_t *d = ctx;
    if (size != 4 || (offset & 3u) || offset >= 0xD0u) { d->stats.bad_accesses++; return 0; }
    if (!clock_on(d)) return 0;
    return reg_read(d, offset);
}

static void dma_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    dma_t *d = ctx;
    if (size != 4 || (offset & 3u) || offset >= 0xD0u) { d->stats.bad_accesses++; return; }
    if (!clock_on(d)) { d->stats.clock_gated++; return; }
    reg_write(d, offset, value);
}

static uint32_t bus_get(bus_t *bus, uint32_t address, unsigned size)
{
    return size == 1 ? bus_read8(bus, address) : size == 2 ? bus_read16(bus, address) : bus_read32(bus, address);
}

static void bus_put(bus_t *bus, uint32_t address, unsigned size, uint32_t value)
{
    if (size == 1) bus_write8(bus, address, (uint8_t)value);
    else if (size == 2) bus_write16(bus, address, (uint16_t)value);
    else bus_write32(bus, address, value);
}

static void transfer_one(dma_t *d, unsigned x)
{
    stream_t *st = &d->s[x];
    unsigned dir = (st->cr >> 6) & 3u, size = item_size(st->cr, 11);
    /* P2M: PAR -> M0AR; M2P: M0AR -> PAR; M2M: PAR (source) -> M0AR. */
    uint32_t src = dir == 1u ? st->cur_m : st->cur_p;
    uint32_t dst = dir == 1u ? st->cur_p : st->cur_m;
    bus_put(d->bus, dst, size, bus_get(d->bus, src, size));
    d->stats.items++;
    if (st->cr & DMA_CR_PINC) st->cur_p += size;
    if (st->cr & DMA_CR_MINC) st->cur_m += size;
    st->ndtr--;
    if (st->ndtr == st->initial_ndtr / 2u) set_flag(d, x, FLAG_HTIF);
    if (st->ndtr == 0) {
        d->stats.completed++;
        if (st->cr & DMA_CR_CIRC) {
            st->ndtr = st->initial_ndtr;
            st->cur_p = st->par;
            st->cur_m = st->m0ar;
        } else {
            st->cr &= ~DMA_CR_EN;
        }
        set_flag(d, x, FLAG_TCIF);
    }
}

void dma_tick(dma_t *d)
{
    if (!clock_on(d)) return;
    for (int pl = 3; pl >= 0; --pl) {
        for (unsigned x = 0; x < 8u; ++x) {
            stream_t *st = &d->s[x];
            if (!(st->cr & DMA_CR_EN) || (int)((st->cr >> 16) & 3u) != pl) continue;
            unsigned dir = (st->cr >> 6) & 3u, channel = (st->cr >> 25) & 7u;
            bool request = dir == 2u ||
                (st->request[channel] && st->request[channel](st->request_ctx[channel]));
            if (request) transfer_one(d, x);
        }
    }
}

dma_t *dma_new(unsigned index, const rcc_t *rcc, bus_t *bus)
{
    if ((index != 1u && index != 2u) || !rcc || !bus) return NULL;
    dma_t *d = calloc(1, sizeof *d);
    if (d) { d->index = index; d->rcc = rcc; d->bus = bus; dma_reset(d); }
    return d;
}

void dma_reset(dma_t *d)
{
    for (unsigned x = 0; x < 8u; ++x) {
        stream_t *st = &d->s[x];
        st->cr = st->ndtr = st->par = st->m0ar = st->m1ar = 0;
        st->fcr = 0x21u; /* RM0390: FS=100, FTH=01 */
        st->initial_ndtr = st->cur_p = st->cur_m = 0;
    }
    d->isr[0] = d->isr[1] = 0;
    d->stats = (dma_stats_t){0};
}

void dma_free(dma_t *d) { free(d); }

int dma_connect(dma_t *d, unsigned stream, unsigned channel, dma_request_fn fn, void *ctx)
{
    if (stream >= 8u || channel >= 8u || !fn || d->s[stream].request[channel]) return -1;
    d->s[stream].request[channel] = fn;
    d->s[stream].request_ctx[channel] = ctx;
    return 0;
}

void dma_set_irq(dma_t *d, dma_irq_fn fn, void *ctx) { d->irq = fn; d->irq_ctx = ctx; }
dma_stats_t dma_get_stats(const dma_t *d) { return d->stats; }

periph_t dma_as_periph(dma_t *d)
{
    return (periph_t){.name = d->index == 1u ? "DMA1" : "DMA2",
                      .base = d->index == 1u ? DMA1_BASE : DMA2_BASE, .length = 0x400u,
                      .ctx = d, .read = dma_read, .write = dma_write};
}
