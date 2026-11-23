/* Checks for the FreeRTOS-support work: IT blocks and the exception model
 * (assembled programs 36-37), SCB, SYSCFG/EXTI, SPI1 with the BME280 SPI
 * front end, DMA1/DMA2, USART2 TX DMA, ADC1 with calibration words, IWDG,
 * the flash interface, WFI sleep and SVC escalation. Register sequences are
 * written the way firmware would write them; they are host checks of the
 * models, not a hardware comparison.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "loader.h"
#include "periph/adc.h"
#include "periph/bme280.h"
#include "periph/dma.h"
#include "periph/exti.h"
#include "periph/flash.h"
#include "periph/gpio.h"
#include "periph/iwdg.h"
#include "periph/nvic.h"
#include "periph/rcc.h"
#include "periph/scb.h"
#include "periph/spi.h"
#include "periph/systick.h"
#include "periph/usart.h"

static unsigned flags_of(const cpu_t *c)
{ return (unsigned)c->n * 8u + (unsigned)c->z * 4u + (unsigned)c->c * 2u + (unsigned)c->v; }

static void run_to_bkpt(cpu_t *cpu, bus_t *bus, unsigned limit)
{
    while (!cpu->halted && cpu->steps < limit) cpu_step(cpu, bus);
    if (!cpu->halted || strcmp(cpu->halt_reason, "bkpt #0")) {
        fprintf(stderr, "halt: %s at %08x after %llu steps\n", cpu->halt_reason, cpu->r[PC],
                (unsigned long long)cpu->steps);
        assert(0);
    }
}

static void test_it_blocks(void)
{
    bus_t *bus = bus_new(); cpu_t cpu;
    assert(bus && !load_bin(bus, "build/progs/36_it_blocks.bin"));
    cpu_reset(&cpu, bus);
    run_to_bkpt(&cpu, bus, 100);
    assert(cpu.r[1] == 1 && cpu.r[2] == 0 && cpu.r[3] == 8);
    assert(cpu.r[4] == 20 && cpu.r[7] == 0xBEEFu && flags_of(&cpu) == 8u);
    assert(cpu.itstate == 0);
    bus_free(bus);
    puts("PASS IT blocks: then/else, skipped instructions, no flag update inside IT, 32-bit load in IT");
}

static void test_exceptions_program(void)
{
    bus_t *bus = bus_new(); cpu_t cpu;
    assert(bus && !load_bin(bus, "build/progs/37_exceptions.bin"));
    cpu_reset(&cpu, bus);
    scb_t *scb = scb_new(&cpu); nvic_t *nvic = nvic_new(&cpu);
    assert(scb && nvic);
    assert(!bus_add_periph(bus, scb_as_periph(scb)) && !bus_add_periph(bus, nvic_as_periph(nvic)));
    run_to_bkpt(&cpu, bus, 1000);
    const uint32_t expected[] = {
        0xFFFFFFFDu, 0x2000FFE0u,           /* SVC: Thread/PSP EXC_RETURN, PSP frame */
        0xA0,                                /* IRQ0 masked by BASEPRI */
        0x20, 0xFFFFFFF1u, 17, 0x21, 0x22,  /* IRQ0, nested IRQ1, back in IRQ0 */
        0x30, 0xFFFFFFFDu,                   /* PendSV after IRQ0 returns */
        0xA1,
        0x30, 0xFFFFFFFDu,                   /* PendSV pended from Thread mode */
        0xA2,
    };
    unsigned count = bus_read32(bus, 0x20000000u);
    if (count != sizeof expected / sizeof expected[0]) fprintf(stderr, "log count %u\n", count);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t got = bus_read32(bus, 0x20000004u + 4u * i);
        if (i >= sizeof expected / sizeof expected[0] || got != expected[i])
            fprintf(stderr, "log[%u] = %08x\n", i, got);
    }
    assert(count == sizeof expected / sizeof expected[0]);
    for (unsigned i = 0; i < count; ++i) assert(bus_read32(bus, 0x20000004u + 4u * i) == expected[i]);
    assert(cpu.r[4] == 0x20010000u && cpu.r[5] == 0x20020000u && cpu.r[6] == 2u);
    assert(cpu.r[SP] == 0x20010000u && cpu_get_msp(&cpu) == 0x20020000u && cpu.ipsr == 0);
    for (unsigned n = 0; n < CPU_EXCEPTION_COUNT; ++n) assert(!cpu_exception_active(&cpu, n));
    assert(cpu.exceptions_taken == 5u);
    nvic_free(nvic); scb_free(scb); bus_free(bus);
    puts("PASS exceptions: PSP/CONTROL, SVC, BASEPRI, IPR priorities, nesting, PendSV after return");
}

