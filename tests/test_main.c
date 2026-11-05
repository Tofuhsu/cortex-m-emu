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
#include "periph/systick.h"
#include "periph/usart.h"

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

typedef void (*after_reset_fn)(cpu_t *cpu, void *ctx);

/* Load build/progs/<name>.bin into bus, reset, call after_reset (if any), run to BKPT.
 * Hooks must be installed in after_reset, because cpu_reset() clears them. */
static void run_hooked(bus_t *bus, const char *name, cpu_t *cpu,
                       after_reset_fn after_reset, void *ctx)
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
    if (after_reset) {
        after_reset(cpu, ctx);
    }
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

static void run_on(bus_t *bus, const char *name, cpu_t *cpu)
{
    run_hooked(bus, name, cpu, NULL, NULL);
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

/*
 * SVC with SP = 4 mod 8 (ARMv7-M ARM B1.5.7): the frame is aligned down to 8 bytes,
 * bit 9 of the stacked xPSR records the 4 padding bytes, and return adds them back.
 * 4 + 6 handler + 3 instructions = 13 steps. Single-stepped in Renode 1.17 on
 * 2026-11-04: frame base, stacked xPSR and SP after return match.
 */
static void test_svc_align(void)
{
    cpu_t cpu;
    bus_t *bus = run("14_svc_align", &cpu);
    check("SP before SVC", cpu.r[5], 0x2001FFFC);
    check("SP after return", cpu.r[6], 0x2001FFFC);
    check("SP at end", cpu.r[SP], 0x20020000);
    check("frame base", bus_read32(bus, 0x20000000), 0x2001FFD8);
    check("stacked xPSR (bit 9 set)", bus_read32(bus, 0x20000004), 0x41000200); /* Z from MOVS */
    check("stacked return address", bus_read32(bus, 0x2001FFF0), 0x08000048);
    check("steps", (uint32_t)cpu.steps, 13);
    bus_free(bus);
}

/* SysTick wiring for tests: the model ticks once after each executed instruction. */
static void systick_pend(void *ctx)
{
    cpu_pend_exception(ctx, CPU_EXCEPTION_SYSTICK);
}

static void systick_step(void *ctx)
{
    systick_tick(ctx);
}

static void systick_hooks(cpu_t *cpu, void *ctx)
{
    cpu_set_step_hook(cpu, systick_step, ctx);
}

/*
 * SysTick interrupt, counted in instructions (one tick per executed instruction;
 * this is a model choice, not clock cycles). The enable write is step 7; the tick
 * after it loads RVR = 99, and 99 more ticks reach 0 after step 106, a CMP. So the
 * first entry stacks the BNE at 0x08000050, which has not run yet, and the handler
 * starts at step 107. Period RVR + 1 = 100: later events after steps 206 and 306.
 * Handler: 11 instructions on the first entry, 4 on the others. Total 317 steps.
 *
 * Single-stepped in Renode 1.17 on 2026-11-04: it reaches BKPT with r4 = 3, IPSR = 15,
 * EXC_RETURN = 0xFFFFFFF9, and COUNTFLAG is set on the first CSR read and clear on
 * the second. Two differences, where these checks follow this model:
 *   - Renode's SysTick follows its virtual time, not instructions: BKPT is reached
 *     at step 430, not 317. Step counts and the CVR value are this model's.
 *   - Renode reads CLKSOURCE (CSR bit 2) as 1 after the program writes CSR = 0,
 *     so its r6/r7 are 0x00010004/0x4. This model stores the written bit.
 */
static void test_systick(void)
{
    cpu_t cpu;
    bus_t *bus = bus_new();
    systick_t *st = systick_new(systick_pend, &cpu);
    if (!bus || !st || bus_add_periph(bus, systick_as_periph(st)) != 0) {
        printf("    cannot set up SysTick model\n");
        test_ok = 0;
    } else {
        run_hooked(bus, "13_systick", &cpu, systick_hooks, st);
        CHECK_REG(&cpu, 4, 3);
        check("CSR with COUNTFLAG", cpu.r[6], 0x00010000);
        check("CSR after read-to-clear", cpu.r[7], 0);
        check("SP", cpu.r[SP], 0x20020000);
        check("steps", (uint32_t)cpu.steps, 317);
        check("IPSR in handler", bus_read32(bus, 0x20000000), 15);
        check("EXC_RETURN", bus_read32(bus, 0x20000004), 0xFFFFFFF9);
        check("first return address", bus_read32(bus, 0x20000008), 0x08000050);
        check("CVR at end", systick_peek(st, SYSTICK_CVR_OFFSET), 93);
        check("nothing pending", cpu_exception_pending(&cpu, CPU_EXCEPTION_SYSTICK), 0);
        check("bad accesses", (uint32_t)systick_bad_access_count(st), 0);
        check("unmapped accesses", bus_unmapped_count(bus), 0);
    }
    bus_free(bus);
    systick_free(st);
}

/*
 * PRIMASK (ARMv7-M ARM B1.4.3; CPS and MRS in B5.2). CPSID i is step 1. The enable
 * write is step 8; with RVR = 9, events follow steps 17, 27, 37 and 47 while
 * PRIMASK = 1, and they coalesce into one pending SysTick. CPSIE i is step 52, so
 * the handler starts at step 53 and stacks the NOP after CPSIE (0x0800005E). The
 * handler stops SysTick first; 7 handler instructions; BKPT is step 62.
 *
 * Single-stepped in Renode 1.17 on 2026-11-04: the same PC at every step from 49
 * to 61, including entry right after CPSIE, and the same registers and stacked
 * return address. (Its SysTick runs on virtual time, but events still fall inside
 * the masked loop.) The CVR value is this model's.
 */
static void test_primask(void)
{
    cpu_t cpu;
    bus_t *bus = bus_new();
    systick_t *st = systick_new(systick_pend, &cpu);
    if (!bus || !st || bus_add_periph(bus, systick_as_periph(st)) != 0) {
        printf("    cannot set up SysTick model\n");
        test_ok = 0;
    } else {
        run_hooked(bus, "15_primask", &cpu, systick_hooks, st);
        check("handler runs while masked", cpu.r[7], 0);
        check("PRIMASK after CPSID", cpu.r[6], 1);
        check("PRIMASK after CPSIE", cpu.r[3], 0);
        CHECK_REG(&cpu, 4, 1); /* coalesced: one entry */
        check("stacked return address", bus_read32(bus, 0x20000000), 0x0800005E);
        check("SP", cpu.r[SP], 0x20020000);
        check("steps", (uint32_t)cpu.steps, 62);
        check("CVR at end", systick_peek(st, SYSTICK_CVR_OFFSET), 4);
        check("nothing pending", cpu_exception_pending(&cpu, CPU_EXCEPTION_SYSTICK), 0);
    }
    bus_free(bus);
    systick_free(st);
}

/* Collects bytes the USART2 model routes to PA2. */
typedef struct {
    uint8_t bytes[16];
    unsigned count;
} tx_capture_t;

static void capture_tx(void *ctx, uint8_t byte)
{
    tx_capture_t *cap = ctx;
    if (cap->count < sizeof cap->bytes) {
        cap->bytes[cap->count] = byte;
    }
    cap->count++;
}

/* Run a USART2 program on RCC + GPIOA + USART2 models; return the stats. */
static usart_stats_t run_usart(const char *name, cpu_t *cpu, tx_capture_t *cap)
{
    usart_stats_t stats = {0};
    bus_t *bus = bus_new();
    rcc_t *rcc = rcc_new();
    gpio_t *gpioa = gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT);
    usart_t *usart = usart_new(rcc, gpioa);
    if (!bus || !rcc || !gpioa || !usart || bus_add_periph(bus, rcc_as_periph(rcc)) != 0 ||
        bus_add_periph(bus, gpio_as_periph(gpioa)) != 0 ||
        bus_add_periph(bus, usart_as_periph(usart)) != 0) {
        printf("    cannot set up RCC/GPIOA/USART2 models\n");
        test_ok = 0;
    } else {
        usart_set_tx_observer(usart, capture_tx, cap);
        run_on(bus, name, cpu);
        stats = usart_get_stats(usart);
        check("BRR", usart_peek(usart, USART_BRR_OFFSET), 0x8B);
        check("unmapped accesses", bus_unmapped_count(bus), 0);
        check("RCC/GPIO bad accesses",
              (uint32_t)(rcc_bad_access_count(rcc) + gpio_bad_access_count(gpioa)), 0);
    }
    usart_free(usart);
    bus_free(bus);
    gpio_free(gpioa);
    rcc_free(rcc);
    return stats;
}

