/*
 * cortex-m-emu: run a raw STM32 firmware image (.bin, linked at 0x08000000).
 *
 *   cortex-m-emu [-n max_steps] [-t] firmware.bin
 *     -n  stop after this many instructions (default 1000000)
 *     -t  print each instruction's address and first halfword before it executes
 *         (after any pending exception has been taken, so it shows what really runs)
 *     --gdb port  wait for arm-none-eabi-gdb on 127.0.0.1:port (0 = any free port);
 *         the debugger controls execution and -n is ignored
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bus.h"
#include "cpu.h"
#include "loader.h"
#include "periph/gpio.h"
#include "periph/rcc.h"
#include "periph/systick.h"
#include "periph/usart.h"
#include "periph/nvic.h"
#include "periph/bme280.h"
#include "vcd.h"
#include "gdbstub.h"

typedef struct { cpu_t *cpu; vcd_t *vcd; } observer_t;

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
    observer_t *observer = ctx;
    const cpu_t *cpu = observer->cpu;
    (void)name;
    if (((old_odr ^ new_odr) & (1u << 5)) != 0) {
        vcd_pa5(observer->vcd, cpu->steps + 1u, (new_odr & (1u << 5)) != 0);
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
    observer_t *observer = ctx;
    vcd_uart(observer->vcd, observer->cpu->steps + 1u, byte);
    putchar(byte);
    fflush(stdout);
}

static void observe_i2c(void *ctx, i2c_event_t event)
{
    observer_t *o = ctx;
    vcd_i2c(o->vcd, o->cpu->steps + 1u, event);
}

/* Post-instruction snapshots. Row zero is the natural reset state. */
static void trace_registers(FILE *file, const cpu_t *cpu)
{
    fprintf(file, "%llu,0x%08x", (unsigned long long)cpu->steps, cpu->r[PC]);
    for (unsigned r = 0; r < 16; ++r) fprintf(file, ",0x%08x", cpu->r[r]);
    fprintf(file, ",0x%08x\n", cpu_get_xpsr(cpu));
}

static bool same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    return !strcmp(a, b) || (stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
                            sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino);
}

static bool unsigned_arg(const char *text, unsigned long long *value)
{
    char *end;
    if (!*text || *text == '-') return false;
    errno = 0;
    unsigned long long result = strtoull(text, &end, 0);
    if (errno || *end) return false;
    *value = result;
    return true;
}