/* A tiny image: vectors + code at 0x100, written with bus_load_flash. */
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }

static void test_svc_escalation_and_wfi(void)
{
    uint8_t image[0x200] = {0};
    put32(image, 0x20020000u); put32(image + 4, 0x08000101u);
    put32(image + 3 * 4, 0x08000181u);   /* HardFault */
    put32(image + 11 * 4, 0x08000191u);  /* SVC (must not run) */
    put32(image + 15 * 4, 0x08000181u);  /* SysTick uses the same BKPT stub */
    const uint8_t code[] = {0x72, 0xB6, 0x00, 0xDF, 0x00, 0xBE}; /* CPSID i; SVC #0; BKPT */
    memcpy(image + 0x100, code, sizeof code);
    const uint8_t fault[] = {0x01, 0xBE}; /* BKPT #1 */
    memcpy(image + 0x180, fault, sizeof fault);
    const uint8_t svc[] = {0x02, 0xBE};   /* BKPT #2 */
    memcpy(image + 0x190, svc, sizeof svc);
    bus_t *bus = bus_new(); cpu_t cpu; assert(bus && !bus_load_flash(bus, 0, image, sizeof image));
    cpu_reset(&cpu, bus);
    while (!cpu.halted) cpu_step(&cpu, bus);
    assert(!strcmp(cpu.halt_reason, "bkpt #1") && cpu.ipsr == 3 && (cpu.hfsr & (1u << 30)));
    assert(bus_read32(bus, cpu.r[SP] + 24) == 0x08000104u); /* return address after SVC */

    /* WFI with SysTick: the core sleeps, ticks continue, the interrupt wakes it. */
    const uint8_t wfi[] = {0x30, 0xBF, 0x00, 0xBE}; /* WFI; BKPT #0 */
    assert(!bus_load_flash(bus, 0x100, wfi, sizeof wfi));
    cpu_reset(&cpu, bus);
    systick_t *st = systick_new(NULL, NULL); assert(st);
    assert(!bus_add_periph(bus, systick_as_periph(st)));
    bus_write32(bus, SYSTICK_BASE + 4, 99); bus_write32(bus, SYSTICK_BASE, 1); /* enable, no interrupt */
    for (unsigned i = 0; i < 300; ++i) { cpu_step(&cpu, bus); systick_tick(st); }
    assert(cpu.sleeping && cpu.sleep_steps == 299 && !cpu.halted);
    cpu_pend_exception(&cpu, CPU_EXCEPTION_SYSTICK); /* wake-up event */
    while (!cpu.halted) cpu_step(&cpu, bus);
    assert(!strcmp(cpu.halt_reason, "bkpt #1") && cpu.ipsr == 15 && !cpu.sleeping);
    /* With PRIMASK set the pending SysTick still wakes WFI, but is not taken. */
    const uint8_t masked[] = {0x72, 0xB6, 0x30, 0xBF, 0x00, 0xBE}; /* CPSID i; WFI; BKPT #0 */
    assert(!bus_load_flash(bus, 0x100, masked, sizeof masked));
    cpu_reset(&cpu, bus);
    cpu_step(&cpu, bus); cpu_step(&cpu, bus); cpu_step(&cpu, bus);
    assert(cpu.sleeping);
    cpu_pend_exception(&cpu, CPU_EXCEPTION_SYSTICK);
    while (!cpu.halted) cpu_step(&cpu, bus);
    assert(!strcmp(cpu.halt_reason, "bkpt #0") && cpu.ipsr == 0 &&
           cpu_exception_pending(&cpu, CPU_EXCEPTION_SYSTICK));
    systick_free(st); bus_free(bus);
    puts("PASS SVC with PRIMASK escalates to HardFault (HFSR.FORCED); WFI sleeps, wakes, PRIMASK case");
}

