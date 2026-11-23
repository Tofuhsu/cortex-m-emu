/* STM32F446 independent watchdog (RM0390 Rev9 section 18).
 *
 * The LSI clock is replaced by a stated model parameter: one LSI period =
 * `steps_per_lsi` emulator steps (default 500, i.e. as if one step were one
 * cycle of the 16 MHz HSI and LSI were exactly 32 kHz). The real LSI is
 * specified at 17-47 kHz (DS10693), so timeouts here are counts, not times.
 * When the counter reaches zero the model calls the reset callback.
 */
#ifndef PERIPH_IWDG_H
#define PERIPH_IWDG_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

#define IWDG_BASE 0x40003000u
#define IWDG_KR  0x00u
#define IWDG_PR  0x04u
#define IWDG_RLR 0x08u
#define IWDG_SR  0x0Cu

typedef struct iwdg iwdg_t;
typedef void (*iwdg_reset_fn)(void *ctx);

iwdg_t *iwdg_new(uint32_t steps_per_lsi);
void iwdg_free(iwdg_t *iwdg);
void iwdg_reset(iwdg_t *iwdg); /* stops the watchdog: only a system reset does this */
periph_t iwdg_as_periph(iwdg_t *iwdg);
void iwdg_set_reset(iwdg_t *iwdg, iwdg_reset_fn fn, void *ctx);
void iwdg_tick(iwdg_t *iwdg); /* one emulator step */
bool iwdg_running(const iwdg_t *iwdg);
uint32_t iwdg_counter(const iwdg_t *iwdg);
uint64_t iwdg_reload_count(const iwdg_t *iwdg);

#endif
