#ifndef PERIPH_NVIC_H
#define PERIPH_NVIC_H

#include "cpu.h"

/* PM0214 Rev10 4.3.1, p208: ISER/ICER/ISPR/ICPR/IABR/IPR bases
 * E000E100/E180/E200/E280/E300/E400. Register details: pp210-215.
 */
#define NVIC_BASE 0xE000E100u
typedef struct nvic nvic_t;
/* CPU is borrowed. Construct/install after cpu_reset; free before CPU expires.
 * Pending bits live in CPU, enable/IPR state here: no shadow pending copy.
 */
nvic_t *nvic_new(cpu_t *cpu);
void nvic_free(nvic_t *nvic);
void nvic_reset(nvic_t *nvic);
bool nvic_pend(nvic_t *nvic, unsigned irq);
periph_t nvic_as_periph(nvic_t *nvic);
uint64_t nvic_bad_access_count(const nvic_t *nvic);

#endif
