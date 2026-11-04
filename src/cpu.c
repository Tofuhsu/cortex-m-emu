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
    cpu->r[SP] = bus_read32(bus, 0x00000000u);
    uint32_t reset_vector = bus_read32(bus, 0x00000004u);
    cpu->r[PC] = reset_vector & ~1u;
    cpu->r[LR] = 0xFFFFFFFFu;

    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
        return;
    }
    if ((reset_vector & 1u) == 0) {
        cpu_halt(cpu, "reset vector 0x%08x has Thumb bit clear", reset_vector);
    }
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

    /* BKPT T1: 1011 1110 imm8 (ARM ARM, BKPT instruction description). */
    if ((insn & 0xFF00u) == 0xBE00u) {
        unsigned imm = insn & 0xFFu;
        cpu->r[PC] = pc;
        cpu_halt(cpu, "bkpt #%u", imm);
        return;
    }

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

    uint32_t pc = cpu->r[PC];
    uint16_t hw1 = bus_read16(bus, pc);
    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
        return -1;
    }

    /* ARM ARM A5.1: prefixes 11101, 11110 and 11111 start 32-bit instructions. */
    unsigned prefix = hw1 >> 11;
    if (prefix == 0x1Du || prefix == 0x1Eu || prefix == 0x1Fu) {
        uint16_t hw2 = bus_read16(bus, pc + 2u);
        if (bus_fault(bus)) {
            cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
            return -1;
        }
        cpu->r[PC] = pc + 4u;
        exec32(cpu, bus, pc, hw1, hw2);
    } else {
        cpu->r[PC] = pc + 2u;
        exec16(cpu, bus, pc, hw1);
    }

    /* cpu->r[PC] tracks the next instruction. A PC operand uses pc + 4 instead. */

    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
    }
    cpu->steps++;
    return cpu->halted ? -1 : 0;
}