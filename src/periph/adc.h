/* STM32F446 ADC1, software-triggered single conversions (RM0390 Rev9 13),
 * plus the system-memory calibration words (DS10693 Rev 11, tables 70/76).
 *
 * Inputs are synthetic and stated here, not measured on a chip:
 *   VDDA          = --vdda-mv (default 3300 mV)
 *   VREFINT       = 1210 mV (datasheet typical)
 *   VSENSE(T)     = 760 mV + 2.5 mV/C * (T - 25 C) (datasheet typical V25 and slope)
 *   VREFINT_CAL   = round(1210 / 3300 * 4095) = 1502 (1501.5 rounded up) (as if taken at 3.3 V, 30 C)
 *   TS_CAL1/2     = VSENSE(30 C) and VSENSE(110 C) at 3.3 V: 959 and 1207
 * A conversion completes inside the SWSTART write: no sampling time, no
 * noise, no offset. Only regular single conversion of SQR3's first channel.
 */
#ifndef PERIPH_ADC_H
#define PERIPH_ADC_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "rcc.h"

#define ADC1_BASE      0x40012000u
#define ADC_COMMON_CCR 0x304u /* offset from ADC1_BASE */
#define SYSMEM_CAL_BASE 0x1FFF7A00u
#define VREFINT_CAL_ADDR 0x1FFF7A2Au
#define TS_CAL1_ADDR     0x1FFF7A2Cu
#define TS_CAL2_ADDR     0x1FFF7A2Eu
#define ADC_CHANNEL_VREFINT 17u
#define ADC_CHANNEL_TEMP    18u

typedef struct adc adc_t;
typedef void (*adc_irq_fn)(void *ctx);

typedef struct {
    uint64_t conversions, sensor_off_conversions, unsupported, bad_accesses, clock_gated;
} adc_stats_t;

adc_t *adc_new(const rcc_t *rcc);
void adc_free(adc_t *adc);
void adc_reset(adc_t *adc); /* register reset; host inputs persist */
periph_t adc_as_periph(adc_t *adc);     /* ADC1 and the common block (0x40012000..0x400123FF) */
periph_t adc_sysmem_as_periph(adc_t *adc); /* read-only calibration words */
void adc_set_irq(adc_t *adc, adc_irq_fn fn, void *ctx); /* EOC with EOCIE: IRQ 18 */
bool adc_set_die_temperature(adc_t *adc, int32_t centi_celsius); /* -4000..12500 */
bool adc_set_vdda(adc_t *adc, uint32_t millivolts);              /* 1800..3600 */
/* Raw result the model returns for a channel (host checks; no side effects). */
uint16_t adc_expected_raw(const adc_t *adc, unsigned channel);
adc_stats_t adc_get_stats(const adc_t *adc);

#endif