static void test_scb(void)
{
    bus_t *bus = bus_new(); cpu_t cpu;
    uint8_t vectors[8]; put32(vectors, 0x20020000u); put32(vectors + 4, 0x08000101u);
    assert(bus && !bus_load_flash(bus, 0, vectors, 8)); cpu_reset(&cpu, bus);
    scb_t *scb = scb_new(&cpu); assert(scb && !bus_add_periph(bus, scb_as_periph(scb)));
    assert(bus_read32(bus, SCB_BASE) == 0x410FC241u);
    bus_write32(bus, SCB_BASE + SCB_ICSR, 1u << 28);
    assert(cpu_exception_pending(&cpu, 14) && (bus_read32(bus, SCB_BASE + SCB_ICSR) & (1u << 28)));
    assert(((bus_read32(bus, SCB_BASE + SCB_ICSR) >> 12) & 0x1FFu) == 14u);
    bus_write32(bus, SCB_BASE + SCB_ICSR, 1u << 27);
    assert(!cpu_exception_pending(&cpu, 14));
    bus_write8(bus, SCB_BASE + SCB_SHPR3 + 2, 0xFF);  /* PendSV byte */
    bus_write8(bus, SCB_BASE + SCB_SHPR3 + 3, 0x5A);  /* SysTick byte */
    assert(bus_read32(bus, SCB_BASE + SCB_SHPR3) == 0x50F00000u);
    bus_write32(bus, SCB_BASE + SCB_SHPR2, 0x12345678u);
    assert(bus_read32(bus, SCB_BASE + SCB_SHPR2) == 0x10000000u); /* only SVC byte, 4 bits */
    bus_write32(bus, SCB_BASE + SCB_AIRCR, 0x00000300u);           /* wrong key: ignored */
    assert(cpu.prigroup == 0 && scb_bad_access_count(scb) == 1);
    bus_write32(bus, SCB_BASE + SCB_AIRCR, 0x05FA0300u);
    assert(cpu.prigroup == 3 && bus_read32(bus, SCB_BASE + SCB_AIRCR) == 0xFA050300u);
    bus_write32(bus, SCB_BASE + SCB_VTOR, 0x08004000u);
    assert(cpu.vtor == 0x08004000u && bus_read32(bus, SCB_BASE + SCB_CCR) == 0x200u);
    /* PRIGROUP 3: bits [3:0] are subpriority; with 4 implemented bits all
     * priority bits are group bits, so 0x50 still preempts 0x60. */
    assert(cpu_exception_priority(&cpu, 15) == 0x50 && cpu_exception_priority(&cpu, 3) == -1);
    scb_free(scb); bus_free(bus);
    puts("PASS SCB: CPUID, ICSR PendSV set/clear and VECTPENDING, SHPR bytes, AIRCR key/PRIGROUP, VTOR, CCR");
}

typedef struct { unsigned count, last; } irq_log_t;
static void log_irq(void *ctx, unsigned irq) { irq_log_t *l = ctx; l->count++; l->last = irq; }
static void log_stream(void *ctx, unsigned stream) { irq_log_t *l = ctx; l->count++; l->last = stream; }
static void log_simple(void *ctx) { ((irq_log_t *)ctx)->count++; }

static void test_exti(void)
{
    bus_t *bus = bus_new(); rcc_t *rcc = rcc_new();
    gpio_t *gpioc = gpio_new("GPIOC", GPIOC_BASE, rcc, RCC_GPIOC_ENABLE_BIT);
    exti_t *exti = exti_new(rcc); irq_log_t log = {0};
    assert(bus && rcc && gpioc && exti);
    assert(!bus_add_periph(bus, rcc_as_periph(rcc)) && !bus_add_periph(bus, exti_as_periph(exti)) &&
           !bus_add_periph(bus, exti_syscfg_as_periph(exti)));
    exti_set_irq(exti, log_irq, &log);
    bus_write32(bus, SYSCFG_BASE + 0x14, 0x20);  /* SYSCFG clock off: ignored */
    assert(bus_read32(bus, SYSCFG_BASE + 0x14) == 0 && exti_bad_access_count(exti) == 1);
    bus_write32(bus, RCC_BASE + RCC_APB2ENR_OFFSET, 1u << RCC_SYSCFG_ENABLE_BIT);
    bus_write32(bus, SYSCFG_BASE + 0x14, 0x20);  /* EXTICR4: EXTI13 = PC */
    bus_write32(bus, EXTI_BASE + EXTI_FTSR, 1u << 13);
    bus_write32(bus, EXTI_BASE + EXTI_IMR, 1u << 13);
    exti_gpio_changed(exti, 0, 1u << 13, 0);     /* port A edge: not selected */
    assert(log.count == 0);
    exti_gpio_changed(exti, 2, 0, 1u << 13);     /* rising: not enabled */
    assert(log.count == 0);
    exti_gpio_changed(exti, 2, 1u << 13, 0);     /* falling on PC13 */
    assert(log.count == 1 && log.last == 40 && (bus_read32(bus, EXTI_BASE + EXTI_PR) & (1u << 13)));
    bus_write32(bus, EXTI_BASE + EXTI_PR, 1u << 13);
    assert(!(bus_read32(bus, EXTI_BASE + EXTI_PR) & (1u << 13)));
    bus_write32(bus, EXTI_BASE + EXTI_SWIER, 1u << 3);  /* line 3 masked: pending only */
    assert(log.count == 1 && (bus_read32(bus, EXTI_BASE + EXTI_PR) & 8u));
    bus_write32(bus, EXTI_BASE + EXTI_IMR, (1u << 13) | 8u);
    assert(log.count == 2 && log.last == 9);
    exti_free(exti); gpio_free(gpioc); rcc_free(rcc); bus_free(bus);
    puts("PASS SYSCFG/EXTI: port selection, falling-edge trigger, IRQ40/IRQ9 mapping, PR rc_w1, SWIER");
}

