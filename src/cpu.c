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

/* Interpret a 32-bit two's-complement bit pattern without an out-of-range signed cast. */
static int64_t signed32(uint32_t value)
{
    if ((value & 0x80000000u) != 0) {
        return (int64_t)value - INT64_C(4294967296);
    }
    return (int64_t)value;
}

/*
 * AddWithCarry() from the ARM ARM pseudocode (chapter A2, "Integer arithmetic").
 * ADDS, SUBS and CMP are all built on it:
 *   ADDS: add_with_carry(a, b, false)
 *   SUBS: add_with_carry(a, ~b, true)   (a - b == a + ~b + 1)
 * Return the 32-bit result; set *carry_out and *overflow.
 */
static uint32_t add_with_carry(uint32_t x, uint32_t y, bool carry_in, bool *carry_out, bool *overflow)
{
    uint64_t unsigned_sum = (uint64_t)x + (uint64_t)y + (uint64_t)carry_in;
    int64_t signed_sum = signed32(x) + signed32(y) + (int64_t)carry_in;
    uint32_t result = (uint32_t)unsigned_sum;

    *carry_out = (uint64_t)result != unsigned_sum;
    *overflow = signed32(result) != signed_sum;
    return result;
}

/* ARM ARM A7.3: the 14 conditions used by B<cond> T1. */
static bool condition_passed(const cpu_t *cpu, unsigned cond)
{
    switch (cond) {
    case 0x0: return cpu->z;                         /* EQ */
    case 0x1: return !cpu->z;                        /* NE */
    case 0x2: return cpu->c;                         /* CS/HS */
    case 0x3: return !cpu->c;                        /* CC/LO */
    case 0x4: return cpu->n;                         /* MI */
    case 0x5: return !cpu->n;                        /* PL */
    case 0x6: return cpu->v;                         /* VS */
    case 0x7: return !cpu->v;                        /* VC */
    case 0x8: return cpu->c && !cpu->z;              /* HI */
    case 0x9: return !cpu->c || cpu->z;              /* LS */
    case 0xA: return cpu->n == cpu->v;               /* GE */
    case 0xB: return cpu->n != cpu->v;               /* LT */
    case 0xC: return !cpu->z && (cpu->n == cpu->v);  /* GT */
    case 0xD: return cpu->z || (cpu->n != cpu->v);   /* LE */
    default: return false; /* 0xE and 0xF are not conditions in B<cond> T1. */
    }
}

static void exec16(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t insn)
{
    /*
     * TODO(Jeff): decode by the top bits (ARM ARM A5.2, "16-bit Thumb instruction encoding").
     * Start with what tests/progs/ needs, one test at a time.
     */
    /* MOVS (immediate) T1: 00100 Rd imm8. IT blocks are not supported yet. */
    if ((insn >> 11) == 0x04u) {
        unsigned rd = (insn >> 8) & 0x07u;
        uint32_t result = insn & 0xFFu;
        cpu->r[rd] = result;
        cpu->n = (result >> 31) != 0;
        cpu->z = result == 0;
        /* T1 preserves C and V. */
        return;
    }

    /* ADDS/SUBS (immediate) T2: 00110/00111 Rdn imm8, outside IT blocks. */
    unsigned opcode = insn >> 11;
    if (opcode == 0x06u || opcode == 0x07u) {
        unsigned rdn = (insn >> 8) & 0x07u;
        uint32_t imm = insn & 0xFFu;
        uint32_t result;
        if (opcode == 0x06u) {
            result = add_with_carry(cpu->r[rdn], imm, false, &cpu->c, &cpu->v);
        } else {
            result = add_with_carry(cpu->r[rdn], (uint32_t)~imm, true, &cpu->c, &cpu->v);
        }
        cpu->r[rdn] = result;
        cpu->n = (result >> 31) != 0;
        cpu->z = result == 0;
        return;
    }

    /* LDR (literal) T1: 01001 Rt imm8. PC as an operand is this instruction + 4. */
    if ((insn >> 11) == 0x09u) {
        unsigned rt = (insn >> 8) & 0x07u;
        uint32_t offset = (insn & 0xFFu) << 2;
        uint32_t base = (pc + 4u) & ~3u;
        uint32_t value = bus_read32(bus, base + offset);
        if (!bus_fault(bus)) {
            cpu->r[rt] = value;
        }
        /* Flags are unchanged; cpu_step handles any bus fault. */
        return;
    }

    /* STR/LDR (immediate) T1: 01100/01101 imm5 Rn Rt. */
    if (opcode == 0x0Cu || opcode == 0x0Du) {
        unsigned rn = (insn >> 3) & 0x07u;
        unsigned rt = insn & 0x07u;
        uint32_t offset = ((insn >> 6) & 0x1Fu) << 2;
        uint32_t address = cpu->r[rn] + offset;
        if (opcode == 0x0Du) {
            uint32_t value = bus_read32(bus, address);
            if (!bus_fault(bus)) {
                cpu->r[rt] = value;
            }
        } else {
            bus_write32(bus, address, cpu->r[rt]);
        }
        return;
    }

    /* STR/LDR (immediate) T2, SP-relative: 10010/10011 Rt imm8. */
    if (opcode == 0x12u || opcode == 0x13u) {
        unsigned rt = (insn >> 8) & 0x07u;
        uint32_t offset = (insn & 0xFFu) << 2;
        uint32_t address = cpu->r[SP] + offset;
        if (opcode == 0x13u) {
            uint32_t value = bus_read32(bus, address);
            if (!bus_fault(bus)) {
                cpu->r[rt] = value;
            }
        } else {
            bus_write32(bus, address, cpu->r[rt]);
        }
        return;
    }

    /* ADD (SP plus immediate) T2: 1011 0000 0 imm7. Flags unchanged. */
    if ((insn & 0xFF80u) == 0xB000u) {
        cpu->r[SP] += (insn & 0x7Fu) << 2;
        return;
    }

    /* SUB (SP minus immediate) T1: 1011 0000 1 imm7. Flags unchanged. */
    if ((insn & 0xFF80u) == 0xB080u) {
        cpu->r[SP] -= (insn & 0x7Fu) << 2;
        return;
    }

    /* B<cond> T1: 1101 cond imm8. 0xDE and 0xDF belong to UDF and SVC. */
    unsigned cond = (insn >> 8) & 0x0Fu;
    if ((insn & 0xF000u) == 0xD000u && cond < 0xEu) {
        if (condition_passed(cpu, cond)) {
            uint32_t imm9 = (insn & 0xFFu) << 1;
            int32_t offset = (int32_t)imm9; /* imm9 is at most 511. */
            if ((imm9 & 0x100u) != 0) {
                offset -= 0x200; /* Sign-extend the 9-bit displacement. */
            }
            cpu->r[PC] = pc + 4u + (uint32_t)offset;
        }
        return;
    }

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
