#include "adc.h"

#include <stdlib.h>

#define SR_EOC (1u << 1)
#define SR_STRT (1u << 4)
#define CR1_EOCIE (1u << 5)
#define CR2_ADON (1u << 0)
#define CR2_CONT (1u << 1)
#define CR2_SWSTART (1u << 30)
#define CR2_ALIGN (1u << 11)
#define CCR_TSVREFE (1u << 23)
#define VREFINT_MV 1210
#define CAL_VDDA_MV 3300

struct adc {
    const rcc_t *rcc;
    uint32_t sr, cr1, cr2, smpr1, smpr2, sqr1, sqr2, sqr3, dr, ccr;
    int32_t temperature;  /* centi-Celsius */
    uint32_t vdda;        /* mV */
    adc_irq_fn irq;
    void *irq_ctx;
    adc_stats_t stats;
};

static int64_t round_div(int64_t n, int64_t d) { return (n + d / 2) / d; }

/* Sensor voltage in microvolts at a temperature in centi-Celsius. */
static int64_t vsense_uv(int32_t centi) { return 760000 + (int64_t)(centi - 2500) * 25; }

static uint16_t raw_from_uv(int64_t microvolts, uint32_t vdda_mv)
{
    int64_t raw = round_div(microvolts * 4095, (int64_t)vdda_mv * 1000);
    return (uint16_t)(raw < 0 ? 0 : raw > 4095 ? 4095 : raw);
}

uint16_t adc_expected_raw(const adc_t *a, unsigned channel)
{
    if (channel == ADC_CHANNEL_VREFINT) return raw_from_uv(VREFINT_MV * 1000, a->vdda);
    if (channel == ADC_CHANNEL_TEMP) return raw_from_uv(vsense_uv(a->temperature), a->vdda);
    return 0; /* external pins are not modeled */
}

static void convert(adc_t *a)
{
    unsigned channel = a->sqr3 & 0x1Fu;
    a->stats.conversions++;
    uint16_t raw;
    if ((channel == ADC_CHANNEL_TEMP || channel == ADC_CHANNEL_VREFINT) && !(a->ccr & CCR_TSVREFE)) {
        a->stats.sensor_off_conversions++;
        raw = 0;
    } else {
        raw = adc_expected_raw(a, channel);
    }
    unsigned res = (a->cr1 >> 24) & 3u; /* 12/10/8/6 bits */
    raw = (uint16_t)(raw >> (2u * res));
    a->dr = (a->cr2 & CR2_ALIGN) ? (uint32_t)raw << (4u + 2u * res) : raw;
    a->sr |= SR_EOC | SR_STRT;
    if ((a->cr1 & CR1_EOCIE) && a->irq) a->irq(a->irq_ctx);
}

static uint32_t *reg(adc_t *a, uint32_t offset)
{
    switch (offset) {
    case 0x00u: return &a->sr;
    case 0x04u: return &a->cr1;
    case 0x08u: return &a->cr2;
    case 0x0Cu: return &a->smpr1;
    case 0x10u: return &a->smpr2;
    case 0x2Cu: return &a->sqr1;
    case 0x30u: return &a->sqr2;
    case 0x34u: return &a->sqr3;
    case 0x4Cu: return &a->dr;
    case ADC_COMMON_CCR: return &a->ccr;
    default: return NULL;
    }
}

static uint32_t adc_read(void *ctx, uint32_t offset, int size)
{
    adc_t *a = ctx;
    uint32_t *r = reg(a, offset);
    if (size != 4 || !r) { a->stats.bad_accesses++; return 0; }
    if (!rcc_apb2_clock_enabled(a->rcc, RCC_ADC1_ENABLE_BIT)) return 0;
    uint32_t value = *r;
    if (offset == 0x4Cu) a->sr &= ~SR_EOC; /* reading DR clears EOC */
    return value;
}

