/*
 * cortex-m-emu: run a raw STM32F446 firmware image (.bin, linked at 0x08000000).
 *
 *   cortex-m-emu [options] firmware.bin
 *     -n  stop after this many steps (default 1000000); a step is one
 *         instruction, or one sleep step while the core waits in WFI/WFE
 *     -t  print each instruction's address and first halfword before it executes
 *         (after any pending exception has been taken, so it shows what really runs)
 *     --gdb port  wait for arm-none-eabi-gdb on 127.0.0.1:port (0 = any free port);
 *         the debugger controls execution and -n is ignored
 *     --uart-rx text  bytes for USART2 RX from step 0; \r, \n and \\ are escapes.
 *         One byte is offered after each step while RXNE is clear and RE is on
 *     --uart-rx-at step text  the same, but not before `step` (repeatable; queued
 *         in the order given)
 *     --button-press step  press B1 (PC13 low) at `step` for --button-hold steps
 *         (default 20000), then release (repeatable)
 *     --oled-dump file  write the SSD1306 (I2C 0x3C) frame as a PBM image at exit
 *     --flash-out file  write the whole 512 KiB flash at exit (load it again as the
 *         image to model a power cycle that keeps flash)
 *     --gpio-log pins  print level changes of output pins, e.g. PA0,PA1,PB0
 *     --uart-out file  write USART2 TX bytes to this file instead of stdout
 *     --stats  print step, sleep, exception, DMA, flash and watchdog counters at exit
 *     --temperature-centi n, --pressure-pa n, --humidity-percent n  BME280 inputs
 *     --die-temp-centi n, --vdda-mv n  ADC inputs (internal sensor, VDDA)
 *     --iwdg-steps-per-lsi n  steps per LSI period for the IWDG model (default 500)
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
#include "periph/ssd1306.h"
#include "periph/systick.h"
#include "periph/usart.h"
#include "vcd.h"
#include "gdbstub.h"

#define MAX_RX_CHUNKS 16
#define MAX_PRESSES 16
#define MAX_LOG_PINS 16

typedef struct { unsigned long long at; char *text; size_t length; } rx_chunk_t;
typedef struct { unsigned port, pin; } pin_ref_t;

typedef struct {
    cpu_t cpu;
    bus_t *bus;
    rcc_t *rcc;
    gpio_t *gpio[3];
    systick_t *systick;
    usart_t *usart2;
    nvic_t *nvic;
    scb_t *scb;
    i2c_t *i2c;
    bme280_t *bme_i2c, *bme_spi;
    ssd1306_t *oled;
    exti_t *exti;
    spi_t *spi1;
    dma_t *dma1, *dma2;
    adc_t *adc;
    iwdg_t *iwdg;
    flash_if_t *flash;
    vcd_t *vcd;
    FILE *uart_out;
    int trace;
    /* Host input schedule. */
    rx_chunk_t rx[MAX_RX_CHUNKS];
    unsigned rx_count, rx_index;
    size_t rx_offset;
    unsigned long long presses[MAX_PRESSES];
    unsigned press_count;
    unsigned long long button_hold;
    pin_ref_t log_pins[MAX_LOG_PINS];
    unsigned log_pin_count;
    /* Resets requested by IWDG or AIRCR.SYSRESETREQ, handled between steps. */
    uint32_t reset_flags;
    unsigned resets;
    uint64_t total_steps; /* steps across resets */
} machine_t;

static void dump_registers(const cpu_t *cpu)
{
    for (int i = 0; i < 16; i++) {
        printf("r%-2d = 0x%08x%s", i, cpu->r[i], (i % 4 == 3) ? "\n" : "   ");
    }
    printf("N=%d Z=%d C=%d V=%d   steps=%llu\n", cpu->n, cpu->z, cpu->c, cpu->v,
           (unsigned long long)cpu->steps);
}