/*
 * USART2 transmit of "OK\n" (RM0390 USART chapter; PA2 = AF7 per DS10693). TX
 * completes at once in this model, so every TXE/TC poll passes the first time:
 * 20 setup + 3 x (2 + 6 putc) + 4 TC poll + 2 reads + BKPT = 51 steps.
 * Single-stepped in Renode 1.17 on 2026-11-05: BKPT after 50 steps, SR = 0xC0,
 * CR1 = 0x2008, and USART2 output 4f 4b 0a.
 */
static void test_usart(void)
{
    cpu_t cpu;
    tx_capture_t cap = {0};
    usart_stats_t st = run_usart("16_usart", &cpu, &cap);
    check("bytes sent", cap.count, 3);
    check("byte 0", cap.bytes[0], 'O');
    check("byte 1", cap.bytes[1], 'K');
    check("byte 2", cap.bytes[2], '\n');
    check("final SR", cpu.r[6], 0xC0);
    check("CR1 read back", cpu.r[7], 0x2008);
    check("steps", (uint32_t)cpu.steps, 51);
    check("clock-gated writes", (uint32_t)st.clock_gated_writes, 0);
    check("UE-off DR writes", (uint32_t)st.ue_disabled_dr_writes, 0);
    check("TE-off DR writes", (uint32_t)st.te_disabled_dr_writes, 0);
    check("pin mismatch bytes", (uint32_t)st.pin_mismatch_bytes, 0);
    check("USART bad accesses", (uint32_t)st.bad_accesses, 0);
}

/*
 * PA2 left at AF0: the USART completes all three transfers (same 48-step flow
 * as test_usart without the 3 AFRL instructions), but no byte reaches the pin.
 * This is an emulator policy. Renode 1.17 (2026-11-05) reaches BKPT after the same
 * 47 steps with SR = 0xC0, but still outputs 4f 4b 0a: it ignores the pin mux.
 */
static void test_usart_no_af(void)
{
    cpu_t cpu;
    tx_capture_t cap = {0};
    usart_stats_t st = run_usart("17_usart_no_af", &cpu, &cap);
    check("bytes sent", cap.count, 0);
    check("pin mismatch bytes", (uint32_t)st.pin_mismatch_bytes, 3);
    check("final SR", cpu.r[6], 0xC0);
    check("steps", (uint32_t)cpu.steps, 48);
    check("USART bad accesses", (uint32_t)st.bad_accesses, 0);
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
    RUN(test_svc_align);
    RUN(test_systick);
    RUN(test_primask);
    RUN(test_usart);
    RUN(test_usart_no_af);
    printf("\n%d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
