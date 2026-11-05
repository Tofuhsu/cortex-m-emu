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

void cpu_set_step_hook(cpu_t *cpu, cpu_step_hook_fn hook, void *ctx)
{
    cpu->step_hook = hook;
    cpu->step_hook_ctx = ctx;
}

void cpu_set_trace_hook(cpu_t *cpu, cpu_trace_fn trace, void *ctx)
{
    cpu->trace_hook = trace;
    cpu->trace_hook_ctx = ctx;
}

void cpu_set_irq_gate(cpu_t *cpu, cpu_irq_enabled_fn query, void *ctx)
{
    cpu->irq_enabled = query;
    cpu->irq_enabled_ctx = ctx;
}

static bool valid_pending(unsigned number)
{
    return number == CPU_EXCEPTION_SYSTICK ||
           (number >= 16u && number < CPU_EXCEPTION_COUNT);
}

bool cpu_pend_exception(cpu_t *cpu, unsigned number)
{
    if (!valid_pending(number)) return false;
    cpu->pending_exceptions[number / 32u] |= UINT32_C(1) << (number % 32u);
    return true;
}

bool cpu_clear_pending_exception(cpu_t *cpu, unsigned number)
{
    if (!valid_pending(number)) return false;
    cpu->pending_exceptions[number / 32u] &= ~(UINT32_C(1) << (number % 32u));
    return true;
}