static void adc_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    adc_t *a = ctx;
    uint32_t *r = reg(a, offset);
    if (size != 4 || !r) { a->stats.bad_accesses++; return; }
    if (!rcc_apb2_clock_enabled(a->rcc, RCC_ADC1_ENABLE_BIT)) { a->stats.clock_gated++; return; }
    switch (offset) {
    case 0x00u: a->sr &= value | ~0x3Fu; return;          /* rc_w0 status bits */
    case 0x4Cu: return;                                     /* DR is read-only */
    case 0x08u:
        if (value & (CR2_CONT | (1u << 8) | (3u << 28) | (3u << 20))) {
            a->stats.unsupported++; /* continuous, DMA and external triggers */
            value &= ~(CR2_CONT | (1u << 8) | (3u << 28) | (3u << 20));
        }
        a->cr2 = value & ~CR2_SWSTART;
        if ((value & CR2_SWSTART) && (value & CR2_ADON)) convert(a);
        return;
    case 0x2Cu:
        if (value & (0xFu << 20)) a->stats.unsupported++; /* sequence length > 1 */
        a->sqr1 = value;
        return;
    case 0x04u:
        if (value & (1u << 8)) a->stats.unsupported++; /* scan mode */
        a->cr1 = value;
        return;
    default:
        *r = value;
        return;
    }
}

static uint32_t sysmem_read(void *ctx, uint32_t offset, int size)
{
    adc_t *a = ctx;
    (void)a;
    /* Halfwords at 0x2A (VREFINT_CAL), 0x2C (TS_CAL1), 0x2E (TS_CAL2). */
    uint8_t bytes[0x40] = {0};
    const uint16_t cal[3] = {
        raw_from_uv(VREFINT_MV * 1000, CAL_VDDA_MV),
        raw_from_uv(vsense_uv(3000), CAL_VDDA_MV),
        raw_from_uv(vsense_uv(11000), CAL_VDDA_MV),
    };
    for (unsigned i = 0; i < 3; ++i) {
        bytes[0x2Au + 2u * i] = (uint8_t)cal[i];
        bytes[0x2Bu + 2u * i] = (uint8_t)(cal[i] >> 8);
    }
    uint32_t value = 0;
    for (int i = size - 1; i >= 0; --i)
        value = (value << 8) | (offset + (uint32_t)i < sizeof bytes ? bytes[offset + (uint32_t)i] : 0u);
    return value;
}

static void sysmem_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    (void)offset; (void)value; (void)size;
    ((adc_t *)ctx)->stats.bad_accesses++; /* read-only */
}

adc_t *adc_new(const rcc_t *rcc)
{
    if (!rcc) return NULL;
    adc_t *a = calloc(1, sizeof *a);
    if (a) { a->rcc = rcc; a->temperature = 2500; a->vdda = 3300; adc_reset(a); }
    return a;
}

void adc_reset(adc_t *a)
{
    a->sr = a->cr1 = a->cr2 = a->smpr1 = a->smpr2 = a->sqr1 = a->sqr2 = a->sqr3 = a->dr = a->ccr = 0;
    a->stats = (adc_stats_t){0};
}

void adc_free(adc_t *a) { free(a); }
void adc_set_irq(adc_t *a, adc_irq_fn fn, void *ctx) { a->irq = fn; a->irq_ctx = ctx; }

bool adc_set_die_temperature(adc_t *a, int32_t centi)
{
    if (centi < -4000 || centi > 12500) return false;
    a->temperature = centi;
    return true;
}

bool adc_set_vdda(adc_t *a, uint32_t mv)
{
    if (mv < 1800u || mv > 3600u) return false;
    a->vdda = mv;
    return true;
}

adc_stats_t adc_get_stats(const adc_t *a) { return a->stats; }

periph_t adc_as_periph(adc_t *a)
{
    return (periph_t){.name = "ADC", .base = ADC1_BASE, .length = 0x400u,
                      .ctx = a, .read = adc_read, .write = adc_write};
}

periph_t adc_sysmem_as_periph(adc_t *a)
{
    return (periph_t){.name = "SYSMEM-CAL", .base = SYSMEM_CAL_BASE, .length = 0x40u,
                      .ctx = a, .read = sysmem_read, .write = sysmem_write};
}