typedef struct {
    bus_t *bus; rcc_t *rcc; gpio_t *gpiob; spi_t *spi; bme280_t *bme; dma_t *dma2;
} spi_fixture_t;

static spi_fixture_t spi_fixture(void)
{
    spi_fixture_t f;
    f.bus = bus_new(); f.rcc = rcc_new();
    f.gpiob = gpio_new("GPIOB", GPIOB_BASE, f.rcc, RCC_GPIOB_ENABLE_BIT);
    f.spi = spi_new(f.rcc, f.gpiob); f.bme = bme280_new(0x76); f.dma2 = dma_new(2, f.rcc, f.bus);
    assert(f.bus && f.rcc && f.gpiob && f.spi && f.bme && f.dma2);
    assert(!bus_add_periph(f.bus, rcc_as_periph(f.rcc)) && !bus_add_periph(f.bus, gpio_as_periph(f.gpiob)) &&
           !bus_add_periph(f.bus, spi_as_periph(f.spi)) && !bus_add_periph(f.bus, dma_as_periph(f.dma2)));
    assert(!spi_attach(f.spi, bme280_as_spi_device(f.bme), f.gpiob, 6));
    assert(!dma_connect(f.dma2, 0, 3, spi_dma_rx_request, f.spi));
    assert(!dma_connect(f.dma2, 3, 3, spi_dma_tx_request, f.spi));
    bus_write32(f.bus, RCC_BASE + RCC_AHB1ENR_OFFSET, (1u << 1) | (1u << RCC_DMA2_ENABLE_BIT));
    bus_write32(f.bus, RCC_BASE + RCC_APB2ENR_OFFSET, 1u << RCC_SPI1_ENABLE_BIT);
    /* PB3/4/5 AF5, PB6 output high (CS idle). PB8/PB9 keep their reset AF. */
    bus_write32(f.bus, GPIOB_BASE + GPIO_BSRR_OFFSET, 1u << 6);
    bus_write32(f.bus, GPIOB_BASE + GPIO_MODER_OFFSET, (2u << 6) | (2u << 8) | (2u << 10) | (1u << 12));
    bus_write32(f.bus, GPIOB_BASE + GPIO_AFRL_OFFSET, (5u << 12) | (5u << 16) | (5u << 20));
    bus_write32(f.bus, SPI1_BASE + SPI_CR1, SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (3u << 3) | SPI_CR1_SPE);
    return f;
}

static void spi_fixture_free(spi_fixture_t *f)
{
    dma_free(f->dma2); bme280_free(f->bme); spi_free(f->spi); gpio_free(f->gpiob); rcc_free(f->rcc); bus_free(f->bus);
}

static uint8_t spi_byte(spi_fixture_t *f, uint8_t out)
{
    assert(bus_read32(f->bus, SPI1_BASE + SPI_SR) & SPI_SR_TXE);
    bus_write32(f->bus, SPI1_BASE + SPI_DR, out);
    assert(bus_read32(f->bus, SPI1_BASE + SPI_SR) & SPI_SR_RXNE);
    return (uint8_t)bus_read32(f->bus, SPI1_BASE + SPI_DR);
}

static void cs(spi_fixture_t *f, bool low)
{
    bus_write32(f->bus, GPIOB_BASE + GPIO_BSRR_OFFSET, low ? 1u << 22 : 1u << 6);
}