bool cpu_exception_pending(const cpu_t *cpu, unsigned number)
{
    return valid_pending(number) &&
           (cpu->pending_exceptions[number / 32u] &
            (UINT32_C(1) << (number % 32u))) != 0;
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

/* Keep pure arithmetic/shift helpers together. DDI0403E.e A7.4.2 and
 * shared Shift_C pseudocode: do not evaluate a C shift by 32 or more.
 */
typedef struct { uint32_t value; bool carry; } shift_result_t;

static void set_nz(cpu_t *cpu, uint32_t value)
{
    cpu->n = (value >> 31) != 0;
    cpu->z = value == 0;
}

static shift_result_t lsl_c(uint32_t value, unsigned amount, bool carry_in)
{
    if (amount == 0) return (shift_result_t){value, carry_in};
    if (amount < 32) return (shift_result_t){value << amount, ((value >> (32u - amount)) & 1u) != 0};
    return (shift_result_t){0, amount == 32 && (value & 1u) != 0};
}

static shift_result_t lsr_c(uint32_t value, unsigned amount, bool carry_in)
{
    if (amount == 0) return (shift_result_t){value, carry_in};
    if (amount < 32) return (shift_result_t){value >> amount, ((value >> (amount - 1u)) & 1u) != 0};
    return (shift_result_t){0, amount == 32 && (value >> 31) != 0};
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

#define XPSR_T         (UINT32_C(1) << 24)
#define XPSR_STACK_PAD (UINT32_C(1) << 9)
#define XPSR_IPSR_MASK UINT32_C(0x1FF)
#define EXC_RETURN_THREAD_MSP UINT32_C(0xFFFFFFF9)

/* Only NZCV, Thumb state and IPSR are modeled; IT/ICI and FP state are absent. */
uint32_t cpu_get_xpsr(const cpu_t *cpu)
{
    return ((uint32_t)cpu->n << 31) | ((uint32_t)cpu->z << 30)
         | ((uint32_t)cpu->c << 29) | ((uint32_t)cpu->v << 28)
         | XPSR_T | (cpu->ipsr & XPSR_IPSR_MASK);
}

/* Basic frame on MSP, with fixed STKALIGN=1 and vector-table base=0. */
static bool exception_enter(cpu_t *cpu, bus_t *bus, unsigned number,
                             uint32_t return_address)
{
    if (cpu->ipsr != 0) {
        cpu_halt(cpu, "nested exception entry not implemented");
        return false;
    }
    uint32_t old_sp = cpu->r[SP];
    if ((old_sp & 3u) != 0) {
        cpu_halt(cpu, "exception entry with unaligned SP 0x%08x", old_sp);
        return false;
    }
    uint32_t handler = bus_read32(bus, number * 4u);
    if (bus_fault(bus)) {
        return false;
    }
    if ((handler & 1u) == 0) {
        cpu_halt(cpu, "exception %u vector 0x%08x has Thumb bit clear", number, handler);
        return false;
    }

    uint32_t frame_sp = (old_sp - 32u) & ~7u;
    uint32_t psr = cpu_get_xpsr(cpu);
    if ((old_sp & 4u) != 0) {
        psr |= XPSR_STACK_PAD;
    }
    const uint32_t frame[8] = {
        cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
        cpu->r[12], cpu->r[LR], return_address, psr,
    };
    for (unsigned i = 0; i < 8; i++) {
        bus_write32(bus, frame_sp + 4u * i, frame[i]);
        if (bus_fault(bus)) {
            return false;
        }
    }
    /* Commit core state after successful transfers. Padding is not written. */
    cpu->r[SP] = frame_sp;
    cpu->r[LR] = EXC_RETURN_THREAD_MSP;
    cpu->ipsr = (uint16_t)number;
    cpu->r[PC] = handler & ~1u;
    return true;
}

static void exception_return(cpu_t *cpu, bus_t *bus, uint32_t target)
{
    if (target != EXC_RETURN_THREAD_MSP) {
        cpu_halt(cpu, "EXC_RETURN 0x%08x not implemented (only Thread/MSP basic frame)", target);
        return;
    }
    uint32_t frame_sp = cpu->r[SP];
    if ((frame_sp & 3u) != 0) {
        cpu_halt(cpu, "exception return with unaligned SP 0x%08x", frame_sp);
        return;
    }
    uint32_t frame[8];
    for (unsigned i = 0; i < 8; i++) {
        frame[i] = bus_read32(bus, frame_sp + 4u * i);
        if (bus_fault(bus)) {
            return;
        }
    }
    uint32_t psr = frame[7];
    /* The stacked PC is even: its Thumb state is stored separately in xPSR.T. */
    if ((psr & XPSR_T) == 0 || (frame[6] & 1u) != 0 ||
        (psr & XPSR_IPSR_MASK) != 0) {
        cpu_halt(cpu, "invalid Thread exception frame (PC=0x%08x xPSR=0x%08x)",
                 frame[6], psr);
        return;
    }
    if ((psr & 0x0600FC00u) != 0) {
        cpu_halt(cpu, "exception return with IT/ICI state not implemented");
        return;
    }
    cpu->r[0] = frame[0];
    cpu->r[1] = frame[1];
    cpu->r[2] = frame[2];
    cpu->r[3] = frame[3];
    cpu->r[12] = frame[4];
    cpu->r[LR] = frame[5];
    cpu->r[PC] = frame[6];
    cpu->r[SP] = (frame_sp + 32u) | ((psr & XPSR_STACK_PAD) != 0 ? 4u : 0u);
    cpu->n = (psr >> 31) != 0;
    cpu->z = ((psr >> 30) & 1u) != 0;
    cpu->c = ((psr >> 29) & 1u) != 0;
    cpu->v = ((psr >> 28) & 1u) != 0;
    cpu->ipsr = (uint16_t)(psr & XPSR_IPSR_MASK);
}

/* BX and POP PC can perform exception return; MOV PC does not use this path. */
static void write_pc_thumb(cpu_t *cpu, bus_t *bus, uint32_t target)
{
    if (cpu->ipsr != 0 && (target & 0xF0000000u) == 0xF0000000u) {
        exception_return(cpu, bus, target);
        return;
    }
    if ((target & 1u) == 0) {
        cpu_halt(cpu, "branch target 0x%08x has Thumb bit clear", target);
        return;
    }
    cpu->r[PC] = target & ~1u;
}

/* Shared scalar transfers. Address calculation belongs to each instruction decoder. */
static void transfer_memory(cpu_t *cpu, bus_t *bus, unsigned rt, uint32_t address,
                            unsigned size, bool load)
{
    uint32_t value = 0;
    switch (size) {
    case 1:
        if (load) value = bus_read8(bus, address);
        else bus_write8(bus, address, (uint8_t)cpu->r[rt]);
        break;
    case 2:
        if (load) value = bus_read16(bus, address);
        else bus_write16(bus, address, (uint16_t)cpu->r[rt]);
        break;
    case 4:
        if (load) value = bus_read32(bus, address);
        else bus_write32(bus, address, cpu->r[rt]);
        break;
    default:
        cpu_halt(cpu, "unsupported memory transfer width %u", size);
        return;
    }
    if (load && !bus_fault(bus)) {
        cpu->r[rt] = value;
    }
}

static void exec16(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t insn)
{
    /* Decode only implemented encodings; IT blocks are not modeled yet. */

    /* CPS T1: only CPSIE i / CPSID i. This model currently runs privileged;
     * CONTROL.nPRIV and ignored unprivileged CPS writes are future work.
     */
    if (insn == 0xB662u || insn == 0xB672u) {
        cpu->primask = insn == 0xB672u;
        return;
    }
    if ((insn & 0xFFE0u) == 0xB660u) {
        if ((insn & 0x000Cu) != 0 || (insn & 3u) == 0) {
            cpu_halt(cpu, "UNPREDICTABLE CPS encoding 0x%04x at 0x%08x", insn, pc);
        } else {
            cpu_halt(cpu, "CPS FAULTMASK update not implemented at 0x%08x", pc);
        }
        return;
    }

    /* LSLS/LSRS immediate T1. LSL #0 is MOVS register T2, preserving C;
     * the zero LSR immediate encodes 32, not zero (A7.4.2 DecodeImmShift).
     * ASR immediate is deliberately still unsupported.
     */
    if ((insn & 0xF800u) == 0 || (insn & 0xF800u) == 0x0800u) {
        unsigned rd = insn & 7u;
        unsigned rm = (insn >> 3) & 7u;
        unsigned amount = (insn >> 6) & 31u;
        bool right = (insn & 0x0800u) != 0;
        if (right && amount == 0) amount = 32;
        shift_result_t result = right ? lsr_c(cpu->r[rm], amount, cpu->c)
                                      : lsl_c(cpu->r[rm], amount, cpu->c);
        cpu->r[rd] = result.value;
        set_nz(cpu, result.value);
        cpu->c = result.carry; /* V is unchanged. */
        return;
    }

    /* UXTB T1: 10110010 11 Rm Rd. Other extension opcodes remain unsupported. */
    if ((insn & 0xFFC0u) == 0xB2C0u) {
        unsigned rd = insn & 7u;
        unsigned rm = (insn >> 3) & 7u;
        cpu->r[rd] = cpu->r[rm] & 0xFFu;
        return;
    }

    /* SUBS (register) T1: 0001101 Rm Rn Rd, outside IT blocks. */
    if ((insn & 0xFE00u) == 0x1A00u) {
        unsigned rd = insn & 0x07u;
        unsigned rn = (insn >> 3) & 0x07u;
        unsigned rm = (insn >> 6) & 0x07u;
        uint32_t result = add_with_carry(cpu->r[rn], ~cpu->r[rm], true,
                                         &cpu->c, &cpu->v);
        cpu->r[rd] = result;
        cpu->n = (result >> 31) != 0;
        cpu->z = result == 0;
        return;
    }

    /* ADDS (immediate) T1: 0001110 imm3 Rn Rd. */
    if ((insn & 0xFE00u) == 0x1C00u) {
        unsigned rd = insn & 0x07u;
        unsigned rn = (insn >> 3) & 0x07u;
        uint32_t imm = (insn >> 6) & 0x07u;
        uint32_t result = add_with_carry(cpu->r[rn], imm, false,
                                         &cpu->c, &cpu->v);
        cpu->r[rd] = result;
        cpu->n = (result >> 31) != 0;
        cpu->z = result == 0;
        return;
    }

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

    unsigned opcode = insn >> 11;

    /* CMP (immediate) T1: 00101 Rn imm8. Update NZCV without register writeback. */
    if (opcode == 0x05u) {
        unsigned rn = (insn >> 8) & 0x07u;
        uint32_t imm = insn & 0xFFu;
        uint32_t result = add_with_carry(cpu->r[rn], (uint32_t)~imm, true,
                                         &cpu->c, &cpu->v);
        cpu->n = (result >> 31) != 0;
        cpu->z = result == 0;
        return;
    }

    /* ADDS/SUBS (immediate) T2: 00110/00111 Rdn imm8, outside IT blocks. */
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

    /* A5.2.2: 010000 opcode(4) Rm(3) Rdn/Rn(3). */
    if ((insn & 0xFC00u) == 0x4000u) {
        unsigned op = (insn >> 6) & 0x0Fu;
        unsigned rm = (insn >> 3) & 0x07u;
        unsigned rn = insn & 0x07u;
        uint32_t result;
        switch (op) {
        case 0x0u: /* ANDS T1 has implicit LSL #0: C/V unchanged. */
            result = cpu->r[rn] & cpu->r[rm]; cpu->r[rn] = result; break;
        case 0x1u: /* EORS T1. */
            result = cpu->r[rn] ^ cpu->r[rm]; cpu->r[rn] = result; break;
        case 0x9u: /* RSBS immediate T1 (#0), alias NEGS. */
            result = add_with_carry(~cpu->r[rm], 0, true, &cpu->c, &cpu->v);
            cpu->r[rn] = result;
            break;
        case 0xAu: /* CMP register: flags only. */
            result = add_with_carry(cpu->r[rn], ~cpu->r[rm], true, &cpu->c, &cpu->v);
            break;
        case 0xCu: /* ORRS T1. */
            result = cpu->r[rn] | cpu->r[rm]; cpu->r[rn] = result; break;
        case 0xFu: /* MVNS T1, implicit LSL #0. */
            result = ~cpu->r[rm]; cpu->r[rn] = result; break;
        default:
            cpu_halt(cpu, "undefined 16-bit instruction 0x%04x at 0x%08x", insn, pc);
            return;
        }
        set_nz(cpu, result);
        return;
    }

    /* ADD (register) T2, DDI0403E.e A7.7.4 (A7-192), including the
     * SP aliases in A7.7.6 (A7-196). 0x44xx and MOV's 0x46xx are disjoint.
     * A5.1.3 (A5-127): writing nonzero SP[1:0] is UNPREDICTABLE.
     */
    if ((insn & 0xFF00u) == 0x4400u) {
        unsigned rd = (insn & 0x07u) | ((insn >> 4) & 0x08u);
        unsigned rm = (insn >> 3) & 0x0Fu;
        if (rd == PC && rm == PC) {
            cpu_halt(cpu, "UNPREDICTABLE ADD with both operands PC at 0x%08x", pc);
            return;
        }
        uint32_t left = rd == PC ? pc + 4u : cpu->r[rd];
        uint32_t right = rm == PC ? pc + 4u : cpu->r[rm];
        uint32_t result = left + right;
        if (rd == SP && (result & 3u) != 0) {
            cpu_halt(cpu, "UNPREDICTABLE ADD to unaligned SP at 0x%08x", pc);
            return;
        }
        /* ALUWritePC/BranchWritePC, like MOV PC, not BX interworking. */
        cpu->r[rd] = rd == PC ? result & ~1u : result;
        return;
    }

    /* MOV (register) T1: 01000110 D Rm(4) Rd(3). Flags unchanged. */
    if ((insn & 0xFF00u) == 0x4600u) {
        unsigned rd = (insn & 0x07u) | ((insn >> 4) & 0x08u);
        unsigned rm = (insn >> 3) & 0x0Fu;
        uint32_t result = rm == PC ? pc + 4u : cpu->r[rm];
        if (rd == SP && (result & 3u) != 0) {
            cpu_halt(cpu, "UNPREDICTABLE MOV to unaligned SP at 0x%08x", pc);
            return;
        }
        /* ALUWritePC is BranchWritePC: discard bit 0, without interworking. */
        cpu->r[rd] = rd == PC ? result & ~1u : result;
        return;
    }

    /* BX T1: 0100 0111 0 Rm 000. BLX (bit 7 set) is a different instruction. */
    if ((insn & 0xFF87u) == 0x4700u) {
        unsigned rm = (insn >> 3) & 0x0Fu;
        uint32_t target = rm == PC ? pc + 4u : cpu->r[rm];
        write_pc_thumb(cpu, bus, target);
        return;
    }

    /* LDR (literal) T1: 01001 Rt imm8. PC as an operand is this instruction + 4. */
    if ((insn >> 11) == 0x09u) {
        unsigned rt = (insn >> 8) & 0x07u;
        uint32_t offset = (insn & 0xFFu) << 2;
        uint32_t base = (pc + 4u) & ~3u;
        transfer_memory(cpu, bus, rt, base + offset, 4, true);
        return;
    }

    /* STR/LDR (immediate) T1: 01100/01101 imm5 Rn Rt. */
    if (opcode == 0x0Cu || opcode == 0x0Du) {
        unsigned rn = (insn >> 3) & 0x07u;
        unsigned rt = insn & 0x07u;
        uint32_t offset = ((insn >> 6) & 0x1Fu) << 2;
        uint32_t address = cpu->r[rn] + offset;
        transfer_memory(cpu, bus, rt, address, 4, opcode == 0x0Du);
        return;
    }

    /* STRB/LDRB (immediate) T1: 01110/01111 imm5 Rn Rt. Byte offset, not scaled;
     * LDRB zero-extends the loaded byte.
     */
    if (opcode == 0x0Eu || opcode == 0x0Fu) {
        unsigned rn = (insn >> 3) & 0x07u;
        unsigned rt = insn & 0x07u;
        uint32_t address = cpu->r[rn] + ((insn >> 6) & 0x1Fu);
        transfer_memory(cpu, bus, rt, address, 1, opcode == 0x0Fu);
        return;
    }

    /* Load/store (register) T1: 0101 opB Rm Rn Rt, address = Rn + Rm.
     * opB: STR, STRH, STRB, LDRSB, LDR, LDRH, LDRB, LDRSH. No shift in T1.
     */
    if ((insn & 0xF000u) == 0x5000u) {
        unsigned op = (insn >> 9) & 0x07u;
        unsigned rm = (insn >> 6) & 0x07u;
        unsigned rn = (insn >> 3) & 0x07u;
        unsigned rt = insn & 0x07u;
        uint32_t address = cpu->r[rn] + cpu->r[rm];
        static const unsigned sizes[8] = {4, 2, 1, 1, 4, 2, 1, 2};
        bool load = op >= 3u;
        transfer_memory(cpu, bus, rt, address, sizes[op], load);
        if (!bus_fault(bus)) {
            if (op == 3u) cpu->r[rt] = (uint32_t)(int32_t)(int8_t)(uint8_t)cpu->r[rt];
            if (op == 7u) cpu->r[rt] = (uint32_t)(int32_t)(int16_t)(uint16_t)cpu->r[rt];
        }
        return;
    }

    /* STR/LDR (immediate) T2, SP-relative: 10010/10011 Rt imm8. */
    if (opcode == 0x12u || opcode == 0x13u) {
        unsigned rt = (insn >> 8) & 0x07u;
        uint32_t offset = (insn & 0xFFu) << 2;
        uint32_t address = cpu->r[SP] + offset;
        transfer_memory(cpu, bus, rt, address, 4, opcode == 0x13u);
        return;
    }

    /* ADD (SP plus immediate) T1: 10101 Rd imm8. Flags unchanged. */
    if (opcode == 0x15u) {
        unsigned rd = (insn >> 8) & 0x07u;
        cpu->r[rd] = cpu->r[SP] + ((insn & 0xFFu) << 2);
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

    /* PUSH T1: 1011 010 M reglist. Lower register numbers use lower addresses. */
    if ((insn & 0xFE00u) == 0xB400u) {
        uint32_t registers = insn & 0xFFu;
        if ((insn & 0x100u) != 0) {
            registers |= 1u << LR;
        }
        unsigned count = 0;
        for (uint32_t bits = registers; bits != 0; bits >>= 1) {
            count += bits & 1u;
        }
        if (count == 0) {
            cpu_halt(cpu, "empty PUSH register list");
            return;
        }
        uint32_t new_sp = cpu->r[SP] - 4u * count;
        uint32_t address = new_sp;
        for (unsigned r = 0; r <= LR; r++) {
            if ((registers & (1u << r)) != 0) {
                bus_write32(bus, address, cpu->r[r]);
                if (bus_fault(bus)) {
                    return;
                }
                address += 4u;
            }
        }
        /* Write back SP only after every selected store has succeeded. */
        cpu->r[SP] = new_sp;
        return;
    }

    /* POP T1: 1011 110 P reglist. The optional PC is the last word on the stack. */
    if ((insn & 0xFE00u) == 0xBC00u) {
        uint32_t registers = insn & 0xFFu;
        if ((insn & 0x100u) != 0) {
            registers |= 1u << PC;
        }
        if (registers == 0) {
            cpu_halt(cpu, "empty POP register list");
            return;
        }
        uint32_t address = cpu->r[SP];
        uint32_t pc_target = 0;
        for (unsigned r = 0; r <= PC; r++) {
            if ((registers & (1u << r)) != 0) {
                uint32_t value = bus_read32(bus, address);
                if (bus_fault(bus)) {
                    return;
                }
                if (r == PC) {
                    pc_target = value;
                } else {
                    cpu->r[r] = value;
                }
                address += 4u;
            }
        }
        cpu->r[SP] = address;
        if ((registers & (1u << PC)) != 0) {
            write_pc_thumb(cpu, bus, pc_target);
        }
        return;
    }

    /* SVC T1: 1101 1111 imm8. Stack the NEXT instruction, not pc+4. */
    if ((insn & 0xFF00u) == 0xDF00u) {
        if (cpu->primask) {
            cpu_halt(cpu, "SVC with PRIMASK=1 escalates to HardFault (not implemented)");
            return;
        }
        (void)exception_enter(cpu, bus, 11u, pc + 2u);
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

    /* B (unconditional) T2: 11100 imm11. Flags unchanged. */
    if ((insn & 0xF800u) == 0xE000u) {
        uint32_t imm12 = (insn & 0x07FFu) << 1;
        int32_t offset = (int32_t)imm12; /* imm12 is at most 4094. */
        if ((imm12 & 0x0800u) != 0) {
            offset -= 0x1000; /* Sign-extend the 12-bit displacement. */
        }
        cpu->r[PC] = pc + 4u + (uint32_t)offset;
        return;
    }

    /* NOP T1 only. Other 0xBFxx encodings include IT and other hints. */
    if (insn == 0xBF00u) {
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

static uint32_t ror32(uint32_t value, unsigned amount)
{
    amount &= 31u;
    if (amount == 0) {
        return value;
    }
    return (value >> amount) | (value << (32u - amount));
}

typedef struct {
    uint32_t value;
    bool carry;
} thumb_imm_t;

/* ARM ARM A5.3.2, ThumbExpandImm_C. False means an invalid/UNPREDICTABLE encoding. */
static bool thumb_expand_imm(uint16_t imm12, bool carry_in, thumb_imm_t *expanded)
{
    if (imm12 > 0x0FFFu) {
        return false;
    }
    thumb_imm_t result = { .value = 0, .carry = carry_in };
    if ((imm12 & 0x0C00u) == 0) {
        unsigned pattern = (imm12 >> 8) & 0x03u;
        uint32_t imm8 = imm12 & 0xFFu;
        if (pattern != 0 && imm8 == 0) {
            return false;
        }
        switch (pattern) {
        case 0: result.value = imm8; break;
        case 1: result.value = (imm8 << 16) | imm8; break;
        case 2: result.value = (imm8 << 24) | (imm8 << 8); break;
        case 3: result.value = (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8; break;
        }
    } else {
        uint32_t unrotated = 0x80u | (imm12 & 0x7Fu);
        result.value = ror32(unrotated, imm12 >> 7);
        result.carry = (result.value >> 31) != 0;
    }
    *expanded = result;
    return true;
}

static void exec32(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    /* MSR PRIMASK T1, ARM ARM B5.2.3. Needed by the I2C driver's
     * save/restore critical windows; this core is privileged only.
     */
    if ((hw1 & 0xFFF0u) == 0xF380u && hw2 == 0x8810u) {
        unsigned rn = hw1 & 0x0Fu;
        if (rn == SP || rn == PC) cpu_halt(cpu, "UNPREDICTABLE MSR PRIMASK register");
        else cpu->primask = (cpu->r[rn] & 1u) != 0;
        return;
    }

    /* MRS T1: 1111 0011 1110 1111 / 1000 Rd SYSm. */
    if (hw1 == 0xF3EFu && (hw2 & 0xF000u) == 0x8000u) {
        unsigned rd = (hw2 >> 8) & 0x0Fu;
        unsigned sysm = hw2 & 0xFFu;
        if (rd == SP || rd == PC) {
            cpu_halt(cpu, "UNPREDICTABLE MRS Rd=%u at 0x%08x", rd, pc);
            return;
        }
        if (sysm == 5u) {
            cpu->r[rd] = cpu->ipsr & XPSR_IPSR_MASK;
        } else if (sysm == 16u) {
            cpu->r[rd] = cpu->primask ? 1u : 0u;
        } else {
            cpu_halt(cpu, "MRS SYSm 0x%02x not implemented at 0x%08x", sysm, pc);
            return;
        }
        return;
    }

    /* B<cond> T3, A7.7.12 (A7-205): raw J2/J1 bits, unlike BL.
     * Conditions 14/15 belong to other encodings. MRS/MSR decode above.
     */
    if ((hw1 & 0xF800u) == 0xF000u && (hw2 & 0xD000u) == 0x8000u &&
        ((hw1 >> 6) & 15u) < 14u) {
        unsigned cond = (hw1 >> 6) & 15u;
        if (condition_passed(cpu, cond)) {
            uint32_t displacement = ((uint32_t)(hw1 & 0x0400u) << 10)
                | ((uint32_t)(hw2 & 0x0800u) << 8)
                | ((uint32_t)(hw2 & 0x2000u) << 5)
                | ((uint32_t)(hw1 & 0x003Fu) << 12)
                | ((uint32_t)(hw2 & 0x07FFu) << 1);
            if (displacement & 0x00100000u) displacement |= 0xFFE00000u;
            cpu->r[PC] = pc + 4u + displacement;
        }
        return;
    }

    /* LSR register T2: only LSR of this family is implemented this round.
     * Register shift uses its low BYTE; zero is genuinely zero here.
     */
    if ((hw1 & 0xFFE0u) == 0xFA20u && (hw2 & 0xF0F0u) == 0xF000u) {
        unsigned rn = hw1 & 15u, rd = (hw2 >> 8) & 15u, rm = hw2 & 15u;
        if (rn == SP || rn == PC || rd == SP || rd == PC || rm == SP || rm == PC) {
            cpu_halt(cpu, "UNPREDICTABLE LSR register operand at 0x%08x", pc);
            return;
        }
        shift_result_t result = lsr_c(cpu->r[rn], cpu->r[rm] & 255u, cpu->c);
        cpu->r[rd] = result.value;
        if (hw1 & 0x10u) { set_nz(cpu, result.value); cpu->c = result.carry; }
        return;
    }

    /* MUL T2 and UDIV T1. The second-halfword masks exclude MLA and
     * adjacent multiply/divide operations. SP/PC operands are forbidden.
     */
    bool multiply = (hw1 & 0xFFF0u) == 0xFB00u && (hw2 & 0xF0F0u) == 0xF000u;
    bool divide = (hw1 & 0xFFF0u) == 0xFBB0u && (hw2 & 0xF0F0u) == 0xF0F0u;
    if (multiply || divide) {
        unsigned rn = hw1 & 15u, rd = (hw2 >> 8) & 15u, rm = hw2 & 15u;
        if (rn == SP || rn == PC || rd == SP || rd == PC || rm == SP || rm == PC) {
            cpu_halt(cpu, "UNPREDICTABLE multiply/divide register at 0x%08x", pc);
            return;
        }
        /* DDI0403E.e A4.4.6/B3.2.8: DIV_0_TRP resets to 0. This model
         * keeps that fixed reset policy; SCB/CCR configuration is absent.
         * A zero divisor must never reach the host C division operator.
         */
        cpu->r[rd] = multiply ? cpu->r[rn] * cpu->r[rm]
                            : cpu->r[rm] == 0 ? 0 : cpu->r[rn] / cpu->r[rm];
        return; /* No NZCV changes. */
    }

    /* BL T1: 11110 S imm10 / 11 J1 1 J2 imm11. */
    if ((hw1 & 0xF800u) == 0xF000u && (hw2 & 0xD000u) == 0xD000u) {
        uint32_t s = (hw1 >> 10) & 1u;
        uint32_t j1 = (hw2 >> 13) & 1u;
        uint32_t j2 = (hw2 >> 11) & 1u;
        uint32_t i1 = 1u ^ (j1 ^ s);
        uint32_t i2 = 1u ^ (j2 ^ s);
        uint32_t imm25 = (s << 24) | (i1 << 23) | (i2 << 22)
                       | ((uint32_t)(hw1 & 0x03FFu) << 12)
                       | ((uint32_t)(hw2 & 0x07FFu) << 1);
        int32_t offset = (int32_t)imm25; /* imm25 fits in int32_t. */
        if (s != 0) {
            offset -= INT32_C(33554432);
        }
        cpu->r[LR] = (pc + 4u) | 1u;
        cpu->r[PC] = pc + 4u + (uint32_t)offset;
        return;
    }

    /* MOVW, MOV (immediate) T3: 11110 i 10 0100 imm4 / 0 imm3 Rd imm8. */
    if ((hw1 & 0xFBF0u) == 0xF240u && (hw2 & 0x8000u) == 0) {
        unsigned rd = (hw2 >> 8) & 0x0Fu;
        if (rd == SP || rd == PC) {
            cpu_halt(cpu, "UNPREDICTABLE MOVW Rd=%u at 0x%08x", rd, pc);
            return;
        }
        uint32_t imm16 = ((uint32_t)(hw1 & 0x0Fu) << 12)
                       | ((uint32_t)(hw1 & 0x0400u) << 1)
                       | ((uint32_t)(hw2 & 0x7000u) >> 4)
                       | (hw2 & 0x00FFu);
        cpu->r[rd] = imm16; /* Zero-extend; preserve all NZCV flags. */
        return;
    }

    /* LDR literal T2, A7.7.44: recognize BOTH U=0 and U=1 before the
     * generic imm12 load/store family. PC operand is instruction+4, aligned.
     */
    if ((hw1 & 0xFF7Fu) == 0xF85Fu) {
        unsigned rt = (hw2 >> 12) & 15u;
        if (rt == PC) {
            cpu_halt(cpu, "LDR literal T2 to PC not implemented at 0x%08x", pc);
            return;
        }
        uint32_t base = (pc + 4u) & ~3u;
        uint32_t offset = hw2 & 0x0FFFu;
        uint32_t address = hw1 & 0x0080u ? base + offset : base - offset;
        if (rt == SP) {
            uint32_t value = bus_read32(bus, address);
            if (bus_fault(bus)) return;
            if (value & 3u) cpu_halt(cpu, "UNPREDICTABLE LDR literal to unaligned SP at 0x%08x", pc);
            else cpu->r[SP] = value;
        } else transfer_memory(cpu, bus, rt, address, 4, true);
        return;
    }

    /* STR.W/LDR.W, STRB.W/LDRB.W, STRH.W/LDRH.W with an unscaled, unsigned
     * imm12 byte offset: 1111 1000 1 size L Rn / Rt imm12. */
    unsigned memory_op = hw1 & 0xFFF0u;
    if (memory_op == 0xF8C0u || memory_op == 0xF8D0u || memory_op == 0xF880u ||
        memory_op == 0xF890u || memory_op == 0xF8A0u || memory_op == 0xF8B0u) {
        unsigned rn = hw1 & 0x0Fu;
        unsigned rt = (hw2 >> 12) & 0x0Fu;
        uint32_t offset = hw2 & 0x0FFFu;
        if (rn == PC || rt == PC) {
            cpu_halt(cpu, "wide load/store with PC not implemented at 0x%08x", pc);
            return;
        }
        unsigned size = (memory_op & 0x0060u) == 0x0040u ? 4u : (memory_op & 0x0020u) ? 2u : 1u;
        bool load = (memory_op & 0x0010u) != 0;
        if (size != 4u && rt == SP) {
            cpu_halt(cpu, "UNPREDICTABLE byte/halfword load/store Rt=SP at 0x%08x", pc);
            return;
        }
        uint32_t address = cpu->r[rn] + offset;
        transfer_memory(cpu, bus, rt, address, size, load);
        return;
    }

    /* STR/LDR, STRB/LDRB, STRH/LDRH (immediate) with an 8-bit offset (the T4/T3
     * encodings): 1111 1000 0 size L Rn / Rt 1 P U W imm8. Covers negative
     * offsets, pre-index and post-index with writeback. LDR literal (Rn = PC)
     * is decoded above.
     */
    if ((memory_op == 0xF800u || memory_op == 0xF810u || memory_op == 0xF820u ||
         memory_op == 0xF830u || memory_op == 0xF840u || memory_op == 0xF850u) &&
        (hw2 & 0x0800u) != 0) {
        unsigned rn = hw1 & 0x0Fu;
        unsigned rt = (hw2 >> 12) & 0x0Fu;
        bool index = (hw2 & 0x0400u) != 0, add = (hw2 & 0x0200u) != 0;
        bool wback = (hw2 & 0x0100u) != 0;
        bool load = (hw1 & 0x0010u) != 0;
        unsigned size = (hw1 & 0x0040u) ? 4u : (hw1 & 0x0020u) ? 2u : 1u;
        uint32_t imm8 = hw2 & 0xFFu;
        if (!index && !wback) {
            cpu_halt(cpu, "UNDEFINED load/store imm8 P=0 W=0 at 0x%08x", pc);
            return;
        }
        if (index && add && !wback) {
            cpu_halt(cpu, "unprivileged LDRT/STRT not implemented at 0x%08x", pc);
            return;
        }
        if (rn == PC || rt == PC || (size != 4u && rt == SP) || (wback && rn == rt)) {
            cpu_halt(cpu, "load/store imm8 with this Rn/Rt not implemented at 0x%08x", pc);
            return;
        }
        uint32_t offset_address = add ? cpu->r[rn] + imm8 : cpu->r[rn] - imm8;
        uint32_t address = index ? offset_address : cpu->r[rn];
        transfer_memory(cpu, bus, rt, address, size, load);
        if (wback && !bus_fault(bus)) cpu->r[rn] = offset_address;
        return;
    }

    /* Modified immediate: 11110 i 0 op S Rn / 0 imm3 Rd imm8. BL is decoded first. */
    if ((hw1 & 0xFA00u) == 0xF000u && (hw2 & 0x8000u) == 0) {
        unsigned op = (hw1 >> 5) & 0x0Fu;
        unsigned rn = hw1 & 0x0Fu;
        unsigned rd = (hw2 >> 8) & 0x0Fu;
        bool setflags = (hw1 & 0x10u) != 0;
        bool arithmetic = op == 8u || op == 13u || op == 14u;
        bool compare = setflags && rd == PC && (op == 8u || op == 13u);
        bool sp_alias = rn == SP && (op == 8u || op == 13u);
        if (op != 0u && op != 1u && op != 2u && op != 3u && op != 4u && !arithmetic) {
            cpu_halt(cpu, "modified-immediate op 0x%x not implemented at 0x%08x", op, pc);
            return;
        }
        if (setflags && !arithmetic) {
            cpu_halt(cpu, "logical modified-immediate S=1 not implemented at 0x%08x", pc);
            return;
        }
        if (op == 3u && rn != PC) {
            cpu_halt(cpu, "ORN immediate not implemented at 0x%08x", pc);
            return;
        }
        /* CMP/CMN aliases must be recognized before rejecting Rd=PC.
         * ADD/SUB SP aliases are supported; RSB and logical SP uses are not.
         */
        if ((rd == PC && !compare) || (rd == SP && !sp_alias) ||
            (rn == SP && !sp_alias) || (rn == PC && op != 2u && op != 3u)) {
            cpu_halt(cpu, "UNPREDICTABLE modified-immediate register encoding at 0x%08x", pc);
            return;
        }
        uint16_t imm12 = (uint16_t)((((hw1 >> 10) & 1u) << 11)
                                 | (((hw2 >> 12) & 7u) << 8) | (hw2 & 0xFFu));
        thumb_imm_t immediate;
        if (!thumb_expand_imm(imm12, cpu->c, &immediate)) {
            cpu_halt(cpu, "UNPREDICTABLE modified immediate 0x%03x at 0x%08x", (unsigned)imm12, pc);
            return;
        }
        uint32_t result;
        bool carry = cpu->c, overflow = cpu->v;
        switch (op) {
        case 0u: result = cpu->r[rn] & immediate.value; break;
        case 1u: result = cpu->r[rn] & ~immediate.value; break;
        case 2u: result = rn == PC ? immediate.value : cpu->r[rn] | immediate.value; break;
        case 3u: result = ~immediate.value; break; /* MVN alias only. */
        case 4u: result = cpu->r[rn] ^ immediate.value; break;
        case 8u: result = add_with_carry(cpu->r[rn], immediate.value, false, &carry, &overflow); break;
        case 13u: result = add_with_carry(cpu->r[rn], ~immediate.value, true, &carry, &overflow); break;
        case 14u: result = add_with_carry(~cpu->r[rn], immediate.value, true, &carry, &overflow); break;
        default: return; /* All allocated cases were checked above. */
        }
        if (!compare && rd == SP && (result & 3u)) {
            cpu_halt(cpu, "UNPREDICTABLE immediate arithmetic to unaligned SP at 0x%08x", pc);
            return;
        }
        if (!compare) cpu->r[rd] = result;
        if (setflags) {
            set_nz(cpu, result);
            cpu->c = carry; cpu->v = overflow; /* NEVER immediate.carry for arithmetic. */
        }
        return;
    }

    /* TODO(Jeff): remaining Thumb-2 instructions. */
    cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
}

int cpu_step(cpu_t *cpu, bus_t *bus)
{
    if (cpu->halted) {
        return -1;
    }

    /* Deliberate first milestone: no nesting, priority registers stored only.
     * Lowest exception number wins; PRIMASK masks entry, not pending sources.
     * A dispatched handler executes its first instruction in this same step.
     */
    uint32_t any_pending = 0;
    for (unsigned i = 0; i < 8; ++i) any_pending |= cpu->pending_exceptions[i];
    if (any_pending && cpu->ipsr == 0 && !cpu->primask) {
        for (unsigned number = CPU_EXCEPTION_SYSTICK; number < CPU_EXCEPTION_COUNT; ++number) {
            if (!cpu_exception_pending(cpu, number)) continue;
            if (number >= 16u && (!cpu->irq_enabled ||
                !cpu->irq_enabled(cpu->irq_enabled_ctx, number - 16u))) continue;
            if (!exception_enter(cpu, bus, number, cpu->r[PC])) {
                if (bus_fault(bus)) cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
                return -1;
            }
            cpu_clear_pending_exception(cpu, number);
            break;
        }
    }

    uint32_t pc = cpu->r[PC];
    uint16_t hw1 = bus_read16(bus, pc);
    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
        return -1;
    }

    /* ARM ARM A5.1: prefixes 11101, 11110 and 11111 start 32-bit instructions. */
    unsigned prefix = hw1 >> 11;
    bool wide = prefix == 0x1Du || prefix == 0x1Eu || prefix == 0x1Fu;
    uint16_t hw2 = 0;
    if (wide) {
        hw2 = bus_read16(bus, pc + 2u);
        if (bus_fault(bus)) {
            cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
            return -1;
        }
    }
    if (cpu->trace_hook) {
        cpu->trace_hook(cpu->trace_hook_ctx, pc, hw1);
    }
    if (wide) {
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
    /* Keep register dumps at the instruction that stopped execution. */
    if (cpu->halted) {
        cpu->r[PC] = pc;
    }
    cpu->steps++;
    if (cpu->step_hook) {
        cpu->step_hook(cpu->step_hook_ctx);
    }
    return cpu->halted ? -1 : 0;
}
