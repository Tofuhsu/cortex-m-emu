/*
 * CPU tests: run each program in tests/progs/ until BKPT, then check registers and flags.
 *
 * Expected values were cross-checked by single-stepping the same .bin files in
 * Renode 1.17 (stm32f4 platform) on 2026-11-03. One deliberate difference: after
 * reset Renode leaves LR = 0, while the ARMv7-M ARM specifies LR = 0xFFFFFFFF;
 * these tests follow the manual. APSR flags are UNKNOWN after reset, so tests only
 * check flags that the program itself sets.
 *
 * The CPU must halt on BKPT with a reason that starts with "bkpt".
 */
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "loader.h"
#include "periph/gpio.h"
#include "periph/rcc.h"

#define MAX_STEPS 1000

static int tests_run, tests_failed, test_ok;

static void check(const char *what, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        printf("    %s: got 0x%08x, expected 0x%08x\n", what, actual, expected);
        test_ok = 0;
    }
}

#define CHECK_REG(cpu, n, value) check("r" #n, (cpu)->r[n], (value))
#define CHECK_FLAG(cpu, f, value) check("flag " #f, (cpu)->f, (value))

/* Load build/progs/<name>.bin into bus, reset, run to BKPT. */
static void run_on(bus_t *bus, const char *name, cpu_t *cpu)
{
    char path[256];
    snprintf(path, sizeof path, "build/progs/%s.bin", name);
    if (load_bin(bus, path) != 0) {
        printf("    cannot load %s (run `make progs`)\n", path);
        test_ok = 0;
        memset(cpu, 0, sizeof *cpu);
        return;
    }
    cpu_reset(cpu, bus);
    while (!cpu->halted && cpu->steps < MAX_STEPS) {
        cpu_step(cpu, bus);
    }
    if (!cpu->halted) {
        printf("    did not halt within %d steps\n", MAX_STEPS);
        test_ok = 0;
    } else if (strncmp(cpu->halt_reason, "bkpt", 4) != 0) {
        printf("    halted early: %s\n", cpu->halt_reason);
        test_ok = 0;
    }
}

/* Same as run_on() on a bus with no peripheral models. Returns the bus for memory checks. */
static bus_t *run(const char *name, cpu_t *cpu)
{
    bus_t *bus = bus_new();
    run_on(bus, name, cpu);
    return bus;
}

static void test_reset(void)
{
    cpu_t cpu;
    bus_t *bus = run("01_reset", &cpu);
    check("SP", cpu.r[SP], 0x20020000);
    check("LR", cpu.r[LR], 0xFFFFFFFF);
    check("PC at bkpt", cpu.r[PC], 0x08000008);
    bus_free(bus);
}

static void test_movs(void)
{
    cpu_t cpu;
    bus_t *bus = run("02_movs", &cpu);
    CHECK_REG(&cpu, 0, 5);
    CHECK_REG(&cpu, 1, 0);
    CHECK_FLAG(&cpu, z, 1);
    CHECK_FLAG(&cpu, n, 0);
    bus_free(bus);
}

static void test_adds_subs(void)
{
    cpu_t cpu;
    bus_t *bus = run("03_adds_subs", &cpu);
    CHECK_REG(&cpu, 0, 0xFFFFFFFF);
    CHECK_REG(&cpu, 1, 207);
    CHECK_REG(&cpu, 2, 0);
    CHECK_FLAG(&cpu, n, 0);
    CHECK_FLAG(&cpu, z, 1);
    CHECK_FLAG(&cpu, c, 1);
    CHECK_FLAG(&cpu, v, 0);
    bus_free(bus);
}

static void test_branch_loop(void)
{
    cpu_t cpu;
    bus_t *bus = run("04_branch_loop", &cpu);
    CHECK_REG(&cpu, 0, 0);
    CHECK_REG(&cpu, 1, 6);
    check("steps", (uint32_t)cpu.steps, 12); /* 11 instructions + BKPT */
    bus_free(bus);
}

static void test_ldr_literal(void)
{
    cpu_t cpu;
    bus_t *bus = run("05_ldr_literal", &cpu);
    CHECK_REG(&cpu, 0, 0x40023830);
    CHECK_REG(&cpu, 1, 0x80000000);
    CHECK_FLAG(&cpu, n, 1);
    CHECK_FLAG(&cpu, z, 0);
    CHECK_FLAG(&cpu, c, 0);
    CHECK_FLAG(&cpu, v, 1);
    bus_free(bus);
}

static void test_load_store(void)
{
    cpu_t cpu;
    bus_t *bus = run("06_load_store", &cpu);
    CHECK_REG(&cpu, 2, 0xAB);
    CHECK_REG(&cpu, 3, 0xAB);
    check("SP restored", cpu.r[SP], 0x20020000);
    check("mem[0x20000004]", bus_read32(bus, 0x20000004), 0xAB);
    check("mem[sp-4]", bus_read32(bus, 0x2001FFFC), 0xAB);
    bus_free(bus);
}

static void test_calls(void)
{
    cpu_t cpu;
    bus_t *bus = run("07_calls", &cpu);
    CHECK_REG(&cpu, 0, 116);
    check("SP balanced", cpu.r[SP], 0x20020000);
    check("LR from last BL", cpu.r[LR], 0x08000013);
    bus_free(bus);
}

static void test_cond_branches(void)
{
    cpu_t cpu;
    bus_t *bus = run("08_cond_branches", &cpu);
    CHECK_REG(&cpu, 1, 0);
    CHECK_REG(&cpu, 2, 2);
    CHECK_REG(&cpu, 3, 1);
    bus_free(bus);
}