static void test_spi_bme280(void)
{
    spi_fixture_t f = spi_fixture();
    cs(&f, true);
    spi_byte(&f, 0xD0);                       /* read chip id */
    assert(spi_byte(&f, 0) == 0x60);
    cs(&f, false);
    /* Write ctrl_hum then ctrl_meas (forced) as two control/data pairs. */
    cs(&f, true);
    spi_byte(&f, 0x72); spi_byte(&f, 0x01);
    spi_byte(&f, 0x74); spi_byte(&f, 0x25);
    cs(&f, false);
    assert(bme280_peek(f.bme, 0xF2) == 1 && (bme280_peek(f.bme, 0xF4) & 3u) == 0); /* forced -> sleep */
    cs(&f, true);
    spi_byte(&f, 0x88);                       /* calibration burst, auto-increment */
    uint8_t t1lo = spi_byte(&f, 0), t1hi = spi_byte(&f, 0);
    cs(&f, false);
    assert((t1lo | (t1hi << 8)) == 24000);
    /* Frames with CS high reach no device; MISO reads 0xFF. */
    assert(spi_byte(&f, 0xD0) == 0xFF && spi_get_stats(f.spi).unselected_frames == 1);
    /* Overrun: a second frame while RXNE is set is lost. */
    bus_write32(f.bus, SPI1_BASE + SPI_DR, 0); bus_write32(f.bus, SPI1_BASE + SPI_DR, 0);
    assert(bus_read32(f.bus, SPI1_BASE + SPI_SR) & SPI_SR_OVR);
    (void)bus_read32(f.bus, SPI1_BASE + SPI_DR); (void)bus_read32(f.bus, SPI1_BASE + SPI_SR);
    assert(!(bus_read32(f.bus, SPI1_BASE + SPI_SR) & SPI_SR_OVR));
    /* SSM=1 with SSI=0 is a mode fault. */
    bus_write32(f.bus, SPI1_BASE + SPI_CR1, SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SPE);
    assert((bus_read32(f.bus, SPI1_BASE + SPI_SR) & SPI_SR_MODF) &&
           !(bus_read32(f.bus, SPI1_BASE + SPI_CR1) & SPI_CR1_MSTR));
    spi_fixture_free(&f);
    puts("PASS SPI1 + BME280 SPI: chip id 0x60, write pairs, burst read, CS gating, OVR, MODF");
}

