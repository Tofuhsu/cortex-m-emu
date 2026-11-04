/*
 * cortex-m-emu: run a raw STM32 firmware image (.bin, linked at 0x08000000).
 *
 *   cortex-m-emu [-n max_steps] [-t] firmware.bin
 *     -n  stop after this many instructions (default 1000000)
 *     -t  print each instruction's address and first halfword before it executes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "loader.h"
#include "periph/gpio.h"
#include "periph/rcc.h"

static void dump_registers(const cpu_t *cpu)
{
    for (int i = 0; i < 16; i++) {
        printf("r%-2d = 0x%08x%s", i, cpu->r[i], (i % 4 == 3) ? "\n" : "   ");
    }
    printf("N=%d Z=%d C=%d V=%d   steps=%llu\n", cpu->n, cpu->z, cpu->c, cpu->v,
           (unsigned long long)cpu->steps);
}

static void observe_pa5(void *ctx, const char *name,
                        uint16_t old_odr, uint16_t new_odr)
{
    const cpu_t *cpu = ctx;
    (void)name;
    if (((old_odr ^ new_odr) & (1u << 5)) != 0) {
        /* cpu_step increments steps after the peripheral write completes. */
        printf("PA5 -> %u at step %llu\n", (unsigned)((new_odr >> 5) & 1u),
               (unsigned long long)(cpu->steps + 1u));
    }
}

int main(int argc, char **argv)
{
    unsigned long long max_steps = 1000000;
    int trace = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-t") == 0) {
            trace = 1;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-n max_steps] [-t] firmware.bin\n", argv[0]);
        return 2;
    }

    bus_t *bus = bus_new();
    if (!bus || load_bin(bus, path) != 0) {
        fprintf(stderr, "cannot load %s\n", path);
        bus_free(bus);
        return 1;
    }

    rcc_t *rcc = rcc_new();
    gpio_t *gpioa = gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT);
    if (!rcc || !gpioa || bus_add_periph(bus, rcc_as_periph(rcc)) != 0 ||
        bus_add_periph(bus, gpio_as_periph(gpioa)) != 0) {
        fprintf(stderr, "cannot initialize RCC/GPIOA models\n");
        bus_free(bus);
        gpio_free(gpioa);
        rcc_free(rcc);
        return 1;
    }

    cpu_t cpu;
    cpu_reset(&cpu, bus);
    gpio_set_observer(gpioa, observe_pa5, &cpu);
    while (!cpu.halted && cpu.steps < max_steps) {
        if (trace) {
            printf("%08x: %04x\n", cpu.r[PC], bus_read16(bus, cpu.r[PC]));
        }
        cpu_step(&cpu, bus);
    }

    printf("stopped: %s\n", cpu.halted ? cpu.halt_reason : "step limit reached");
    dump_registers(&cpu);
    if (bus_unmapped_count(bus)) {
        printf("note: %u accesses to peripheral addresses with no model (last 0x%08x)\n",
               bus_unmapped_count(bus), bus_last_unmapped_addr(bus));
    }
    if (gpio_gated_write_count(gpioa)) {
        printf("note: %llu GPIOA writes ignored while clock disabled\n",
               (unsigned long long)gpio_gated_write_count(gpioa));
    }
    if (rcc_bad_access_count(rcc) || gpio_bad_access_count(gpioa)) {
        printf("note: unsupported/invalid peripheral accesses: RCC=%llu GPIOA=%llu\n",
               (unsigned long long)rcc_bad_access_count(rcc),
               (unsigned long long)gpio_bad_access_count(gpioa));
    }
    bus_free(bus);
    gpio_free(gpioa);
    rcc_free(rcc);
    return 0;
}