static void test_thumb2_logic(void)
{
    cpu_t cpu;
    bus_t *bus = run("09_thumb2_logic", &cpu);
    CHECK_REG(&cpu, 1, 0xA8000420);
    CHECK_REG(&cpu, 2, 0x00AB00AB);
    CHECK_FLAG(&cpu, z, 1); /* ORR.W without S leaves flags from MOVS r2, #0 */
    bus_free(bus);
}

static void test_wide_load_store(void)
{
    cpu_t cpu;
    bus_t *bus = run("10_wide_load_store", &cpu);
    CHECK_REG(&cpu, 2, 0x12345678);
    CHECK_REG(&cpu, 3, 0x78);
    bus_free(bus);
}

/*
 * RCC + GPIOA models. Single-stepped in Renode 1.17 on 2026-11-04: MODER read-back
 * and the ODR after each BSRR set/reset match. Two deliberate differences, where
 * these tests follow RM0390 instead of Renode:
 *   - Renode does not gate GPIO on RCC_AHB1ENR, so the ODR write before the clock
 *     is enabled takes effect there (ODR = 0x20 after step 3).
 *   - For BSRR = 0x00200020 (set and reset PA5 together) Renode clears PA5;
 *     RM0390 says the set bit has priority, so ODR stays 0x20 here.
 */
static void test_gpio(void)
{
    cpu_t cpu;
    bus_t *bus = bus_new();
    rcc_t *rcc = rcc_new();
    gpio_t *gpioa = gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT);
    if (!bus || !rcc || !gpioa || bus_add_periph(bus, rcc_as_periph(rcc)) != 0 ||
        bus_add_periph(bus, gpio_as_periph(gpioa)) != 0) {
        printf("    cannot set up RCC/GPIOA models\n");
        test_ok = 0;
    } else {
        run_on(bus, "11_gpio", &cpu);
        check("MODER read back", cpu.r[5], 0xA8000400);
        check("ODR read back", cpu.r[7], 0x20);
        check("ODR", gpio_get_odr(gpioa), 0x20);
        check("gated writes", (uint32_t)gpio_gated_write_count(gpioa), 1);
        check("bad accesses", (uint32_t)(gpio_bad_access_count(gpioa) + rcc_bad_access_count(rcc)), 0);
        check("unmapped accesses", bus_unmapped_count(bus), 0);
    }
    bus_free(bus);
    gpio_free(gpioa);
    rcc_free(rcc);
}

/*
 * SVC exception entry and return (ARMv7-M ARM B1.5.6, B1.5.8). Single-stepped in
 * Renode 1.17 on 2026-11-04. Exception entry is not an instruction, so it is not a
 * step: 15 instructions + BKPT. The stacked LR is the reset LR, so it differs from
 * Renode (0) for the same reason as test_reset.
 */
static void test_svc(void)
{
    cpu_t cpu;
    bus_t *bus = run("12_svc", &cpu);
    CHECK_REG(&cpu, 0, 42);      /* changed through the stacked frame */
    CHECK_REG(&cpu, 1, 2);
    CHECK_REG(&cpu, 2, 3);       /* restored by unstacking */
    CHECK_REG(&cpu, 3, 4);
    check("SP after return", cpu.r[SP], 0x20020000);
    check("LR after return", cpu.r[LR], 0xFFFFFFFF);
    check("steps", (uint32_t)cpu.steps, 16);
    check("IPSR in handler", bus_read32(bus, 0x20000000), 11);
    check("EXC_RETURN", bus_read32(bus, 0x20000004), 0xFFFFFFF9);
    check("SP in handler", bus_read32(bus, 0x20000008), 0x2001FFE0);
    check("stacked r0", bus_read32(bus, 0x2001FFE0), 42);
    check("stacked r1", bus_read32(bus, 0x2001FFE4), 2);
    check("stacked r2", bus_read32(bus, 0x2001FFE8), 3);
    check("stacked r3", bus_read32(bus, 0x2001FFEC), 4);
    check("stacked r12", bus_read32(bus, 0x2001FFF0), 0);
    check("stacked LR", bus_read32(bus, 0x2001FFF4), 0xFFFFFFFF);
    check("stacked return address", bus_read32(bus, 0x2001FFF8), 0x0800004A);
    check("stacked xPSR", bus_read32(bus, 0x2001FFFC), 0x01000000);
    bus_free(bus);
}

static void test_bus_rejects_flash_write(void)
{
    bus_t *bus = bus_new();
    bus_write32(bus, 0x08000000, 1);
    check("fault", bus_fault(bus), 1);
    bus_free(bus);
}

static void test_bus_flash_alias(void)
{
    bus_t *bus = bus_new();
    const uint8_t img[4] = {0x00, 0x00, 0x02, 0x20};
    bus_load_flash(bus, 0, img, 4);
    check("alias 0x0", bus_read32(bus, 0x00000000), 0x20020000);
    check("flash", bus_read32(bus, 0x08000000), 0x20020000);
    bus_free(bus);
}

#define RUN(fn)                                            \
    do {                                                   \
        test_ok = 1;                                       \
        tests_run++;                                       \
        printf("%s\n", #fn);                               \
        fn();                                              \
        printf("    %s\n", test_ok ? "PASS" : "FAIL");     \
        tests_failed += !test_ok;                          \
    } while (0)

int main(void)
{
    RUN(test_bus_rejects_flash_write);
    RUN(test_bus_flash_alias);
    RUN(test_reset);
    RUN(test_movs);
    RUN(test_adds_subs);
    RUN(test_branch_loop);
    RUN(test_ldr_literal);
    RUN(test_load_store);
    RUN(test_calls);
    RUN(test_cond_branches);
    RUN(test_thumb2_logic);
    RUN(test_wide_load_store);
    RUN(test_gpio);
    RUN(test_svc);
    printf("\n%d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