static void test_spi_dma(void)
{
    spi_fixture_t f = spi_fixture(); irq_log_t log = {0};
    dma_set_irq(f.dma2, log_stream, &log);
    /* TX buffer: control byte 0x88 + 25 dummies; RX gets 26 bytes. */
    uint8_t tx[26] = {0x88};
    for (unsigned i = 0; i < 26; ++i) bus_write8(f.bus, 0x20000000u + i, tx[i]);
    const uint32_t rx = 0x20000100u;
    /* RX stream 0 ch3: P2M, MINC, TCIE. TX stream 3 ch3: M2P, MINC. */
    bus_write32(f.bus, DMA2_BASE + DMA_SxPAR(0), SPI1_BASE + SPI_DR);
    bus_write32(f.bus, DMA2_BASE + DMA_SxM0AR(0), rx);
    bus_write32(f.bus, DMA2_BASE + DMA_SxNDTR(0), 26);
    bus_write32(f.bus, DMA2_BASE + DMA_SxCR(0), (3u << 25) | DMA_CR_MINC | DMA_CR_TCIE | DMA_CR_EN);
    bus_write32(f.bus, DMA2_BASE + DMA_SxPAR(3), SPI1_BASE + SPI_DR);
    bus_write32(f.bus, DMA2_BASE + DMA_SxM0AR(3), 0x20000000u);
    bus_write32(f.bus, DMA2_BASE + DMA_SxNDTR(3), 26);
    bus_write32(f.bus, DMA2_BASE + DMA_SxCR(3), (3u << 25) | (1u << 6) | DMA_CR_MINC | DMA_CR_EN);
    cs(&f, true);
    bus_write32(f.bus, SPI1_BASE + SPI_CR2, SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    unsigned ticks = 0;
    while ((bus_read32(f.bus, DMA2_BASE + DMA_SxCR(0)) & DMA_CR_EN) && ticks < 100) { dma_tick(f.dma2); ++ticks; }
    cs(&f, false);
    assert(ticks == 27); /* one item per stream per step; RX trails TX by one step */
    assert(bus_read8(f.bus, rx + 1) == (uint8_t)24000 && bus_read8(f.bus, rx + 2) == (uint8_t)(24000 >> 8));
    assert(log.count == 1 && log.last == 0);
    assert(bus_read32(f.bus, DMA2_BASE + 0x00) & (1u << 5));     /* LISR TCIF0 */
    assert(bus_read32(f.bus, DMA2_BASE + 0x00) & (1u << 27));    /* LISR TCIF3 */
    bus_write32(f.bus, DMA2_BASE + 0x08, (1u << 5) | (1u << 4) | (1u << 27) | (1u << 26));
    assert(bus_read32(f.bus, DMA2_BASE + 0x00) == 0);
    assert(spi_get_stats(f.spi).overruns == 0 && dma_get_stats(f.dma2).items == 52);
    /* FIFO mode is rejected: EN stays clear. */
    bus_write32(f.bus, DMA2_BASE + DMA_SxNDTR(1), 4);
    bus_write32(f.bus, DMA2_BASE + DMA_SxFCR(1), 1u << 2);
    bus_write32(f.bus, DMA2_BASE + DMA_SxCR(1), DMA_CR_EN);
    assert(!(bus_read32(f.bus, DMA2_BASE + DMA_SxCR(1)) & DMA_CR_EN) && dma_get_stats(f.dma2).rejected_config == 1);
    spi_fixture_free(&f);
    puts("PASS DMA2 + SPI1: full-duplex 26-byte calibration burst, TCIF/IRQ, IFCR clear, no overrun, FIFO rejected");
}

static void count_tx(void *ctx, uint8_t byte) { (void)byte; ((irq_log_t *)ctx)->count++; }

static void test_usart_dma(void)
{
    bus_t *bus = bus_new(); rcc_t *rcc = rcc_new();
    gpio_t *gpioa = gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT);
    usart_t *u = usart_new(rcc, gpioa); dma_t *dma1 = dma_new(1, rcc, bus); irq_log_t log = {0}, tx = {0};
    assert(bus && rcc && gpioa && u && dma1);
    assert(!bus_add_periph(bus, rcc_as_periph(rcc)) && !bus_add_periph(bus, gpio_as_periph(gpioa)) &&
           !bus_add_periph(bus, usart_as_periph(u)) && !bus_add_periph(bus, dma_as_periph(dma1)));
    assert(!dma_connect(dma1, 6, 4, usart_dma_tx_request, u));
    dma_set_irq(dma1, log_stream, &log);
    usart_set_tx_observer(u, count_tx, &tx);
    bus_write32(bus, RCC_BASE + RCC_AHB1ENR_OFFSET, 1u | (1u << RCC_DMA1_ENABLE_BIT));
    bus_write32(bus, RCC_BASE + RCC_APB1ENR_OFFSET, 1u << RCC_USART2_ENABLE_BIT);
    bus_write32(bus, GPIOA_BASE + GPIO_MODER_OFFSET, 0xA8000000u | (2u << 4));
    bus_write32(bus, GPIOA_BASE + GPIO_AFRL_OFFSET, 7u << 8);
    bus_write32(bus, USART2_BASE + USART_CR1_OFFSET, USART_CR1_UE | USART_CR1_TE);
    bus_write32(bus, USART2_BASE + USART_CR3_OFFSET, USART_CR3_DMAT);
    const char *text = "hello\r\n";
    for (unsigned i = 0; i < 7; ++i) bus_write8(bus, 0x20000000u + i, (uint8_t)text[i]);
    bus_write32(bus, DMA1_BASE + DMA_SxPAR(6), USART2_BASE + USART_DR_OFFSET);
    bus_write32(bus, DMA1_BASE + DMA_SxM0AR(6), 0x20000000u);
    bus_write32(bus, DMA1_BASE + DMA_SxNDTR(6), 7);
    /* PAR write while enabled must be ignored; set it before enabling. */
    bus_write32(bus, DMA1_BASE + DMA_SxCR(6), (4u << 25) | (1u << 6) | DMA_CR_MINC | DMA_CR_TCIE | DMA_CR_EN);
    bus_write32(bus, DMA1_BASE + DMA_SxPAR(6), 0x12345678u);
    assert(bus_read32(bus, DMA1_BASE + DMA_SxPAR(6)) == USART2_BASE + USART_DR_OFFSET);
    for (unsigned i = 0; i < 10; ++i) dma_tick(dma1);
    assert(tx.count == 7 && log.count == 1 && log.last == 6 && bus_read32(bus, DMA1_BASE + DMA_SxNDTR(6)) == 0);
    assert(bus_read32(bus, DMA1_BASE + 0x04) & (1u << 21)); /* HISR TCIF6 */
    /* Wrong channel: no request reaches the stream. */
    bus_write32(bus, DMA1_BASE + 0x0C, 0x0F7D0F7Du);
    bus_write32(bus, DMA1_BASE + DMA_SxNDTR(6), 3);
    bus_write32(bus, DMA1_BASE + DMA_SxCR(6), (3u << 25) | (1u << 6) | DMA_CR_MINC | DMA_CR_EN);
    for (unsigned i = 0; i < 10; ++i) dma_tick(dma1);
    assert(tx.count == 7 && bus_read32(bus, DMA1_BASE + DMA_SxNDTR(6)) == 3);
    dma_free(dma1); usart_free(u); gpio_free(gpioa); rcc_free(rcc); bus_free(bus);
    puts("PASS DMA1 + USART2 TX: stream 6 channel 4, one byte per step, TCIF6, write protection, CHSEL match");
}