int main(int argc, char **argv)
{
    unsigned long long max_steps = 1000000;
    int trace = 0;
    int32_t temperature = 2500;
    const char *path = NULL, *vcd_path = NULL, *regs_path = NULL;
    long gdb_port = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            if (!unsigned_arg(argv[++i], &max_steps)) goto usage;
        } else if (strcmp(argv[i], "-t") == 0) trace = 1;
        else if (strcmp(argv[i], "--vcd") == 0 && i + 1 < argc) vcd_path = argv[++i];
        else if (strcmp(argv[i], "--trace-regs") == 0 && i + 1 < argc) regs_path = argv[++i];
        else if (strcmp(argv[i], "--gdb") == 0 && i + 1 < argc) {
            unsigned long long value;
            if (!unsigned_arg(argv[++i], &value) || value > 65535) goto usage;
            gdb_port = (long)value;
        }
        else if (strcmp(argv[i], "--temperature-centi") == 0 && i + 1 < argc) {
            char *end; errno = 0;
            long value = strtol(argv[++i], &end, 10);
            if (errno || !*argv[i] || *end || value < -4000 || value > 8500) goto usage;
            temperature = (int32_t)value;
        } else if (argv[i][0] == '-' || path) goto usage;
        else path = argv[i];
    }
    if (!path) goto usage;
    /* Outputs must not truncate the input image or each other. */
    if ((vcd_path && same_file(path, vcd_path)) ||
        (regs_path && same_file(path, regs_path)) ||
        (vcd_path && regs_path && same_file(vcd_path, regs_path))) goto usage;

    int result = 1;
    bus_t *bus = bus_new();
    cpu_t cpu = {0};
    rcc_t *rcc = NULL;
    gpio_t *gpioa = NULL, *gpiob = NULL;
    systick_t *systick = NULL;
    usart_t *usart2 = NULL;
    nvic_t *nvic = NULL;
    i2c_t *i2c = NULL;
    bme280_t *bme = NULL;
    vcd_t *vcd = NULL;
    FILE *regs = NULL;
    if (!bus || load_bin(bus, path) != 0) { fprintf(stderr, "cannot load %s\n", path); goto cleanup; }
    cpu_reset(&cpu, bus);
    rcc = rcc_new();
    gpioa = rcc ? gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT) : NULL;
    gpiob = rcc ? gpio_new("GPIOB", GPIOB_BASE, rcc, RCC_GPIOB_ENABLE_BIT) : NULL;
    systick = systick_new(pend_systick, &cpu);
    usart2 = gpioa ? usart_new(rcc, gpioa) : NULL;
    nvic = nvic_new(&cpu);
    i2c = gpiob ? i2c_new(rcc, gpiob) : NULL;
    bme = bme280_new(0x76);
    if (!rcc || !gpioa || !gpiob || !systick || !usart2 || !nvic || !i2c || !bme) {
        fprintf(stderr, "cannot allocate peripheral models\n"); goto cleanup;
    }
    bme280_set_temperature(bme, temperature);
    if (i2c_attach(i2c, bme280_as_device(bme)) != 0 ||
        bus_add_periph(bus, rcc_as_periph(rcc)) != 0 ||
        bus_add_periph(bus, gpio_as_periph(gpioa)) != 0 ||
        bus_add_periph(bus, gpio_as_periph(gpiob)) != 0 ||
        bus_add_periph(bus, systick_as_periph(systick)) != 0 ||
        bus_add_periph(bus, usart_as_periph(usart2)) != 0 ||
        bus_add_periph(bus, nvic_as_periph(nvic)) != 0 ||
        bus_add_periph(bus, i2c_as_periph(i2c)) != 0) {
        fprintf(stderr, "cannot register peripheral models\n"); goto cleanup;
    }
    if (vcd_path && !(vcd = vcd_open(vcd_path))) { perror(vcd_path); goto cleanup; }
    if (regs_path) {
        if (vcd_path && same_file(vcd_path, regs_path)) { fprintf(stderr, "output paths alias each other\n"); goto cleanup; }
        regs = fopen(regs_path, "w");
        if (!regs) { perror(regs_path); goto cleanup; }
        fputs("step,pc", regs);
        for (unsigned r = 0; r < 16; ++r) fprintf(regs, ",r%u", r);
        fputs(",xpsr\n", regs);
        trace_registers(regs, &cpu);
    }
    observer_t observer = {&cpu, vcd};
    cpu_set_step_hook(&cpu, tick_systick, systick);
    if (trace) cpu_set_trace_hook(&cpu, trace_instruction, NULL);
    gpio_set_observer(gpioa, observe_pa5, &observer);
    usart_set_tx_observer(usart2, print_usart_byte, &observer);
    i2c_set_observer(i2c, observe_i2c, &observer);
    if (gdb_port >= 0) {
        if (gdbstub_serve(&cpu, bus, (uint16_t)gdb_port) != 0) goto cleanup;
    } else {
        while (!cpu.halted && cpu.steps < max_steps) {
            uint64_t before = cpu.steps;
            cpu_step(&cpu, bus);
            if (regs && cpu.steps != before) trace_registers(regs, &cpu);
        }
    }
    vcd_end(vcd, cpu.steps + 1u);
    printf("stopped: %s\n", cpu.halted ? cpu.halt_reason : "step limit reached");
    dump_registers(&cpu);
    if (bus_unmapped_count(bus)) printf("note: %u unmapped accesses (last 0x%08x)\n",
                                      bus_unmapped_count(bus), bus_last_unmapped_addr(bus));
    i2c_stats_t is = i2c_get_stats(i2c);
    usart_stats_t us = usart_get_stats(usart2);
    if (is.clock_gated_writes || is.pe_disabled_transfers || is.pin_mismatch_starts ||
        is.protocol_errors || is.bad_accesses)
        printf("note: I2C1: clock off=%llu PE off=%llu pin mismatch=%llu protocol=%llu bad access=%llu\n",
            (unsigned long long)is.clock_gated_writes, (unsigned long long)is.pe_disabled_transfers,
            (unsigned long long)is.pin_mismatch_starts, (unsigned long long)is.protocol_errors,
            (unsigned long long)is.bad_accesses);
    if (us.clock_gated_writes || us.ue_disabled_dr_writes || us.te_disabled_dr_writes || us.pin_mismatch_bytes || us.bad_accesses)
        printf("note: USART2: clock off=%llu UE off=%llu TE off=%llu pin mismatch=%llu bad access=%llu\n",
            (unsigned long long)us.clock_gated_writes, (unsigned long long)us.ue_disabled_dr_writes,
            (unsigned long long)us.te_disabled_dr_writes, (unsigned long long)us.pin_mismatch_bytes,
            (unsigned long long)us.bad_accesses);
    if (rcc_bad_access_count(rcc) || gpio_bad_access_count(gpioa) || gpio_bad_access_count(gpiob) ||
        systick_bad_access_count(systick) || nvic_bad_access_count(nvic))
        printf("note: bad accesses RCC=%llu GPIOA=%llu GPIOB=%llu SysTick=%llu NVIC=%llu\n",
            (unsigned long long)rcc_bad_access_count(rcc), (unsigned long long)gpio_bad_access_count(gpioa),
            (unsigned long long)gpio_bad_access_count(gpiob), (unsigned long long)systick_bad_access_count(systick),
            (unsigned long long)nvic_bad_access_count(nvic));
    if (gpio_gated_write_count(gpioa) || gpio_gated_write_count(gpiob))
        printf("note: GPIO gated writes A=%llu B=%llu\n", (unsigned long long)gpio_gated_write_count(gpioa),
               (unsigned long long)gpio_gated_write_count(gpiob));
    result = cpu.halted && strncmp(cpu.halt_reason, "bkpt #", 6) != 0 ? 1 : 0;
cleanup:
    if (regs && (ferror(regs) || fflush(regs))) result = 1;
    if (regs && fclose(regs)) result = 1;
    if (!vcd_close(vcd)) { fprintf(stderr, "VCD write failed\n"); result = 1; }
    nvic_free(nvic); usart_free(usart2); systick_free(systick);
    i2c_free(i2c); bme280_free(bme); gpio_free(gpiob); gpio_free(gpioa); rcc_free(rcc); bus_free(bus);
    return result;
usage:
    fprintf(stderr, "usage: %s [-n max_steps] [-t] [--vcd file] [--trace-regs file] [--gdb port] "
                    "[--temperature-centi -4000..8500] firmware.bin\n", argv[0]);
    return 2;
}
