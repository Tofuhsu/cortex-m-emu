#include "cpu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void cpu_halt(cpu_t *cpu, const char *fmt, ...)
{
    if (cpu->halted) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cpu->halt_reason, sizeof cpu->halt_reason, fmt, ap);
    va_end(ap);
    cpu->halted = true;
}

void cpu_reset(cpu_t *cpu, bus_t *bus)
{
    memset(cpu, 0, sizeof *cpu);
    /*
     * TODO(Jeff): reset sequence (ARMv7-M ARM, section "Reset behavior" in chapter B1).
     *   - SP <- word at address 0x00000000
     *   - PC <- word at address 0x00000004, with bit 0 cleared.
     *           Bit 0 must be 1 (Thumb state); if it is 0, halt: a real Cortex-M would fault.
     *   - LR <- 0xFFFFFFFF
     */
    (void)bus;
}

/*
 * TODO(Jeff): AddWithCarry() from the ARM ARM pseudocode (chapter A2, "Integer arithmetic").
 * ADDS, SUBS and CMP are all built on it:
 *   ADDS: add_with_carry(a, b, false)
 *   SUBS: add_with_carry(a, ~b, true)   (a - b == a + ~b + 1)
 * Return the 32-bit result; set *carry_out and *overflow.
 */
static uint32_t add_with_carry(uint32_t x, uint32_t y, bool carry_in, bool *carry_out, bool *overflow)
{
    (void)x; (void)y; (void)carry_in;
    *carry_out = false;
    *overflow = false;
    return 0;
}

static void exec16(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t insn)
{
    /*
     * TODO(Jeff): decode by the top bits (ARM ARM A5.2, "16-bit Thumb instruction encoding").
     * Start with what tests/progs/ needs, one test at a time.
     */
    (void)bus;
    (void)add_with_carry;
    cpu_halt(cpu, "undefined 16-bit instruction 0x%04x at 0x%08x", insn, pc);
}

static void exec32(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    /* TODO(Jeff): 32-bit Thumb-2 encodings (ARM ARM A5.3). Start with BL, then ORR/BIC/EOR (modified immediate). */
    (void)bus;
    cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
}

int cpu_step(cpu_t *cpu, bus_t *bus)
{
    if (cpu->halted) {
        return -1;
    }

    /*
     * TODO(Jeff): fetch and dispatch.
     *   1. Read the halfword at PC.
     *   2. Decide whether it is the first half of a 32-bit instruction (ARM ARM A5.1:
     *      bits [15:11] are 0b11101, 0b11110 or 0b11111). If so, read the second halfword.
     *   3. Advance PC past the instruction *before* executing it, so branches can overwrite it.
     *      Careful: an instruction that reads PC as an operand sees (its own address + 4).
     *   4. Call exec16() or exec32().
     */
    (void)exec16;
    (void)exec32;
    cpu_halt(cpu, "cpu_step not implemented");

    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
    }
    cpu->steps++;
    return cpu->halted ? -1 : 0;
}