static void test_adc(void)
{
    bus_t *bus = bus_new(); rcc_t *rcc = rcc_new(); adc_t *adc = adc_new(rcc); irq_log_t log = {0};
    assert(bus && rcc && adc);
    assert(!bus_add_periph(bus, rcc_as_periph(rcc)) && !bus_add_periph(bus, adc_as_periph(adc)) &&
           !bus_add_periph(bus, adc_sysmem_as_periph(adc)));
    adc_set_irq(adc, log_simple, &log);
    assert(bus_read16(bus, VREFINT_CAL_ADDR) == 1502 && bus_read16(bus, TS_CAL1_ADDR) == 959 &&
           bus_read16(bus, TS_CAL2_ADDR) == 1207);
    bus_write32(bus, RCC_BASE + RCC_APB2ENR_OFFSET, 1u << RCC_ADC1_ENABLE_BIT);
    bus_write32(bus, ADC1_BASE + 0x08, 1);                 /* ADON */
    bus_write32(bus, ADC1_BASE + 0x34, ADC_CHANNEL_TEMP);
    bus_write32(bus, ADC1_BASE + 0x08, 1u | (1u << 30));   /* SWSTART, TSVREFE off */
    assert(bus_read32(bus, ADC1_BASE + 0x4C) == 0 && adc_get_stats(adc).sensor_off_conversions == 1);
    bus_write32(bus, ADC1_BASE + ADC_COMMON_CCR, 1u << 23);
    assert(adc_set_die_temperature(adc, 3000) && adc_set_vdda(adc, 3300));
    bus_write32(bus, ADC1_BASE + 0x08, 1u | (1u << 30));
    assert(bus_read32(bus, ADC1_BASE) & 2u);              /* EOC */
    assert(bus_read32(bus, ADC1_BASE + 0x4C) == 959);      /* equals TS_CAL1 at 30 C / 3.3 V */
    assert(!(bus_read32(bus, ADC1_BASE) & 2u));           /* DR read clears EOC */
    bus_write32(bus, ADC1_BASE + 0x04, 1u << 5);           /* EOCIE */
    assert(adc_set_vdda(adc, 3000));
    bus_write32(bus, ADC1_BASE + 0x34, ADC_CHANNEL_VREFINT);
    bus_write32(bus, ADC1_BASE + 0x08, 1u | (1u << 30));
    assert(log.count == 1 && bus_read32(bus, ADC1_BASE + 0x4C) == 1652); /* 1210/3000*4095 */
    assert(!adc_set_vdda(adc, 1000) && !adc_set_die_temperature(adc, 20000));
    adc_free(adc); rcc_free(rcc); bus_free(bus);
    puts("PASS ADC1: calibration words 1502/959/1207, TSVREFE gating, temperature and VREFINT raw values, EOC/EOCIE");
}

static void count_reset(void *ctx) { ((irq_log_t *)ctx)->count++; }

static void test_iwdg(void)
{
    iwdg_t *w = iwdg_new(10); bus_t *bus = bus_new(); irq_log_t resets = {0};
    assert(w && bus && !bus_add_periph(bus, iwdg_as_periph(w)));
    iwdg_set_reset(w, count_reset, &resets);
    bus_write32(bus, IWDG_BASE + IWDG_RLR, 5);   /* no 0x5555 yet: ignored */
    assert(bus_read32(bus, IWDG_BASE + IWDG_RLR) == 0xFFF);
    bus_write32(bus, IWDG_BASE + IWDG_KR, 0x5555);
    bus_write32(bus, IWDG_BASE + IWDG_PR, 0);    /* /4 */
    bus_write32(bus, IWDG_BASE + IWDG_RLR, 5);
    bus_write32(bus, IWDG_BASE + IWDG_KR, 0xCCCC);
    /* Each count = 4 LSI periods = 40 steps; 5 counts = 200 steps. */
    for (unsigned i = 0; i < 199; ++i) iwdg_tick(w);
    assert(resets.count == 0 && iwdg_counter(w) == 1);
    bus_write32(bus, IWDG_BASE + IWDG_KR, 0xAAAA);   /* refresh: counter = RLR */
    /* The prescaler keeps running across a refresh: the first count after it
     * comes 1 step later (39 of 40 steps had passed), the rest every 40. */
    for (unsigned i = 0; i < 160; ++i) iwdg_tick(w);
    assert(resets.count == 0 && iwdg_counter(w) == 1);
    iwdg_tick(w);
    assert(resets.count == 1 && iwdg_reload_count(w) == 1);
    bus_write32(bus, IWDG_BASE + IWDG_PR, 3);        /* write access closed by 0xAAAA/0xCCCC */
    assert(bus_read32(bus, IWDG_BASE + IWDG_PR) == 0);
    iwdg_free(w); bus_free(bus);
    puts("PASS IWDG: key protection, start, prescaled countdown in steps, refresh, reset callback");
}

