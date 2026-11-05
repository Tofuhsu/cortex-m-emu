#ifndef PERIPH_BME280_H
#define PERIPH_BME280_H

#include "i2c.h"

typedef struct bme280 bme280_t;
bme280_t *bme280_new(uint8_t address);
void bme280_free(bme280_t *bme);
void bme280_reset(bme280_t *bme);
i2c_device_t bme280_as_device(bme280_t *bme);
/* Physical inputs persist across chip reset. Conversion is instantaneous;
 * forced mode snapshots on trigger; normal mode updates on host input change.
 * Units: hundredths Celsius, Q22.10 %RH, integer pascals.
 */
bool bme280_set_temperature(bme280_t *bme, int32_t centi_celsius);
bool bme280_set_humidity(bme280_t *bme, uint32_t percent_q10);
bool bme280_set_pressure(bme280_t *bme, uint32_t pascals);
uint8_t bme280_peek(const bme280_t *bme, uint8_t reg);

#endif
