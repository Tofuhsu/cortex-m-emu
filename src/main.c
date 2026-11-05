/*
 * cortex-m-emu: run a raw STM32 firmware image (.bin, linked at 0x08000000).
 *
 *   cortex-m-emu [-n max_steps] [-t] firmware.bin
 *     -n  stop after this many instructions (default 1000000)
 *     -t  print each instruction's address and first halfword before it executes
 *         (after any pending exception has been taken, so it shows what really runs)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "loader.h"
#include "periph/gpio.h"
#include "periph/rcc.h"
#include "periph/systick.h"
#include "periph/usart.h"

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

static void trace_instruction(void *ctx, uint32_t pc, uint16_t first_halfword)
{
    (void)ctx;
    printf("%08x: %04x\n", pc, first_halfword);
}

/* SysTick counts executed instructions: one tick after each step (not clock cycles). */
static void tick_systick(void *ctx)
{
    systick_tick(ctx);
}

static void pend_systick(void *ctx)
{
    cpu_pend_exception(ctx, CPU_EXCEPTION_SYSTICK);
}

/* USART2 bytes that reach PA2 go to stdout unchanged. */
static void print_usart_byte(void *ctx, uint8_t byte)
{
    (void)ctx;
    putchar(byte);
    fflush(stdout);
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

    cpu_t cpu;
    rcc_t *rcc = rcc_new();
    gpio_t *gpioa = gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT);
    systick_t *systick = systick_new(pend_systick, &cpu);
    usart_t *usart2 = usart_new(rcc, gpioa);
    if (!rcc || !gpioa || !systick || !usart2 || bus_add_periph(bus, rcc_as_periph(rcc)) != 0 ||
        bus_add_periph(bus, gpio_as_periph(gpioa)) != 0 ||
        bus_add_periph(bus, systick_as_periph(systick)) != 0 ||
        bus_add_periph(bus, usart_as_periph(usart2)) != 0) {
        fprintf(stderr, "cannot initialize RCC/GPIOA/SysTick/USART2 models\n");
        bus_free(bus);
        usart_free(usart2);
        systick_free(systick);
        gpio_free(gpioa);
        rcc_free(rcc);
        return 1;
    }

    cpu_reset(&cpu, bus); /* clears hooks, so install them after */
    cpu_set_step_hook(&cpu, tick_systick, systick);
    if (trace) {
        cpu_set_trace_hook(&cpu, trace_instruction, NULL);
    }
    gpio_set_observer(gpioa, observe_pa5, &cpu);
    usart_set_tx_observer(usart2, print_usart_byte, NULL);
    while (!cpu.halted && cpu.steps < max_steps) {
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
    usart_stats_t us = usart_get_stats(usart2);
    if (us.clock_gated_writes || us.ue_disabled_dr_writes || us.te_disabled_dr_writes ||
        us.pin_mismatch_bytes) {
        printf("note: USART2 bytes not sent: clock off=%llu UE off=%llu TE off=%llu"
               " PA2 not AF7=%llu\n",
               (unsigned long long)us.clock_gated_writes,
               (unsigned long long)us.ue_disabled_dr_writes,
               (unsigned long long)us.te_disabled_dr_writes,
               (unsigned long long)us.pin_mismatch_bytes);
    }
    if (rcc_bad_access_count(rcc) || gpio_bad_access_count(gpioa) ||
        systick_bad_access_count(systick) || us.bad_accesses) {
        printf("note: unsupported/invalid peripheral accesses: RCC=%llu GPIOA=%llu"
               " SysTick=%llu USART2=%llu\n",
               (unsigned long long)rcc_bad_access_count(rcc),
               (unsigned long long)gpio_bad_access_count(gpioa),
               (unsigned long long)systick_bad_access_count(systick),
               (unsigned long long)us.bad_accesses);
    }
    bus_free(bus);
    usart_free(usart2);
    systick_free(systick);
    gpio_free(gpioa);
    rcc_free(rcc);
    return 0;
}