static void test_flash(void)
{
    bus_t *bus = bus_new(); flash_if_t *f = flash_if_new(bus);
    assert(bus && f && !bus_add_periph(bus, flash_if_as_periph(f)));
    const uint32_t sector7 = FLASH_BASE + 0x60000u;
    bus_write32(bus, sector7, 0);                                  /* no PG: PGSERR, ignored */
    assert(!bus_fault(bus) && (bus_read32(bus, FLASH_IF_BASE + FLASH_SR) & FLASH_SR_PGSERR));
    assert(bus_read32(bus, sector7) == 0);                          /* blank image is zero */
    bus_write32(bus, FLASH_IF_BASE + FLASH_SR, FLASH_SR_PGSERR);
    bus_write32(bus, FLASH_IF_BASE + FLASH_CR, FLASH_CR_SER | (7u << 3) | FLASH_CR_STRT); /* locked */
    assert(bus_read32(bus, sector7) == 0 && (bus_read32(bus, FLASH_IF_BASE + FLASH_CR) & FLASH_CR_LOCK));
    bus_write32(bus, FLASH_IF_BASE + FLASH_KEYR, 0x45670123u);
    bus_write32(bus, FLASH_IF_BASE + FLASH_KEYR, 0xCDEF89ABu);
    assert(!(bus_read32(bus, FLASH_IF_BASE + FLASH_CR) & FLASH_CR_LOCK));
    bus_write32(bus, FLASH_IF_BASE + FLASH_CR, FLASH_CR_SER | (7u << 3) | (2u << 8) | FLASH_CR_EOPIE | FLASH_CR_STRT);
    assert(bus_read32(bus, sector7) == 0xFFFFFFFFu && bus_read32(bus, sector7 + 0x1FFFCu) == 0xFFFFFFFFu);
    assert(bus_read32(bus, FLASH_BASE + 0x5FFFCu) == 0);           /* sector 6 untouched */
    assert(bus_read32(bus, FLASH_IF_BASE + FLASH_SR) & FLASH_SR_EOP);
    bus_write32(bus, FLASH_IF_BASE + FLASH_CR, FLASH_CR_PG | (2u << 8));
    bus_write32(bus, sector7, 0xA5A5F00Fu);
    assert(bus_read32(bus, sector7) == 0xA5A5F00Fu);
    bus_write32(bus, sector7, 0xFFFF0FF0u);                        /* only clears bits */
    assert(bus_read32(bus, sector7) == 0xA5A50000u && flash_if_get_stats(f).overwrite_nonerased == 1);
    bus_write16(bus, sector7 + 4, 0x1234);                         /* PSIZE x32: parallelism error */
    assert((bus_read32(bus, FLASH_IF_BASE + FLASH_SR) & FLASH_SR_PGPERR) && bus_read32(bus, sector7 + 4) == 0xFFFFFFFFu);
    bus_write32(bus, FLASH_IF_BASE + FLASH_CR, FLASH_CR_LOCK);
    bus_write32(bus, sector7 + 8, 0);
    assert(bus_read32(bus, sector7 + 8) == 0xFFFFFFFFu);
    flash_if_free(f);
    bus_write32(bus, sector7, 0);                                  /* no interface: bus fault */
    assert(bus_fault(bus));
    bus_free(bus);
    puts("PASS flash interface: PGSERR, LOCK/KEYR, sector 7 erase only, PG AND-programming, PGPERR, relock");
}

int main(void)
{
    test_it_blocks();
    test_exceptions_program();
    test_svc_escalation_and_wfi();
    test_scb();
    test_exti();
    test_spi_bme280();
    test_spi_dma();
    test_usart_dma();
    test_adc();
    test_iwdg();
    test_flash();
    return 0;
}