/* cpu_step increments steps after the peripheral write completes. */
static void observe_gpio(void *ctx, const char *name, uint16_t old_odr, uint16_t new_odr)
{
    machine_t *m = ctx;
    unsigned port = (unsigned)(name[4] - 'A');
    uint64_t step = m->cpu.steps + 1u;
    if (port == 0 && ((old_odr ^ new_odr) & (1u << 5)) != 0) {
        vcd_pa5(m->vcd, step, (new_odr & (1u << 5)) != 0);
        printf("PA5 -> %u at step %llu\n", (unsigned)((new_odr >> 5) & 1u), (unsigned long long)step);
    }
    for (unsigned i = 0; i < m->log_pin_count; ++i) {
        pin_ref_t p = m->log_pins[i];
        if (p.port == port && ((old_odr ^ new_odr) & (1u << p.pin)) && !(port == 0 && p.pin == 5))
            printf("P%c%u -> %u at step %llu\n", 'A' + port, p.pin, (unsigned)((new_odr >> p.pin) & 1u),
                   (unsigned long long)step);
    }
}

static void observe_gpioc_inputs(void *ctx, const char *name, uint16_t old_idr, uint16_t new_idr)
{
    machine_t *m = ctx;
    (void)name;
    exti_gpio_changed(m->exti, 2u, old_idr, new_idr);
}

static void trace_instruction(void *ctx, uint32_t pc, uint16_t first_halfword)
{
    (void)ctx;
    printf("%08x: %04x\n", pc, first_halfword);
}

/* Button B1: PC13, active low; released = high (the board's pull-up). */
static void update_button(machine_t *m)
{
    unsigned long long now = m->cpu.steps;
    bool pressed = false;
    for (unsigned i = 0; i < m->press_count; ++i)
        if (now >= m->presses[i] && now < m->presses[i] + m->button_hold) pressed = true;
    uint16_t levels = gpio_get_inputs(m->gpio[2]);
    uint16_t next = pressed ? (uint16_t)(levels & ~(1u << 13)) : (uint16_t)(levels | (1u << 13));
    if (next != levels) gpio_set_inputs(m->gpio[2], next);
}

/* After each step: one tick for SysTick, DMA and IWDG (steps, not clock
 * cycles), then host inputs. */
static void after_step(void *ctx)
{
    machine_t *m = ctx;
    systick_tick(m->systick);
    dma_tick(m->dma1);
    dma_tick(m->dma2);
    iwdg_tick(m->iwdg);
    if (m->rx_index < m->rx_count) {
        rx_chunk_t *c = &m->rx[m->rx_index];
        if (m->cpu.steps >= c->at) {
            if (m->rx_offset >= c->length) { m->rx_index++; m->rx_offset = 0; }
            else if (usart_receive(m->usart2, (uint8_t)c->text[m->rx_offset])) {
                if (++m->rx_offset >= c->length) { m->rx_index++; m->rx_offset = 0; }
            }
        }
    }
    if (m->press_count) update_button(m);
}

static void pend_systick(void *ctx) { cpu_pend_exception(ctx, CPU_EXCEPTION_SYSTICK); }
/* RM0390 Rev9 table 38 vector positions. */
static void pend_usart2(void *ctx) { nvic_pend(ctx, 38u); }
static void pend_spi1(void *ctx) { nvic_pend(ctx, 35u); }
static void pend_adc(void *ctx) { nvic_pend(ctx, 18u); }
static void pend_exti(void *ctx, unsigned irq) { nvic_pend(ctx, irq); }
static void pend_dma1(void *ctx, unsigned stream) { nvic_pend(ctx, stream < 7u ? 11u + stream : 47u); }
static void pend_dma2(void *ctx, unsigned stream) { nvic_pend(ctx, stream < 5u ? 56u + stream : 68u + (stream - 5u)); }

static void iwdg_expired(void *ctx) { ((machine_t *)ctx)->reset_flags |= RCC_CSR_IWDGRSTF; }
static void sysreset_requested(void *ctx) { ((machine_t *)ctx)->reset_flags |= RCC_CSR_SFTRSTF; }

