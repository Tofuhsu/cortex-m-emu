#ifndef VCD_H
#define VCD_H

#include <stdbool.h>
#include <stdint.h>
#include "periph/i2c.h"

typedef struct vcd vcd_t;
vcd_t *vcd_open(const char *path);
void vcd_pa5(vcd_t *vcd, uint64_t step, bool value);
void vcd_uart(vcd_t *vcd, uint64_t step, uint8_t byte);
void vcd_i2c(vcd_t *vcd, uint64_t step, i2c_event_t event);
bool vcd_close(vcd_t *vcd);

#endif
