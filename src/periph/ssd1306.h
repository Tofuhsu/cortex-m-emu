/* SSD1306 128x64 OLED controller as an I2C device (write-only use).
 *
 * After each START the first byte is a control byte: Co (bit 7) and D/C#
 * (bit 6). With Co = 0 the rest of the transfer is a command stream
 * (D/C# = 0) or a data stream (D/C# = 1); with Co = 1 one byte follows and
 * then another control byte. Data bytes go to GDDRAM at the address pointer.
 *
 * Modeled commands: display on/off (AE/AF), memory addressing mode (20,
 * horizontal and page), column and page windows (21, 22), page-mode start
 * page (B0-B7) and column (00-1F). Other documented commands are accepted
 * with their argument bytes and stored nowhere. Segment remap, COM scan
 * direction, contrast and charge pump do not change the frame dump, which
 * shows GDDRAM as written: column x, page p, bit b -> pixel (x, 8p + b).
 * Reads return 0. No timing, no scrolling.
 */
#ifndef PERIPH_SSD1306_H
#define PERIPH_SSD1306_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "i2c.h"

#define SSD1306_WIDTH 128u
#define SSD1306_PAGES 8u

typedef struct ssd1306 ssd1306_t;

ssd1306_t *ssd1306_new(uint8_t address);
void ssd1306_free(ssd1306_t *oled);
i2c_device_t ssd1306_as_device(ssd1306_t *oled);
bool ssd1306_display_on(const ssd1306_t *oled);
uint8_t ssd1306_gddram(const ssd1306_t *oled, unsigned page, unsigned column);
/* Bytes that were neither a known command, argument nor data. */
uint64_t ssd1306_unknown_bytes(const ssd1306_t *oled);
/* Write GDDRAM as a plain PBM (P1) image, 128x64, 1 = lit. Deterministic. */
bool ssd1306_write_pbm(const ssd1306_t *oled, FILE *file);

#endif