/* Unescape \r, \n and \\ in place. Returns false for any other escape. */
static bool unescape(char *text, size_t *length)
{
    char *out = text;
    for (const char *in = text; *in; ++in) {
        if (*in != '\\') { *out++ = *in; continue; }
        ++in;
        if (*in == 'r') *out++ = '\r';
        else if (*in == 'n') *out++ = '\n';
        else if (*in == '\\') *out++ = '\\';
        else return false;
    }
    *length = (size_t)(out - text);
    return true;
}

/* USART2 bytes that reach PA2 go to stdout unchanged. */
static void print_usart_byte(void *ctx, uint8_t byte)
{
    machine_t *m = ctx;
    vcd_uart(m->vcd, m->cpu.steps + 1u, byte);
    FILE *out = m->uart_out ? m->uart_out : stdout;
    putc(byte, out);
    if (!m->uart_out) fflush(stdout);
}

static void observe_i2c(void *ctx, i2c_event_t event)
{
    machine_t *m = ctx;
    vcd_i2c(m->vcd, m->cpu.steps + 1u, event);
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

static bool signed_arg(const char *text, long lo, long hi, long *value)
{
    char *end;
    errno = 0;
    long result = strtol(text, &end, 10);
    if (errno || !*text || *end || result < lo || result > hi) return false;
    *value = result;
    return true;
}

static bool parse_pins(char *text, machine_t *m)
{
    for (char *tok = strtok(text, ","); tok; tok = strtok(NULL, ",")) {
        if (m->log_pin_count == MAX_LOG_PINS || tok[0] != 'P' || tok[1] < 'A' || tok[1] > 'C') return false;
        long pin;
        if (!signed_arg(tok + 2, 0, 15, &pin)) return false;
        m->log_pins[m->log_pin_count++] = (pin_ref_t){(unsigned)(tok[1] - 'A'), (unsigned)pin};
    }
    return m->log_pin_count > 0;
}

/* Wire CPU hooks and the interrupt gates. Used at start and after a reset,
 * because cpu_reset() clears them. */
static void wire_cpu(machine_t *m)
{
    cpu_set_step_hook(&m->cpu, after_step, m);
    if (m->trace) cpu_set_trace_hook(&m->cpu, trace_instruction, NULL);
    nvic_reset(m->nvic); /* also reinstalls the enable/priority queries */
}

/* System reset (RM0390 6.1.2): core and peripherals reset, SRAM and flash
 * keep their contents, RCC_CSR reset flags accumulate. External chips (BME280,
 * SSD1306) are not reset. */
static void machine_reset(machine_t *m, uint32_t flags)
{
    m->total_steps += m->cpu.steps;
    rcc_system_reset(m->rcc, flags);
    for (unsigned i = 0; i < 3; ++i) gpio_reset(m->gpio[i]);
    systick_reset(m->systick);
    usart_reset(m->usart2);
    i2c_reset(m->i2c);
    spi_reset(m->spi1);
    dma_reset(m->dma1);
    dma_reset(m->dma2);
    adc_reset(m->adc);
    exti_reset(m->exti);
    iwdg_reset(m->iwdg);
    flash_if_reset(m->flash);
    scb_reset(m->scb);
    cpu_reset(&m->cpu, m->bus);
    wire_cpu(m);
    gpio_set_inputs(m->gpio[2], 1u << 13);
    m->resets++;
}

static int write_file(const char *path, const uint8_t *data, size_t length)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    int ok = fwrite(data, 1, length, f) == length;
    if (fclose(f)) ok = 0;
    if (!ok) perror(path);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    static machine_t m;
    unsigned long long max_steps = 1000000, iwdg_ratio = 500;
    int32_t temperature = 2500, die_temperature = 2500;
    long pressure = 101325, humidity = 50, vdda = 3300;
    bool stats = false;
    const char *path = NULL, *vcd_path = NULL, *regs_path = NULL, *oled_path = NULL, *flash_path = NULL,
               *uart_path = NULL;
    long gdb_port = -1;
    m.button_hold = 20000;
    for (int i = 1; i < argc; i++) {
        long value;
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            if (!unsigned_arg(argv[++i], &max_steps)) goto usage;
        } else if (strcmp(argv[i], "-t") == 0) m.trace = 1;
        else if (strcmp(argv[i], "--vcd") == 0 && i + 1 < argc) vcd_path = argv[++i];
        else if (strcmp(argv[i], "--trace-regs") == 0 && i + 1 < argc) regs_path = argv[++i];
        else if (strcmp(argv[i], "--oled-dump") == 0 && i + 1 < argc) oled_path = argv[++i];
        else if (strcmp(argv[i], "--flash-out") == 0 && i + 1 < argc) flash_path = argv[++i];
        else if (strcmp(argv[i], "--uart-out") == 0 && i + 1 < argc) uart_path = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0) stats = true;
        else if (strcmp(argv[i], "--gpio-log") == 0 && i + 1 < argc) {
            if (!parse_pins(argv[++i], &m)) goto usage;
        } else if ((strcmp(argv[i], "--uart-rx") == 0 && i + 1 < argc) ||
                   (strcmp(argv[i], "--uart-rx-at") == 0 && i + 2 < argc)) {
            unsigned long long at = 0;
            if (argv[i][9] == '-' && !unsigned_arg(argv[++i], &at)) goto usage;
            if (m.rx_count == MAX_RX_CHUNKS) goto usage;
            rx_chunk_t *c = &m.rx[m.rx_count++];
            c->at = at;
            c->text = argv[++i];
            if (!unescape(c->text, &c->length)) goto usage;
        } else if (strcmp(argv[i], "--button-press") == 0 && i + 1 < argc) {
            if (m.press_count == MAX_PRESSES || !unsigned_arg(argv[++i], &m.presses[m.press_count])) goto usage;
            m.press_count++;
        } else if (strcmp(argv[i], "--button-hold") == 0 && i + 1 < argc) {
            if (!unsigned_arg(argv[++i], &m.button_hold) || m.button_hold == 0) goto usage;
        } else if (strcmp(argv[i], "--iwdg-steps-per-lsi") == 0 && i + 1 < argc) {
            if (!unsigned_arg(argv[++i], &iwdg_ratio) || iwdg_ratio == 0 || iwdg_ratio > 1000000) goto usage;
        } else if (strcmp(argv[i], "--gdb") == 0 && i + 1 < argc) {
            unsigned long long port;
            if (!unsigned_arg(argv[++i], &port) || port > 65535) goto usage;
            gdb_port = (long)port;
        } else if (strcmp(argv[i], "--temperature-centi") == 0 && i + 1 < argc) {
            if (!signed_arg(argv[++i], -4000, 8500, &value)) goto usage;
            temperature = (int32_t)value;
        } else if (strcmp(argv[i], "--pressure-pa") == 0 && i + 1 < argc) {
            if (!signed_arg(argv[++i], 30000, 110000, &pressure)) goto usage;
        } else if (strcmp(argv[i], "--humidity-percent") == 0 && i + 1 < argc) {
            if (!signed_arg(argv[++i], 0, 100, &humidity)) goto usage;
        } else if (strcmp(argv[i], "--die-temp-centi") == 0 && i + 1 < argc) {
            if (!signed_arg(argv[++i], -4000, 12500, &value)) goto usage;
            die_temperature = (int32_t)value;
        } else if (strcmp(argv[i], "--vdda-mv") == 0 && i + 1 < argc) {
            if (!signed_arg(argv[++i], 1800, 3600, &vdda)) goto usage;
        } else if (argv[i][0] == '-' || path) goto usage;
        else path = argv[i];
    }
    if (!path) goto usage;
    /* Outputs must not truncate the input image or each other. */
    const char *outputs[] = {vcd_path, regs_path, oled_path, flash_path, uart_path};
    for (unsigned a = 0; a < 5; ++a) {
        if (!outputs[a]) continue;
        if (same_file(path, outputs[a])) goto usage;
        for (unsigned b = a + 1; b < 5; ++b)
            if (outputs[b] && same_file(outputs[a], outputs[b])) goto usage;
    }

    int result = 1;
    FILE *regs = NULL;
    m.bus = bus_new();
    if (!m.bus || load_bin(m.bus, path) != 0) { fprintf(stderr, "cannot load %s\n", path); goto cleanup; }
    {
        /* Flash past the image reads as erased (0xFF), as on a device whose
         * unused sectors were erased before programming. */
        struct stat st;
        if (stat(path, &st) == 0 && (uint64_t)st.st_size < FLASH_SIZE)
            memset(bus_flash_data(m.bus) + st.st_size, 0xFF, FLASH_SIZE - (size_t)st.st_size);
    }
    cpu_reset(&m.cpu, m.bus);
    m.rcc = rcc_new();
    if (m.rcc) {
        m.gpio[0] = gpio_new("GPIOA", GPIOA_BASE, m.rcc, RCC_GPIOA_ENABLE_BIT);
        m.gpio[1] = gpio_new("GPIOB", GPIOB_BASE, m.rcc, RCC_GPIOB_ENABLE_BIT);
        m.gpio[2] = gpio_new("GPIOC", GPIOC_BASE, m.rcc, RCC_GPIOC_ENABLE_BIT);
    }
    m.systick = systick_new(pend_systick, &m.cpu);
    m.usart2 = m.gpio[0] ? usart_new(m.rcc, m.gpio[0]) : NULL;
    m.nvic = nvic_new(&m.cpu);
    m.scb = scb_new(&m.cpu);
    m.i2c = m.gpio[1] ? i2c_new(m.rcc, m.gpio[1]) : NULL;
    m.bme_i2c = bme280_new(0x76);
    m.bme_spi = bme280_new(0x76);
    m.oled = ssd1306_new(0x3C);
    m.exti = m.rcc ? exti_new(m.rcc) : NULL;
    m.spi1 = m.gpio[1] ? spi_new(m.rcc, m.gpio[1]) : NULL;
    m.dma1 = m.rcc ? dma_new(1, m.rcc, m.bus) : NULL;
    m.dma2 = m.rcc ? dma_new(2, m.rcc, m.bus) : NULL;
    m.adc = m.rcc ? adc_new(m.rcc) : NULL;
    m.iwdg = iwdg_new((uint32_t)iwdg_ratio);
    m.flash = flash_if_new(m.bus);
    if (!m.rcc || !m.gpio[0] || !m.gpio[1] || !m.gpio[2] || !m.systick || !m.usart2 || !m.nvic ||
        !m.scb || !m.i2c || !m.bme_i2c || !m.bme_spi || !m.oled || !m.exti || !m.spi1 || !m.dma1 ||
        !m.dma2 || !m.adc || !m.iwdg || !m.flash) {
        fprintf(stderr, "cannot allocate peripheral models\n"); goto cleanup;
    }
    bme280_t *bmes[2] = {m.bme_i2c, m.bme_spi};
    for (unsigned i = 0; i < 2; ++i) {
        bme280_set_temperature(bmes[i], temperature);
        bme280_set_pressure(bmes[i], (uint32_t)pressure);
        bme280_set_humidity(bmes[i], (uint32_t)humidity * 1024u);
    }
    adc_set_die_temperature(m.adc, die_temperature);
    adc_set_vdda(m.adc, (uint32_t)vdda);
    /* BME280 on I2C1 (0x76, the earlier firmware) and a second BME280 on SPI1
     * with chip select on PB6 (the FreeRTOS logger). Same physical inputs. */
    if (i2c_attach(m.i2c, bme280_as_device(m.bme_i2c)) != 0 ||
        i2c_attach(m.i2c, ssd1306_as_device(m.oled)) != 0 ||
        spi_attach(m.spi1, bme280_as_spi_device(m.bme_spi), m.gpio[1], 6) != 0 ||
        dma_connect(m.dma2, 0, 3, spi_dma_rx_request, m.spi1) != 0 ||
        dma_connect(m.dma2, 3, 3, spi_dma_tx_request, m.spi1) != 0 ||
        dma_connect(m.dma1, 6, 4, usart_dma_tx_request, m.usart2) != 0 ||
        dma_connect(m.dma1, 5, 4, usart_dma_rx_request, m.usart2) != 0) {
        fprintf(stderr, "cannot connect peripheral models\n"); goto cleanup;
    }
    const periph_t periphs[] = {
        rcc_as_periph(m.rcc), gpio_as_periph(m.gpio[0]), gpio_as_periph(m.gpio[1]),
        gpio_as_periph(m.gpio[2]), systick_as_periph(m.systick), usart_as_periph(m.usart2),
        nvic_as_periph(m.nvic), scb_as_periph(m.scb), i2c_as_periph(m.i2c),
        exti_as_periph(m.exti), exti_syscfg_as_periph(m.exti), spi_as_periph(m.spi1),
        dma_as_periph(m.dma1), dma_as_periph(m.dma2), adc_as_periph(m.adc),
        adc_sysmem_as_periph(m.adc), iwdg_as_periph(m.iwdg), flash_if_as_periph(m.flash),
    };
    for (unsigned i = 0; i < sizeof periphs / sizeof periphs[0]; ++i)
        if (bus_add_periph(m.bus, periphs[i]) != 0) { fprintf(stderr, "cannot register peripheral models\n"); goto cleanup; }
    if (vcd_path && !(m.vcd = vcd_open(vcd_path))) { perror(vcd_path); goto cleanup; }
    if (uart_path && !(m.uart_out = fopen(uart_path, "wb"))) { perror(uart_path); goto cleanup; }
    if (regs_path) {
        regs = fopen(regs_path, "w");
        if (!regs) { perror(regs_path); goto cleanup; }
        fputs("step,pc", regs);
        for (unsigned r = 0; r < 16; ++r) fprintf(regs, ",r%u", r);
        fputs(",xpsr\n", regs);
        trace_registers(regs, &m.cpu);
    }
    usart_set_irq(m.usart2, pend_usart2, m.nvic);
    spi_set_irq(m.spi1, pend_spi1, m.nvic);
    adc_set_irq(m.adc, pend_adc, m.nvic);
    exti_set_irq(m.exti, pend_exti, m.nvic);
    dma_set_irq(m.dma1, pend_dma1, m.nvic);
    dma_set_irq(m.dma2, pend_dma2, m.nvic);
    iwdg_set_reset(m.iwdg, iwdg_expired, &m);
    scb_set_reset_request(m.scb, sysreset_requested, &m);
    for (unsigned i = 0; i < 3; ++i) gpio_set_observer(m.gpio[i], observe_gpio, &m);
    gpio_set_input_observer(m.gpio[2], observe_gpioc_inputs, &m);
    gpio_set_inputs(m.gpio[2], 1u << 13); /* B1 released */
    usart_set_tx_observer(m.usart2, print_usart_byte, &m);
    i2c_set_observer(m.i2c, observe_i2c, &m);
    wire_cpu(&m);
    if (gdb_port >= 0) {
        if (gdbstub_serve(&m.cpu, m.bus, (uint16_t)gdb_port) != 0) goto cleanup;
    } else {
        while (!m.cpu.halted && m.total_steps + m.cpu.steps < max_steps) {
            uint64_t before = m.cpu.steps;
            cpu_step(&m.cpu, m.bus);
            if (regs && m.cpu.steps != before) trace_registers(regs, &m.cpu);
            if (m.reset_flags && !m.cpu.halted) {
                printf("[emu] %s reset at step %llu\n",
                       (m.reset_flags & RCC_CSR_IWDGRSTF) ? "IWDG" : "software",
                       (unsigned long long)(m.total_steps + m.cpu.steps));
                uint32_t flags = m.reset_flags;
                m.reset_flags = 0;
                machine_reset(&m, flags);
            }
        }
    }
    vcd_end(m.vcd, m.cpu.steps + 1u);
    printf("stopped: %s\n", m.cpu.halted ? m.cpu.halt_reason : "step limit reached");
    dump_registers(&m.cpu);
    if (bus_unmapped_count(m.bus)) printf("note: %u unmapped accesses (last 0x%08x)\n",
                                        bus_unmapped_count(m.bus), bus_last_unmapped_addr(m.bus));
    i2c_stats_t is = i2c_get_stats(m.i2c);
    usart_stats_t us = usart_get_stats(m.usart2);
    spi_stats_t ss = spi_get_stats(m.spi1);
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
    if (ss.clock_gated || ss.pin_mismatch_frames || ss.unselected_frames || ss.overruns ||
        ss.unsupported_config || ss.bad_accesses)
        printf("note: SPI1: clock off=%llu pin mismatch=%llu unselected=%llu overrun=%llu unsupported=%llu bad access=%llu\n",
            (unsigned long long)ss.clock_gated, (unsigned long long)ss.pin_mismatch_frames,
            (unsigned long long)ss.unselected_frames, (unsigned long long)ss.overruns,
            (unsigned long long)ss.unsupported_config, (unsigned long long)ss.bad_accesses);
    dma_stats_t d1 = dma_get_stats(m.dma1), d2 = dma_get_stats(m.dma2);
    if (d1.rejected_config || d1.bad_accesses || d1.clock_gated || d2.rejected_config || d2.bad_accesses || d2.clock_gated)
        printf("note: DMA: rejected config=%llu bad access=%llu clock off=%llu\n",
            (unsigned long long)(d1.rejected_config + d2.rejected_config),
            (unsigned long long)(d1.bad_accesses + d2.bad_accesses),
            (unsigned long long)(d1.clock_gated + d2.clock_gated));
    adc_stats_t as = adc_get_stats(m.adc);
    if (as.sensor_off_conversions || as.unsupported || as.bad_accesses || as.clock_gated)
        printf("note: ADC1: sensor off=%llu unsupported=%llu bad access=%llu clock off=%llu\n",
            (unsigned long long)as.sensor_off_conversions, (unsigned long long)as.unsupported,
            (unsigned long long)as.bad_accesses, (unsigned long long)as.clock_gated);
    flash_stats_t fs = flash_if_get_stats(m.flash);
    if (fs.errors || fs.overwrite_nonerased || fs.bad_accesses)
        printf("note: FLASH: errors=%llu overwrite non-erased=%llu bad access=%llu\n",
            (unsigned long long)fs.errors, (unsigned long long)fs.overwrite_nonerased,
            (unsigned long long)fs.bad_accesses);
    if (rcc_bad_access_count(m.rcc) || gpio_bad_access_count(m.gpio[0]) || gpio_bad_access_count(m.gpio[1]) ||
        gpio_bad_access_count(m.gpio[2]) || systick_bad_access_count(m.systick) || nvic_bad_access_count(m.nvic) ||
        scb_bad_access_count(m.scb) || exti_bad_access_count(m.exti))
        printf("note: bad accesses RCC=%llu GPIOA=%llu GPIOB=%llu GPIOC=%llu SysTick=%llu NVIC=%llu SCB=%llu EXTI/SYSCFG=%llu\n",
            (unsigned long long)rcc_bad_access_count(m.rcc), (unsigned long long)gpio_bad_access_count(m.gpio[0]),
            (unsigned long long)gpio_bad_access_count(m.gpio[1]), (unsigned long long)gpio_bad_access_count(m.gpio[2]),
            (unsigned long long)systick_bad_access_count(m.systick), (unsigned long long)nvic_bad_access_count(m.nvic),
            (unsigned long long)scb_bad_access_count(m.scb), (unsigned long long)exti_bad_access_count(m.exti));
    if (gpio_gated_write_count(m.gpio[0]) || gpio_gated_write_count(m.gpio[1]) || gpio_gated_write_count(m.gpio[2]))
        printf("note: GPIO gated writes A=%llu B=%llu C=%llu\n", (unsigned long long)gpio_gated_write_count(m.gpio[0]),
               (unsigned long long)gpio_gated_write_count(m.gpio[1]), (unsigned long long)gpio_gated_write_count(m.gpio[2]));
    {
        size_t left = 0;
        for (unsigned i = m.rx_index; i < m.rx_count; ++i) left += m.rx[i].length;
        if (m.rx_index < m.rx_count) left -= m.rx_offset;
        if (left) printf("note: %zu UART RX bytes not delivered\n", left);
    }
    if (ssd1306_unknown_bytes(m.oled))
        printf("note: SSD1306 unknown bytes=%llu\n", (unsigned long long)ssd1306_unknown_bytes(m.oled));
    if (stats) {
        uint64_t steps = m.total_steps + m.cpu.steps;
        printf("stats: steps=%llu sleep_steps=%llu (since last reset) resets=%u exceptions=%llu\n",
               (unsigned long long)steps, (unsigned long long)m.cpu.sleep_steps, m.resets,
               (unsigned long long)m.cpu.exceptions_taken);
        printf("stats: dma_items=%llu spi_frames=%llu flash_programmed_bytes=%llu flash_erased_sectors=%llu iwdg_reloads=%llu\n",
               (unsigned long long)(d1.items + d2.items), (unsigned long long)ss.frames,
               (unsigned long long)fs.programmed_bytes, (unsigned long long)fs.erased_sectors,
               (unsigned long long)iwdg_reload_count(m.iwdg));
    }
    result = m.cpu.halted && strncmp(m.cpu.halt_reason, "bkpt #", 6) != 0 ? 1 : 0;
    if (oled_path) {
        FILE *image = fopen(oled_path, "w");
        if (!image || !ssd1306_write_pbm(m.oled, image)) { perror(oled_path); result = 1; }
        if (image && fclose(image)) result = 1;
    }
    if (flash_path && write_file(flash_path, bus_flash_data(m.bus), FLASH_SIZE) != 0) result = 1;
cleanup:
    if (m.uart_out && fclose(m.uart_out)) result = 1;
    if (regs && (ferror(regs) || fflush(regs))) result = 1;
    if (regs && fclose(regs)) result = 1;
    if (!vcd_close(m.vcd)) { fprintf(stderr, "VCD write failed\n"); result = 1; }
    nvic_free(m.nvic); scb_free(m.scb); usart_free(m.usart2); systick_free(m.systick);
    i2c_free(m.i2c); spi_free(m.spi1); bme280_free(m.bme_i2c); bme280_free(m.bme_spi); ssd1306_free(m.oled);
    exti_free(m.exti); dma_free(m.dma1); dma_free(m.dma2); adc_free(m.adc); iwdg_free(m.iwdg);
    flash_if_free(m.flash);
    for (unsigned i = 0; i < 3; ++i) gpio_free(m.gpio[i]);
    rcc_free(m.rcc); bus_free(m.bus);
    return result;
usage:
    fprintf(stderr, "usage: %s [-n max_steps] [-t] [--vcd file] [--trace-regs file] [--gdb port] "
                    "[--uart-rx text] [--uart-rx-at step text] [--button-press step] [--button-hold steps] "
                    "[--oled-dump file] [--flash-out file] [--uart-out file] [--gpio-log PA0,PB1,...] [--stats] "
                    "[--temperature-centi -4000..8500] [--pressure-pa 30000..110000] [--humidity-percent 0..100] "
                    "[--die-temp-centi -4000..12500] [--vdda-mv 1800..3600] [--iwdg-steps-per-lsi n] firmware.bin\n",
            argv[0]);
    return 2;
}
