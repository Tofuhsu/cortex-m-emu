#include "cpu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Section references are to the ARMv7-M Architecture Reference Manual,
 * Arm DDI 0403E.e: A5 (Thumb instruction encodings), A7 (instruction
 * descriptions), B1.4 (registers), B1.5 (exception model).
 */

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
    cpu->r[SP] = bus_read32(bus, 0x00000000u) & ~3u;
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

void cpu_set_irq_priority(cpu_t *cpu, cpu_irq_priority_fn query, void *ctx)
{
    cpu->irq_priority = query;
    cpu->irq_priority_ctx = ctx;
}

/* Exceptions that can be made pending by hardware or software in this model. */
static bool valid_pending(unsigned number)
{
    return number == CPU_EXCEPTION_NMI || number == CPU_EXCEPTION_PENDSV ||
           number == CPU_EXCEPTION_SYSTICK ||
           (number >= 16u && number < CPU_EXCEPTION_COUNT);
}

static bool bit_get(const uint32_t *bits, unsigned number)
{
    return (bits[number / 32u] & (UINT32_C(1) << (number % 32u))) != 0;
}

static void bit_set(uint32_t *bits, unsigned number, bool value)
{
    uint32_t mask = UINT32_C(1) << (number % 32u);
    if (value) bits[number / 32u] |= mask;
    else bits[number / 32u] &= ~mask;
}

bool cpu_pend_exception(cpu_t *cpu, unsigned number)
{
    if (!valid_pending(number)) return false;
    bit_set(cpu->pending_exceptions, number, true);
    return true;
}

bool cpu_clear_pending_exception(cpu_t *cpu, unsigned number)
{
    if (!valid_pending(number)) return false;
    bit_set(cpu->pending_exceptions, number, false);
    return true;
}

bool cpu_exception_pending(const cpu_t *cpu, unsigned number)
{
    return valid_pending(number) && bit_get(cpu->pending_exceptions, number);
}

bool cpu_exception_active(const cpu_t *cpu, unsigned number)
{
    return number < CPU_EXCEPTION_COUNT && bit_get(cpu->active_exceptions, number);
}

/* ---------------------------------------------------------------------------
 * Stack pointer banking (B1.4.1). r[SP] always holds the stack selected by
 * the current mode: SP_process in Thread mode with CONTROL.SPSEL=1, else
 * SP_main. The other one lives in sp_main or sp_process.
 */
static bool using_psp(const cpu_t *cpu)
{
    return cpu->ipsr == 0 && cpu->spsel;
}

uint32_t cpu_get_msp(const cpu_t *cpu) { return using_psp(cpu) ? cpu->sp_main : cpu->r[SP]; }
uint32_t cpu_get_psp(const cpu_t *cpu) { return using_psp(cpu) ? cpu->r[SP] : cpu->sp_process; }

static void set_msp(cpu_t *cpu, uint32_t value)
{
    if (using_psp(cpu)) cpu->sp_main = value & ~3u;
    else cpu->r[SP] = value & ~3u;
}

static void set_psp(cpu_t *cpu, uint32_t value)
{
    if (using_psp(cpu)) cpu->r[SP] = value & ~3u;
    else cpu->sp_process = value & ~3u;
}

/* Change mode/SPSEL and swap the banked stack pointer if the selection changes. */
static void set_stack_selection(cpu_t *cpu, uint16_t ipsr, bool spsel)
{
    uint32_t msp = cpu_get_msp(cpu), psp = cpu_get_psp(cpu);
    cpu->ipsr = ipsr;
    cpu->spsel = spsel;
    if (using_psp(cpu)) { cpu->r[SP] = psp; cpu->sp_main = msp; }
    else { cpu->r[SP] = msp; cpu->sp_process = psp; }
}

/* ---------------------------------------------------------------------------
 * Priorities (B1.5.4). Reset -3, NMI -2, HardFault -1; others use the stored
 * byte. Group priority drops the AIRCR.PRIGROUP subpriority bits.
 */
int cpu_exception_priority(const cpu_t *cpu, unsigned number)
{
    if (number == 1u) return -3;
    if (number == CPU_EXCEPTION_NMI) return -2;
    if (number == CPU_EXCEPTION_HARDFAULT) return -1;
    if (number < 16u) return cpu->shpr[number];
    if (number >= CPU_EXCEPTION_COUNT) return 256;
    return cpu->irq_priority ? cpu->irq_priority(cpu->irq_priority_ctx, number - 16u) : 0;
}

static int group_priority(const cpu_t *cpu, int priority)
{
    if (priority < 0) return priority;
    unsigned subgroup_bits = (unsigned)cpu->prigroup + 1u; /* PRIGROUP n: bits [n:0] */
    return (int)((unsigned)priority & (0xFFu << subgroup_bits) & 0xFFu);
}

static int execution_priority(const cpu_t *cpu, bool include_primask)
{
    int highest = 256;
    for (unsigned number = 2; number < CPU_EXCEPTION_COUNT; ++number) {
        if (!bit_get(cpu->active_exceptions, number)) continue;
        int group = group_priority(cpu, cpu_exception_priority(cpu, number));
        if (group < highest) highest = group;
    }
    int boosted = 256;
    if (cpu->basepri != 0) boosted = group_priority(cpu, cpu->basepri);
    if (include_primask && cpu->primask) boosted = 0;
    if (cpu->faultmask) boosted = -1;
    return boosted < highest ? boosted : highest;
}

int cpu_execution_priority(const cpu_t *cpu) { return execution_priority(cpu, true); }

unsigned cpu_pending_winner(const cpu_t *cpu)
{
    unsigned winner = 0;
    int best = 0;
    for (unsigned word = 0; word < 8; ++word) {
        uint32_t bits = cpu->pending_exceptions[word];
        while (bits) {
            unsigned bit = 0;
            while (!(bits & (UINT32_C(1) << bit))) ++bit;
            bits &= ~(UINT32_C(1) << bit);
            unsigned number = word * 32u + bit;
            if (number >= 16u && (!cpu->irq_enabled ||
                !cpu->irq_enabled(cpu->irq_enabled_ctx, number - 16u))) continue;
            int priority = cpu_exception_priority(cpu, number);
            if (!winner || priority < best) { winner = number; best = priority; }
        }
    }
    return winner;
}

/* A pending exception preempts when its GROUP priority is higher (lower
 * value) than the execution priority. Equal priority never preempts.
 */
static bool can_preempt(const cpu_t *cpu, unsigned number, bool include_primask)
{
    return group_priority(cpu, cpu_exception_priority(cpu, number)) <
           execution_priority(cpu, include_primask);
}

/* Interpret a 32-bit two's-complement bit pattern without an out-of-range signed cast. */
static int64_t signed32(uint32_t value)
{
    if ((value & 0x80000000u) != 0) {
        return (int64_t)value - INT64_C(4294967296);
    }
    return (int64_t)value;
}

/* AddWithCarry() (A2.2.1). SUB is add_with_carry(a, ~b, true). */
static uint32_t add_with_carry(uint32_t x, uint32_t y, bool carry_in, bool *carry_out, bool *overflow)
{
    uint64_t unsigned_sum = (uint64_t)x + (uint64_t)y + (uint64_t)carry_in;
    int64_t signed_sum = signed32(x) + signed32(y) + (int64_t)carry_in;
    uint32_t result = (uint32_t)unsigned_sum;

    *carry_out = (uint64_t)result != unsigned_sum;
    *overflow = signed32(result) != signed_sum;
    return result;
}

/* Shift_C (A7.4.2). Never evaluates a C shift by 32 or more. */
typedef struct { uint32_t value; bool carry; } shift_result_t;
enum { SRTYPE_LSL, SRTYPE_LSR, SRTYPE_ASR, SRTYPE_ROR, SRTYPE_RRX };

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

static shift_result_t asr_c(uint32_t value, unsigned amount, bool carry_in)
{
    if (amount == 0) return (shift_result_t){value, carry_in};
    bool negative = (value >> 31) != 0;
    if (amount >= 32) return (shift_result_t){negative ? UINT32_MAX : 0u, negative};
    uint32_t result = value >> amount;
    if (negative) result |= ~(UINT32_MAX >> amount);
    return (shift_result_t){result, ((value >> (amount - 1u)) & 1u) != 0};
}

static shift_result_t ror_c(uint32_t value, unsigned amount, bool carry_in)
{
    if (amount == 0) return (shift_result_t){value, carry_in};
    unsigned m = amount % 32u;
    uint32_t result = m == 0 ? value : (value >> m) | (value << (32u - m));
    return (shift_result_t){result, (result >> 31) != 0};
}

static shift_result_t shift_c(uint32_t value, unsigned type, unsigned amount, bool carry_in)
{
    switch (type) {
    case SRTYPE_LSL: return lsl_c(value, amount, carry_in);
    case SRTYPE_LSR: return lsr_c(value, amount, carry_in);
    case SRTYPE_ASR: return asr_c(value, amount, carry_in);
    case SRTYPE_ROR: return ror_c(value, amount, carry_in);
    default: /* RRX */
        return (shift_result_t){(value >> 1) | ((uint32_t)carry_in << 31), (value & 1u) != 0};
    }
}

/* DecodeImmShift (A7.4.2). */
static void decode_imm_shift(unsigned type, unsigned imm5, unsigned *srtype, unsigned *amount)
{
    *srtype = type;
    *amount = imm5;
    if (type == SRTYPE_LSR || type == SRTYPE_ASR) {
        if (imm5 == 0) *amount = 32;
    } else if (type == SRTYPE_ROR && imm5 == 0) {
        *srtype = SRTYPE_RRX;
        *amount = 1;
    }
}

/* ConditionPassed (A7.3). 0xE is AL; 0xF never reaches this helper. */
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
    case 0xE: return true;                           /* AL (IT blocks only) */
    default: return false;
    }
}

/* ITAdvance (A7.3.3): shift the mask; the block ends when IT[2:0] is zero. */
static uint8_t it_advance(uint8_t it)
{
    if ((it & 0x07u) == 0) return 0;
    return (uint8_t)((it & 0xE0u) | ((it << 1) & 0x1Fu));
}

static bool in_it_block(const cpu_t *cpu) { return (cpu->it_exec & 0x0Fu) != 0; }
static bool last_in_it_block(const cpu_t *cpu) { return (cpu->it_exec & 0x0Fu) == 0x08u; }

#define XPSR_T         (UINT32_C(1) << 24)
#define XPSR_Q         (UINT32_C(1) << 27)
#define XPSR_STACK_PAD (UINT32_C(1) << 9)
#define XPSR_IPSR_MASK UINT32_C(0x1FF)
#define EXC_RETURN_HANDLER    UINT32_C(0xFFFFFFF1)
#define EXC_RETURN_THREAD_MSP UINT32_C(0xFFFFFFF9)
#define EXC_RETURN_THREAD_PSP UINT32_C(0xFFFFFFFD)

static uint32_t it_bits(uint8_t it)
{
    return ((uint32_t)(it & 3u) << 25) | ((uint32_t)(it >> 2) << 10);
}

/* NZCVQ, the IT bits of EPSR, T and IPSR. GE and FP state are absent. */
uint32_t cpu_get_xpsr(const cpu_t *cpu)
{
    return ((uint32_t)cpu->n << 31) | ((uint32_t)cpu->z << 30)
         | ((uint32_t)cpu->c << 29) | ((uint32_t)cpu->v << 28)
         | ((uint32_t)cpu->q << 27) | it_bits(cpu->itstate)
         | XPSR_T | (cpu->ipsr & XPSR_IPSR_MASK);
}

/* ---------------------------------------------------------------------------
 * Exception entry (B1.5.6 PushStack/ExceptionTaken). The basic 8-word frame is
 * pushed on the current stack with STKALIGN=1 (fixed); the handler runs on
 * SP_main. No FP context frame: this core has no FPU.
 */
static bool exception_enter(cpu_t *cpu, bus_t *bus, unsigned number,
                             uint32_t return_address)
{
    uint32_t old_sp = cpu->r[SP];
    if ((old_sp & 3u) != 0) {
        cpu_halt(cpu, "exception entry with unaligned SP 0x%08x", old_sp);
        return false;
    }
    uint32_t handler = bus_read32(bus, (cpu->vtor & ~0x7Fu) + number * 4u);
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
    uint32_t exc_return = cpu->ipsr != 0 ? EXC_RETURN_HANDLER
                        : cpu->spsel ? EXC_RETURN_THREAD_PSP : EXC_RETURN_THREAD_MSP;
    cpu->r[SP] = frame_sp;
    set_stack_selection(cpu, (uint16_t)number, false);
    cpu->r[LR] = exc_return;
    cpu->itstate = 0;
    bit_set(cpu->active_exceptions, number, true);
    cpu->r[PC] = handler & ~1u;
    cpu->event_register = true;
    cpu->exclusive = false;
    cpu->exceptions_taken++;
    return true;
}

/* Exception return (B1.5.8). Valid EXC_RETURN values: 0xFFFFFFF1 (Handler,
 * MSP), 0xFFFFFFF9 (Thread, MSP), 0xFFFFFFFD (Thread, PSP). FP-frame values
 * (bit 4 clear) and invalid returns halt instead of raising UsageFault.
 */
static void exception_return(cpu_t *cpu, bus_t *bus, uint32_t exc_return)
{
    unsigned kind = exc_return & 0xFu;
    if ((exc_return & 0x0FFFFFF0u) != 0x0FFFFFF0u ||
        (kind != 0x1u && kind != 0x9u && kind != 0xDu)) {
        if ((exc_return & 0x0FFFFFE0u) == 0x0FFFFFE0u && (exc_return & 0x10u) == 0)
            cpu_halt(cpu, "EXC_RETURN 0x%08x selects an FP frame (no FPU modeled)", exc_return);
        else
            cpu_halt(cpu, "invalid EXC_RETURN 0x%08x", exc_return);
        return;
    }
    unsigned returning = cpu->ipsr;
    if (!bit_get(cpu->active_exceptions, returning)) {
        cpu_halt(cpu, "exception return from inactive exception %u", returning);
        return;
    }
    unsigned still_active = 0;
    for (unsigned number = 1; number < CPU_EXCEPTION_COUNT; ++number)
        if (number != returning && bit_get(cpu->active_exceptions, number)) ++still_active;
    bool to_thread = kind != 0x1u;
    if (to_thread && still_active) {
        cpu_halt(cpu, "return to Thread mode with %u exception(s) still active "
                 "(INVPC UsageFault, not modeled)", still_active);
        return;
    }
    if (!to_thread && !still_active) {
        cpu_halt(cpu, "return to Handler mode with no other active exception "
                 "(INVPC UsageFault, not modeled)");
        return;
    }
    uint32_t frame_sp = kind == 0xDu ? cpu_get_psp(cpu) : cpu_get_msp(cpu);
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
    unsigned stacked_ipsr = psr & XPSR_IPSR_MASK;
    /* The stacked PC is even: its Thumb state is stored separately in xPSR.T. */
    if ((psr & XPSR_T) == 0 || (frame[6] & 1u) != 0 ||
        (to_thread && stacked_ipsr != 0) || (!to_thread && stacked_ipsr == 0)) {
        cpu_halt(cpu, "invalid %s exception frame (PC=0x%08x xPSR=0x%08x)",
                 to_thread ? "Thread" : "Handler", frame[6], psr);
        return;
    }
    /* Commit. */
    bit_set(cpu->active_exceptions, returning, false);
    if (returning != CPU_EXCEPTION_NMI) cpu->faultmask = false;
    uint32_t new_sp = (frame_sp + 32u) | ((psr & XPSR_STACK_PAD) != 0 ? 4u : 0u);
    if (kind == 0xDu) set_psp(cpu, new_sp);
    else set_msp(cpu, new_sp);
    set_stack_selection(cpu, (uint16_t)stacked_ipsr, kind == 0xDu);
    cpu->r[0] = frame[0];
    cpu->r[1] = frame[1];
    cpu->r[2] = frame[2];
    cpu->r[3] = frame[3];
    cpu->r[12] = frame[4];
    cpu->r[LR] = frame[5];
    cpu->r[PC] = frame[6];
    cpu->n = (psr >> 31) != 0;
    cpu->z = ((psr >> 30) & 1u) != 0;
    cpu->c = ((psr >> 29) & 1u) != 0;
    cpu->v = ((psr >> 28) & 1u) != 0;
    cpu->q = ((psr >> 27) & 1u) != 0;
    cpu->itstate = (uint8_t)(((psr >> 25) & 3u) | (((psr >> 10) & 0x3Fu) << 2));
    cpu->event_register = true;
    cpu->exclusive = false;
}

/* BXWritePC/LoadWritePC: may perform an exception return in Handler mode. */
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

/* Synchronous SVCall, or HardFault escalation when SVC cannot preempt (B1.5.4). */
static void take_svc(cpu_t *cpu, bus_t *bus, uint32_t return_address)
{
    unsigned number = CPU_EXCEPTION_SVCALL;
    if (!can_preempt(cpu, number, true)) {
        if (execution_priority(cpu, true) <= -1) {
            cpu_halt(cpu, "SVC escalation in a priority -1 context: lockup (not modeled)");
            return;
        }
        cpu->hfsr |= UINT32_C(1) << 30; /* FORCED */
        number = CPU_EXCEPTION_HARDFAULT;
    }
    (void)exception_enter(cpu, bus, number, return_address);
}

/* Shared scalar transfers. Address calculation belongs to each instruction decoder. */
static bool load_value(cpu_t *cpu, bus_t *bus, uint32_t address, unsigned size,
                       bool sign, uint32_t *value)
{
    switch (size) {
    case 1: *value = bus_read8(bus, address);
        if (sign) *value = (uint32_t)(int32_t)(int8_t)(uint8_t)*value; break;
    case 2: *value = bus_read16(bus, address);
        if (sign) *value = (uint32_t)(int32_t)(int16_t)(uint16_t)*value; break;
    case 4: *value = bus_read32(bus, address); break;
    default:
        cpu_halt(cpu, "unsupported memory transfer width %u", size);
        return false;
    }
    return !bus_fault(bus);
}

static void store_value(bus_t *bus, uint32_t address, unsigned size, uint32_t value)
{
    if (size == 1) bus_write8(bus, address, (uint8_t)value);
    else if (size == 2) bus_write16(bus, address, (uint16_t)value);
    else bus_write32(bus, address, value);
}

/* Register write from a load: PC uses LoadWritePC, SP must stay aligned. */
static void write_loaded(cpu_t *cpu, bus_t *bus, unsigned rt, uint32_t value, uint32_t pc)
{
    if (rt == PC) {
        write_pc_thumb(cpu, bus, value);
    } else if (rt == SP && (value & 3u) != 0) {
        cpu_halt(cpu, "UNPREDICTABLE load of unaligned SP 0x%08x at 0x%08x", value, pc);
    } else {
        cpu->r[rt] = value;
    }
}

static void transfer_memory(cpu_t *cpu, bus_t *bus, unsigned rt, uint32_t address,
                            unsigned size, bool load, bool sign, uint32_t pc)
{
    if (load) {
        uint32_t value;
        if (load_value(cpu, bus, address, size, sign, &value)) write_loaded(cpu, bus, rt, value, pc);
    } else {
        store_value(bus, address, size, cpu->r[rt]);
    }
}

static unsigned bit_count(uint32_t value)
{
    unsigned count = 0;
    for (; value; value &= value - 1u) ++count;
    return count;
}

/* Load/store multiple, increment-after or decrement-before. */
static void load_store_multiple(cpu_t *cpu, bus_t *bus, uint32_t pc, unsigned rn,
                                uint32_t registers, bool load, bool decrement, bool wback)
{
    unsigned count = bit_count(registers);
    if (count == 0) {
        cpu_halt(cpu, "UNPREDICTABLE empty register list at 0x%08x", pc);
        return;
    }
    uint32_t base = cpu->r[rn];
    uint32_t start = decrement ? base - 4u * count : base;
    uint32_t final = decrement ? base - 4u * count : base + 4u * count;
    uint32_t address = start;
    uint32_t loaded[16];
    for (unsigned r = 0; r < 16; r++) {
        if (!(registers & (1u << r))) continue;
        if (load) {
            loaded[r] = bus_read32(bus, address);
        } else {
            bus_write32(bus, address, cpu->r[r]);
        }
        if (bus_fault(bus)) return;
        address += 4u;
    }
    /* Commit after every transfer succeeded. */
    if (wback) cpu->r[rn] = final;
    if (load) {
        for (unsigned r = 0; r < 15; r++)
            if (registers & (1u << r)) cpu->r[r] = loaded[r];
        if (registers & (1u << PC)) write_pc_thumb(cpu, bus, loaded[PC]);
    }
}

static uint32_t reg_or_pc(const cpu_t *cpu, unsigned n, uint32_t pc)
{
    return n == PC ? pc + 4u : cpu->r[n];
}

/* Saturation helpers (A2.2.1 SignedSatQ/UnsignedSatQ). */
static uint32_t signed_sat(int64_t value, unsigned bits, bool *saturated)
{
    int64_t max = (INT64_C(1) << (bits - 1u)) - 1, min = -(INT64_C(1) << (bits - 1u));
    *saturated = value > max || value < min;
    if (value > max) value = max;
    if (value < min) value = min;
    return (uint32_t)(value & 0xFFFFFFFF);
}

static uint32_t unsigned_sat(int64_t value, unsigned bits, bool *saturated)
{
    int64_t max = (INT64_C(1) << bits) - 1;
    *saturated = value > max || value < 0;
    if (value > max) value = max;
    if (value < 0) value = 0;
    return (uint32_t)value;
}

/* WFI/WFE wake-up test (B1.5.19): an exception that would preempt if
 * PRIMASK were clear. BASEPRI and active priorities still apply.
 */
static bool wake_pending(const cpu_t *cpu)
{
    unsigned winner = cpu_pending_winner(cpu);
    return winner && can_preempt(cpu, winner, false);
}

/* ===========================================================================
 * 16-bit Thumb instructions (A5.2).
 */
static void exec16(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t insn)
{
    bool setflags = !in_it_block(cpu);

    /* Shift (immediate), add, subtract, move and compare: 00xxxx. */
    if ((insn & 0xC000u) == 0x0000u) {
        unsigned rd = insn & 7u, rn = (insn >> 3) & 7u;
        if ((insn & 0x1800u) != 0x1800u && (insn & 0xE000u) == 0x0000u) {
            /* LSL/LSR/ASR (immediate) T1. LSL #0 is MOV (register) T2. */
            unsigned type = (insn >> 11) & 3u, srtype, amount;
            decode_imm_shift(type, (insn >> 6) & 31u, &srtype, &amount);
            shift_result_t result = shift_c(cpu->r[rn], srtype, amount, cpu->c);
            cpu->r[rd] = result.value;
            if (setflags) { set_nz(cpu, result.value); cpu->c = result.carry; }
            return;
        }
        if ((insn & 0xF800u) == 0x1800u) {
            /* ADD/SUB (register) T1, ADD/SUB (immediate) T1. */
            bool sub = (insn & 0x0200u) != 0, immediate = (insn & 0x0400u) != 0;
            unsigned m = (insn >> 6) & 7u;
            uint32_t operand = immediate ? m : cpu->r[m];
            bool c, v;
            uint32_t result = sub ? add_with_carry(cpu->r[rn], ~operand, true, &c, &v)
                                  : add_with_carry(cpu->r[rn], operand, false, &c, &v);
            cpu->r[rd] = result;
            if (setflags) { set_nz(cpu, result); cpu->c = c; cpu->v = v; }
            return;
        }
        /* MOV/CMP/ADD/SUB (immediate) imm8: 001 op Rdn imm8. */
        unsigned op = (insn >> 11) & 3u, rdn = (insn >> 8) & 7u;
        uint32_t imm = insn & 0xFFu, result;
        bool c, v;
        switch (op) {
        case 0: /* MOVS T1 preserves C and V. */
            cpu->r[rdn] = imm;
            if (setflags) set_nz(cpu, imm);
            return;
        case 1: /* CMP T1 always sets flags. */
            result = add_with_carry(cpu->r[rdn], ~imm, true, &c, &v);
            set_nz(cpu, result); cpu->c = c; cpu->v = v;
            return;
        case 2:
            result = add_with_carry(cpu->r[rdn], imm, false, &c, &v);
            break;
        default:
            result = add_with_carry(cpu->r[rdn], ~imm, true, &c, &v);
            break;
        }
        cpu->r[rdn] = result;
        if (setflags) { set_nz(cpu, result); cpu->c = c; cpu->v = v; }
        return;
    }

    /* Data processing (A5.2.2): 010000 opcode Rm Rdn. */
    if ((insn & 0xFC00u) == 0x4000u) {
        unsigned op = (insn >> 6) & 0x0Fu, rm = (insn >> 3) & 7u, rdn = insn & 7u;
        uint32_t a = cpu->r[rdn], b = cpu->r[rm], result = 0;
        bool c = cpu->c, v = cpu->v, write = true, flags = setflags;
        shift_result_t sr;
        switch (op) {
        case 0x0: result = a & b; break;                                   /* AND */
        case 0x1: result = a ^ b; break;                                   /* EOR */
        case 0x2: sr = lsl_c(a, b & 0xFFu, c); result = sr.value; c = sr.carry; break;
        case 0x3: sr = lsr_c(a, b & 0xFFu, c); result = sr.value; c = sr.carry; break;
        case 0x4: sr = asr_c(a, b & 0xFFu, c); result = sr.value; c = sr.carry; break;
        case 0x5: result = add_with_carry(a, b, cpu->c, &c, &v); break;    /* ADC */
        case 0x6: result = add_with_carry(a, ~b, cpu->c, &c, &v); break;   /* SBC */
        case 0x7: sr = ror_c(a, b & 0xFFu, c); result = sr.value; c = sr.carry; break;
        case 0x8: result = a & b; write = false; flags = true; break;      /* TST */
        case 0x9: result = add_with_carry(~b, 0, true, &c, &v); break;     /* RSB #0 */
        case 0xA: result = add_with_carry(a, ~b, true, &c, &v); write = false; flags = true; break;
        case 0xB: result = add_with_carry(a, b, false, &c, &v); write = false; flags = true; break;
        case 0xC: result = a | b; break;                                   /* ORR */
        case 0xD: result = a * b; break;                                   /* MUL: C,V unchanged */
        case 0xE: result = a & ~b; break;                                  /* BIC */
        default:  result = ~b; break;                                      /* MVN */
        }
        if (write) cpu->r[rdn] = result;
        if (flags) { set_nz(cpu, result); cpu->c = c; cpu->v = v; }
        return;
    }

    /* Special data processing and branch and exchange (A5.2.3): 010001. */
    if ((insn & 0xFC00u) == 0x4400u) {
        unsigned op = (insn >> 8) & 3u;
        unsigned rd = (insn & 0x07u) | ((insn >> 4) & 0x08u);
        unsigned rm = (insn >> 3) & 0x0Fu;
        if (op == 0) {
            /* ADD (register) T2 (A7.7.4), including SP aliases. A5.1.3:
             * writing nonzero SP[1:0] is UNPREDICTABLE.
             */
            if (rd == PC && rm == PC) {
                cpu_halt(cpu, "UNPREDICTABLE ADD with both operands PC at 0x%08x", pc);
                return;
            }
            uint32_t result = reg_or_pc(cpu, rd, pc) + reg_or_pc(cpu, rm, pc);
            if (rd == SP && (result & 3u) != 0) {
                cpu_halt(cpu, "UNPREDICTABLE ADD to unaligned SP at 0x%08x", pc);
                return;
            }
            /* ALUWritePC/BranchWritePC, like MOV PC, not BX interworking. */
            cpu->r[rd] = rd == PC ? result & ~1u : result;
            return;
        }
        if (op == 1) {
            /* CMP (register) T2: high registers. */
            if (rd == PC || rm == PC || (rd < 8 && rm < 8)) {
                cpu_halt(cpu, "UNPREDICTABLE CMP (register) T2 at 0x%08x", pc);
                return;
            }
            bool c, v;
            uint32_t result = add_with_carry(cpu->r[rd], ~cpu->r[rm], true, &c, &v);
            set_nz(cpu, result); cpu->c = c; cpu->v = v;
            return;
        }
        if (op == 2) {
            /* MOV (register) T1. Flags unchanged. */
            uint32_t result = reg_or_pc(cpu, rm, pc);
            if (rd == SP && (result & 3u) != 0) {
                cpu_halt(cpu, "UNPREDICTABLE MOV to unaligned SP at 0x%08x", pc);
                return;
            }
            cpu->r[rd] = rd == PC ? result & ~1u : result;
            return;
        }
        /* BX / BLX (register): 0100 0111 L Rm 000. */
        if ((insn & 0x0007u) != 0 || (in_it_block(cpu) && !last_in_it_block(cpu))) {
            cpu_halt(cpu, "UNPREDICTABLE BX/BLX encoding 0x%04x at 0x%08x", insn, pc);
            return;
        }
        uint32_t target = reg_or_pc(cpu, rm, pc);
        if (insn & 0x0080u) {
            if (rm == PC) { cpu_halt(cpu, "UNPREDICTABLE BLX PC at 0x%08x", pc); return; }
            if ((target & 1u) == 0) {
                cpu_halt(cpu, "BLX target 0x%08x has Thumb bit clear (INVSTATE)", target);
                return;
            }
            cpu->r[LR] = (pc + 2u) | 1u;
        }
        write_pc_thumb(cpu, bus, target);
        return;
    }

    /* LDR (literal) T1: 01001 Rt imm8. PC as an operand is Align(pc + 4, 4). */
    if ((insn & 0xF800u) == 0x4800u) {
        unsigned rt = (insn >> 8) & 0x07u;
        uint32_t base = (pc + 4u) & ~3u;
        transfer_memory(cpu, bus, rt, base + ((insn & 0xFFu) << 2), 4, true, false, pc);
        return;
    }

    /* Load/store (register) T1: 0101 opB Rm Rn Rt. opB: STR, STRH, STRB,
     * LDRSB, LDR, LDRH, LDRB, LDRSH. */
    if ((insn & 0xF000u) == 0x5000u) {
        static const unsigned sizes[8] = {4, 2, 1, 1, 4, 2, 1, 2};
        unsigned op = (insn >> 9) & 0x07u;
        unsigned rm = (insn >> 6) & 0x07u, rn = (insn >> 3) & 0x07u, rt = insn & 0x07u;
        transfer_memory(cpu, bus, rt, cpu->r[rn] + cpu->r[rm], sizes[op], op >= 3u,
                        op == 3u || op == 7u, pc);
        return;
    }

    /* STR/LDR (word, imm5*4), STRB/LDRB (imm5), STRH/LDRH (imm5*2). */
    if ((insn & 0xE000u) == 0x6000u || (insn & 0xF000u) == 0x8000u) {
        unsigned rn = (insn >> 3) & 0x07u, rt = insn & 0x07u, imm5 = (insn >> 6) & 0x1Fu;
        bool load = (insn & 0x0800u) != 0;
        unsigned size = (insn & 0xF000u) == 0x8000u ? 2u : (insn & 0x1000u) ? 1u : 4u;
        transfer_memory(cpu, bus, rt, cpu->r[rn] + imm5 * size, size, load, false, pc);
        return;
    }

    /* STR/LDR (immediate) T2, SP-relative: 1001 L Rt imm8. */
    if ((insn & 0xF000u) == 0x9000u) {
        unsigned rt = (insn >> 8) & 0x07u;
        uint32_t address = cpu->r[SP] + ((insn & 0xFFu) << 2);
        transfer_memory(cpu, bus, rt, address, 4, (insn & 0x0800u) != 0, false, pc);
        return;
    }

    /* ADR T1 (10100) and ADD (SP plus immediate) T1 (10101). Flags unchanged. */
    if ((insn & 0xF000u) == 0xA000u) {
        unsigned rd = (insn >> 8) & 0x07u;
        uint32_t base = (insn & 0x0800u) ? cpu->r[SP] : (pc + 4u) & ~3u;
        cpu->r[rd] = base + ((insn & 0xFFu) << 2);
        return;
    }

    /* Miscellaneous 16-bit instructions (A5.2.5): 1011 xxxx. */
    if ((insn & 0xF000u) == 0xB000u) {
        if ((insn & 0xFF00u) == 0xB000u) {
            /* ADD/SUB (SP plus/minus immediate) T2/T1: 1011 0000 S imm7. */
            uint32_t imm = (insn & 0x7Fu) << 2;
            cpu->r[SP] = (insn & 0x80u) ? cpu->r[SP] - imm : cpu->r[SP] + imm;
            return;
        }
        if ((insn & 0xF500u) == 0xB100u) {
            /* CBZ/CBNZ: 1011 op 0 i 1 imm5 Rn. */
            if (in_it_block(cpu)) { cpu_halt(cpu, "UNPREDICTABLE CBZ/CBNZ in IT block at 0x%08x", pc); return; }
            unsigned rn = insn & 7u;
            uint32_t imm = (((insn >> 9) & 1u) << 6) | (((insn >> 3) & 0x1Fu) << 1);
            bool nonzero = (insn & 0x0800u) != 0;
            if ((cpu->r[rn] == 0) != nonzero) cpu->r[PC] = pc + 4u + imm;
            return;
        }
        if ((insn & 0xFF00u) == 0xB200u) {
            /* SXTH, SXTB, UXTH, UXTB T1. Flags unchanged. */
            unsigned rd = insn & 7u, rm = (insn >> 3) & 7u;
            uint32_t value = cpu->r[rm];
            switch ((insn >> 6) & 3u) {
            case 0: value = (uint32_t)(int32_t)(int16_t)(uint16_t)value; break;
            case 1: value = (uint32_t)(int32_t)(int8_t)(uint8_t)value; break;
            case 2: value &= 0xFFFFu; break;
            default: value &= 0xFFu; break;
            }
            cpu->r[rd] = value;
            return;
        }
        if ((insn & 0xFE00u) == 0xB400u) {
            /* PUSH T1: STMDB SP!, the lowest register at the lowest address. */
            uint32_t registers = (insn & 0xFFu) | ((insn & 0x100u) ? 1u << LR : 0u);
            load_store_multiple(cpu, bus, pc, SP, registers, false, true, true);
            return;
        }
        if ((insn & 0xFFE0u) == 0xB660u) {
            /* CPS: 1011 0110 011 im 0 0 I F. Privileged execution is assumed. */
            if ((insn & 0x000Cu) != 0 || (insn & 3u) == 0) {
                cpu_halt(cpu, "UNPREDICTABLE CPS encoding 0x%04x at 0x%08x", insn, pc);
                return;
            }
            bool disable = (insn & 0x10u) != 0;
            if (insn & 2u) cpu->primask = disable;
            /* FAULTMASK cannot be set at execution priority -1 or above. */
            if ((insn & 1u) && (!disable || execution_priority(cpu, true) > -1))
                cpu->faultmask = disable;
            return;
        }
        if ((insn & 0xFF00u) == 0xBA00u) {
            /* REV, REV16, (op 2 undefined), REVSH. */
            unsigned rd = insn & 7u, rm = (insn >> 3) & 7u, op = (insn >> 6) & 3u;
            uint32_t x = cpu->r[rm];
            if (op == 0) cpu->r[rd] = (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
            else if (op == 1) cpu->r[rd] = ((x >> 8) & 0x00FF00FFu) | ((x << 8) & 0xFF00FF00u);
            else if (op == 3) cpu->r[rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((x & 0xFFu) << 8) | ((x >> 8) & 0xFFu));
            else cpu_halt(cpu, "undefined 16-bit instruction 0x%04x at 0x%08x", insn, pc);
            return;
        }
        if ((insn & 0xFE00u) == 0xBC00u) {
            /* POP T1: the optional PC is the last word on the stack. */
            uint32_t registers = (insn & 0xFFu) | ((insn & 0x100u) ? 1u << PC : 0u);
            if ((registers & (1u << PC)) && in_it_block(cpu) && !last_in_it_block(cpu)) {
                cpu_halt(cpu, "UNPREDICTABLE POP {pc} inside IT block at 0x%08x", pc);
                return;
            }
            load_store_multiple(cpu, bus, pc, SP, registers, true, false, true);
            return;
        }
        if ((insn & 0xFF00u) == 0xBE00u) {
            /* BKPT T1 (unconditional, also inside an IT block). */
            cpu->r[PC] = pc;
            cpu_halt(cpu, "bkpt #%u", insn & 0xFFu);
            return;
        }
        if ((insn & 0xFF00u) == 0xBF00u) {
            if ((insn & 0x000Fu) != 0) {
                /* IT: 1011 1111 firstcond mask (A7.7.37). */
                unsigned firstcond = (insn >> 4) & 0xFu, mask = insn & 0xFu;
                if (firstcond == 0xFu || in_it_block(cpu) ||
                    (firstcond == 0xEu && bit_count(mask) != 1u)) {
                    cpu_halt(cpu, "UNPREDICTABLE IT encoding 0x%04x at 0x%08x", insn, pc);
                    return;
                }
                cpu->itstate = (uint8_t)(insn & 0xFFu);
                return;
            }
            switch ((insn >> 4) & 0xFu) {
            case 0: return; /* NOP */
            case 1: return; /* YIELD: no multithreading hint consumer */
            case 2: /* WFE */
                if (cpu->event_register) cpu->event_register = false;
                else cpu->sleeping = true;
                return;
            case 3: /* WFI */
                cpu->sleeping = true;
                return;
            case 4: cpu->event_register = true; return; /* SEV (single core) */
            default:
                return; /* Unallocated hints execute as NOP (A5.2.5). */
            }
        }
        cpu_halt(cpu, "undefined 16-bit instruction 0x%04x at 0x%08x", insn, pc);
        return;
    }

    /* STM (STMIA) T1 / LDM (LDMIA) T1: 1100 L Rn reglist. */
    if ((insn & 0xF000u) == 0xC000u) {
        unsigned rn = (insn >> 8) & 7u;
        uint32_t registers = insn & 0xFFu;
        bool load = (insn & 0x0800u) != 0;
        bool wback = !load || !(registers & (1u << rn));
        load_store_multiple(cpu, bus, pc, rn, registers, load, false, wback);
        return;
    }

    /* SVC T1: 1101 1111 imm8. Stack the NEXT instruction, not pc+4. */
    if ((insn & 0xFF00u) == 0xDF00u) {
        take_svc(cpu, bus, pc + 2u);
        return;
    }

    /* B<cond> T1: 1101 cond imm8. 0xDE is UDF. Not allowed inside IT. */
    unsigned cond = (insn >> 8) & 0x0Fu;
    if ((insn & 0xF000u) == 0xD000u && cond < 0xEu) {
        if (in_it_block(cpu)) { cpu_halt(cpu, "UNPREDICTABLE B<cond> in IT block at 0x%08x", pc); return; }
        if (condition_passed(cpu, cond)) {
            uint32_t imm9 = (insn & 0xFFu) << 1;
            int32_t offset = (int32_t)imm9;
            if ((imm9 & 0x100u) != 0) offset -= 0x200;
            cpu->r[PC] = pc + 4u + (uint32_t)offset;
        }
        return;
    }

    /* B (unconditional) T2: 11100 imm11. */
    if ((insn & 0xF800u) == 0xE000u) {
        if (in_it_block(cpu) && !last_in_it_block(cpu)) {
            cpu_halt(cpu, "UNPREDICTABLE B inside IT block at 0x%08x", pc);
            return;
        }
        uint32_t imm12 = (insn & 0x07FFu) << 1;
        int32_t offset = (int32_t)imm12;
        if ((imm12 & 0x0800u) != 0) offset -= 0x1000;
        cpu->r[PC] = pc + 4u + (uint32_t)offset;
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

/* ThumbExpandImm_C (A5.3.2). False means an invalid/UNPREDICTABLE encoding. */
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

static bool bad_reg(unsigned r) { return r == SP || r == PC; }

/*
 * Shared data-processing semantics for the modified-immediate (A5.3.1) and
 * shifted-register (A5.3.11) encodings. `operand` and `shift_carry` come from
 * ThumbExpandImm_C or Shift_C. op is the 4-bit opcode of those tables.
 */
static void data_processing(cpu_t *cpu, uint32_t pc, unsigned op, bool setflags,
                            unsigned rn, unsigned rd, uint32_t operand, bool shift_carry,
                            bool immediate)
{
    bool compare = setflags && rd == PC && (op == 0u || op == 4u || op == 8u || op == 13u);
    bool move = (op == 2u || op == 3u) && rn == PC;          /* MOV/MVN aliases */
    bool sp_arith = rn == SP && (op == 8u || op == 13u);      /* ADD/SUB SP forms */
    switch (op) {
    case 0u: case 1u: case 2u: case 3u: case 4u: case 8u: case 10u: case 11u: case 13u: case 14u:
        break;
    default:
        cpu_halt(cpu, "%s data-processing op 0x%x not implemented at 0x%08x",
                 immediate ? "modified-immediate" : "shifted-register", op, pc);
        return;
    }
    /* Register restrictions (A7.7 per instruction): PC never as a data
     * register except the compare/move aliases; SP only in ADD/SUB SP forms
     * (and as a MOV source/destination without flags). */
    bool rd_ok = compare || rd != PC;
    if (rd == SP && !compare) {
        rd_ok = sp_arith || (move && op == 2u && !immediate && !setflags);
        if (op == 8u || op == 13u) rd_ok = sp_arith;
    }
    bool rn_ok = move || (rn != PC && (rn != SP || sp_arith));
    if (!rd_ok || !rn_ok) {
        cpu_halt(cpu, "UNPREDICTABLE %s register encoding at 0x%08x",
                 immediate ? "modified-immediate" : "shifted-register", pc);
        return;
    }
    uint32_t a = move ? 0 : cpu->r[rn], result;
    bool carry = cpu->c, overflow = cpu->v, logical = true;
    switch (op) {
    case 0u: result = a & operand; break;                              /* AND/TST */
    case 1u: result = a & ~operand; break;                             /* BIC */
    case 2u: result = move ? operand : a | operand; break;             /* ORR/MOV */
    case 3u: result = move ? ~operand : a | ~operand; break;           /* ORN/MVN */
    case 4u: result = a ^ operand; break;                              /* EOR/TEQ */
    case 8u: result = add_with_carry(a, operand, false, &carry, &overflow); logical = false; break;
    case 10u: result = add_with_carry(a, operand, cpu->c, &carry, &overflow); logical = false; break;
    case 11u: result = add_with_carry(a, ~operand, cpu->c, &carry, &overflow); logical = false; break;
    case 13u: result = add_with_carry(a, ~operand, true, &carry, &overflow); logical = false; break;
    default: result = add_with_carry(~a, operand, true, &carry, &overflow); logical = false; break;
    }
    if (logical) carry = shift_carry;
    if (!compare && rd == SP && (result & 3u)) {
        cpu_halt(cpu, "UNPREDICTABLE %s arithmetic to unaligned SP at 0x%08x",
                 immediate ? "immediate" : "register", pc);
        return;
    }
    if (!compare) cpu->r[rd] = result;
    if (setflags) {
        set_nz(cpu, result);
        cpu->c = carry; /* NEVER the immediate carry for arithmetic. */
        if (!logical) cpu->v = overflow;
    }
}

/* MSR (B5.2.3) / MRS (B5.2.2) special register access. Privileged model. */
static uint32_t read_special(cpu_t *cpu, unsigned sysm, bool *ok)
{
    *ok = true;
    if (sysm <= 7u) {
        uint32_t value = 0;
        if ((sysm & 4u) == 0) value |= cpu_get_xpsr(cpu) & 0xF8000000u; /* APSR */
        if (sysm & 1u) value |= cpu->ipsr & XPSR_IPSR_MASK;               /* IPSR */
        return value; /* EPSR reads as zero (B5.2.2) */
    }
    switch (sysm) {
    case 8u: return cpu_get_msp(cpu);
    case 9u: return cpu_get_psp(cpu);
    case 16u: return cpu->primask ? 1u : 0u;
    case 17u: case 18u: return cpu->basepri;
    case 19u: return cpu->faultmask ? 1u : 0u;
    case 20u: return (cpu->spsel ? 2u : 0u) | (cpu->npriv ? 1u : 0u);
    default: *ok = false; return 0;
    }
}

static bool write_special(cpu_t *cpu, unsigned sysm, unsigned mask, uint32_t value)
{
    if (sysm <= 7u) {
        if ((sysm & 4u) == 0 && (mask & 2u)) {
            cpu->n = (value >> 31) & 1u; cpu->z = (value >> 30) & 1u;
            cpu->c = (value >> 29) & 1u; cpu->v = (value >> 28) & 1u;
            cpu->q = (value >> 27) & 1u;
        }
        return true; /* IPSR/EPSR writes are ignored; GE (mask bit 0) not modeled */
    }
    switch (sysm) {
    case 8u: set_msp(cpu, value); return true;
    case 9u: set_psp(cpu, value); return true;
    case 16u: cpu->primask = value & 1u; return true;
    case 17u: cpu->basepri = (uint8_t)(value & 0xF0u); return true; /* 4 implemented bits */
    case 18u: {
        uint8_t v = (uint8_t)(value & 0xF0u);
        if (v != 0 && (v < cpu->basepri || cpu->basepri == 0)) cpu->basepri = v;
        return true;
    }
    case 19u:
        if (execution_priority(cpu, true) > -1 || !(value & 1u)) cpu->faultmask = value & 1u;
        return true;
    case 20u:
        cpu->npriv = value & 1u;
        if (cpu->ipsr == 0) set_stack_selection(cpu, 0, (value & 2u) != 0);
        return true;
    default:
        return false;
    }
}

/* ===========================================================================
 * 32-bit Thumb-2 instructions (A5.3).
 */
static void exec_load_store_single(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned rn = hw1 & 0x0Fu, rt = (hw2 >> 12) & 0x0Fu;
    bool load = (hw1 & 0x0010u) != 0, sign = (hw1 & 0x0100u) != 0;
    unsigned size_code = (hw1 >> 5) & 3u;
    unsigned size = size_code == 0 ? 1u : size_code == 1 ? 2u : 4u;
    if (size_code == 3u || (sign && (!load || size == 4u))) {
        cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
        return;
    }
    /* PLD/PLI: byte/halfword loads with Rt=PC are hints. */
    if (load && rt == PC && size != 4u) return;
    if (load && rt == PC && in_it_block(cpu) && !last_in_it_block(cpu)) {
        cpu_halt(cpu, "UNPREDICTABLE load to PC inside IT block at 0x%08x", pc);
        return;
    }

    if (rn == PC) {
        /* Literal: LDR/LDRB/LDRH/LDRSB/LDRSH, U = hw1 bit 7 (A7.7.44 etc.). */
        if (!load) { cpu_halt(cpu, "UNDEFINED store with Rn=PC at 0x%08x", pc); return; }
        if (size == 4u && rt == PC) {
            cpu_halt(cpu, "LDR literal T2 to PC not implemented at 0x%08x", pc);
            return;
        }
        uint32_t base = (pc + 4u) & ~3u, offset = hw2 & 0x0FFFu;
        uint32_t address = (hw1 & 0x0080u) ? base + offset : base - offset;
        if (size != 4u && rt == SP) {
            cpu_halt(cpu, "UNPREDICTABLE byte/halfword load Rt=SP at 0x%08x", pc);
            return;
        }
        if (rt == SP) {
            uint32_t value;
            if (!load_value(cpu, bus, address, 4, false, &value)) return;
            if (value & 3u) cpu_halt(cpu, "UNPREDICTABLE LDR literal to unaligned SP at 0x%08x", pc);
            else cpu->r[SP] = value;
            return;
        }
        transfer_memory(cpu, bus, rt, address, size, true, sign, pc);
        return;
    }
    if ((size != 4u && rt == SP) || (!load && rt == PC)) {
        cpu_halt(cpu, "UNPREDICTABLE load/store Rt=%u at 0x%08x", rt, pc);
        return;
    }

    if (hw1 & 0x0080u) {
        /* imm12, unsigned byte offset (T2/T3 encodings). */
        transfer_memory(cpu, bus, rt, cpu->r[rn] + (hw2 & 0x0FFFu), size, load, sign, pc);
        return;
    }
    if (hw2 & 0x0800u) {
        /* imm8 with P U W (T4/T3 encodings): negative offset, pre/post-index. */
        bool index = (hw2 & 0x0400u) != 0, add = (hw2 & 0x0200u) != 0, wback = (hw2 & 0x0100u) != 0;
        uint32_t imm8 = hw2 & 0xFFu;
        if (!index && !wback) {
            cpu_halt(cpu, "UNDEFINED load/store imm8 P=0 W=0 at 0x%08x", pc);
            return;
        }
        /* P=1 U=1 W=0 is the unprivileged LDRT/STRT form: this model runs
         * privileged, so it behaves like the plain offset form. */
        if (wback && rn == rt) {
            cpu_halt(cpu, "UNPREDICTABLE load/store writeback with Rn=Rt at 0x%08x", pc);
            return;
        }
        uint32_t offset_address = add ? cpu->r[rn] + imm8 : cpu->r[rn] - imm8;
        uint32_t address = index ? offset_address : cpu->r[rn];
        if (load) {
            uint32_t value;
            if (!load_value(cpu, bus, address, size, sign, &value)) return;
            if (wback) cpu->r[rn] = offset_address;
            write_loaded(cpu, bus, rt, value, pc);
        } else {
            store_value(bus, address, size, cpu->r[rt]);
            if (wback && !bus_fault(bus)) cpu->r[rn] = offset_address;
        }
        return;
    }
    if ((hw2 & 0x0FC0u) == 0) {
        /* Register offset: [Rn, Rm, LSL #imm2]. */
        unsigned rm = hw2 & 0x0Fu;
        if (bad_reg(rm)) { cpu_halt(cpu, "UNPREDICTABLE load/store Rm=%u at 0x%08x", rm, pc); return; }
        uint32_t address = cpu->r[rn] + (cpu->r[rm] << ((hw2 >> 4) & 3u));
        transfer_memory(cpu, bus, rt, address, size, load, sign, pc);
        return;
    }
    cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
}

static void exec_dual_exclusive_table(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned rn = hw1 & 0xFu, rt = (hw2 >> 12) & 0xFu, rt2 = (hw2 >> 8) & 0xFu;
    bool load = (hw1 & 0x0010u) != 0;
    bool p = (hw1 & 0x0100u) != 0, u = (hw1 & 0x0080u) != 0, w = (hw1 & 0x0020u) != 0;
    if (p || w) {
        /* LDRD/STRD (immediate): imm8*4, P U W. */
        uint32_t imm = (hw2 & 0xFFu) << 2;
        uint32_t base = rn == PC ? (pc + 4u) & ~3u : cpu->r[rn];
        if ((rn == PC && (!load || w)) || bad_reg(rt) || bad_reg(rt2) || (load && rt == rt2) ||
            (w && (rn == rt || rn == rt2))) {
            cpu_halt(cpu, "UNPREDICTABLE LDRD/STRD registers at 0x%08x", pc);
            return;
        }
        uint32_t offset_address = u ? base + imm : base - imm;
        uint32_t address = p ? offset_address : base;
        if (load) {
            uint32_t first = bus_read32(bus, address), second = bus_read32(bus, address + 4u);
            if (bus_fault(bus)) return;
            cpu->r[rt] = first; cpu->r[rt2] = second;
        } else {
            bus_write32(bus, address, cpu->r[rt]);
            bus_write32(bus, address + 4u, cpu->r[rt2]);
            if (bus_fault(bus)) return;
        }
        if (w) cpu->r[rn] = offset_address;
        return;
    }
    if (!u) {
        /* LDREX/STREX (word): imm8*4 offset. */
        uint32_t address = cpu->r[rn] + ((hw2 & 0xFFu) << 2);
        if (load) {
            uint32_t value = bus_read32(bus, address);
            if (bus_fault(bus)) return;
            cpu->r[rt] = value;
            cpu->exclusive = true; cpu->exclusive_address = address;
        } else {
            unsigned rd = (hw2 >> 8) & 0xFu;
            if (cpu->exclusive && cpu->exclusive_address == address) {
                bus_write32(bus, address, cpu->r[rt]);
                if (bus_fault(bus)) return;
                cpu->r[rd] = 0;
            } else {
                cpu->r[rd] = 1;
            }
            cpu->exclusive = false;
        }
        return;
    }
    unsigned op3 = (hw2 >> 4) & 0xFu;
    if (load && (op3 == 0u || op3 == 1u)) {
        /* TBB/TBH: branch forward by twice the table entry. */
        unsigned rm = hw2 & 0xFu;
        if (in_it_block(cpu) && !last_in_it_block(cpu)) {
            cpu_halt(cpu, "UNPREDICTABLE TBB/TBH inside IT block at 0x%08x", pc);
            return;
        }
        uint32_t base = reg_or_pc(cpu, rn, pc), entry;
        bool half = op3 == 1u;
        uint32_t address = base + (half ? cpu->r[rm] << 1 : cpu->r[rm]);
        if (!load_value(cpu, bus, address, half ? 2u : 1u, false, &entry)) return;
        cpu->r[PC] = pc + 4u + 2u * entry;
        return;
    }
    if (op3 == 4u || op3 == 5u) {
        /* LDREXB/H, STREXB/H. */
        unsigned size = op3 == 4u ? 1u : 2u;
        uint32_t address = cpu->r[rn];
        if (load) {
            uint32_t value;
            if (!load_value(cpu, bus, address, size, false, &value)) return;
            cpu->r[rt] = value;
            cpu->exclusive = true; cpu->exclusive_address = address;
        } else {
            unsigned rd = hw2 & 0xFu;
            if (cpu->exclusive && cpu->exclusive_address == address) {
                store_value(bus, address, size, cpu->r[rt]);
                if (bus_fault(bus)) return;
                cpu->r[rd] = 0;
            } else {
                cpu->r[rd] = 1;
            }
            cpu->exclusive = false;
        }
        return;
    }
    cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
}

static void exec_plain_immediate(cpu_t *cpu, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op = (hw1 >> 4) & 0x1Fu, rn = hw1 & 0xFu, rd = (hw2 >> 8) & 0xFu;
    uint32_t imm12 = (((uint32_t)hw1 >> 10) & 1u) << 11 | ((hw2 >> 12) & 7u) << 8 | (hw2 & 0xFFu);
    uint32_t imm16 = ((uint32_t)(hw1 & 0x0Fu) << 12) | imm12;
    unsigned imm5 = ((hw2 >> 12) & 7u) << 2 | ((hw2 >> 6) & 3u);
    unsigned width_field = hw2 & 0x1Fu;
    if (bad_reg(rd) && !((op == 0u || op == 0x0Au) && rd == SP && rn == SP)) {
        cpu_halt(cpu, "UNPREDICTABLE plain-immediate Rd=%u at 0x%08x", rd, pc);
        return;
    }
    switch (op) {
    case 0x00u: /* ADDW / ADR (add) */
    case 0x0Au: /* SUBW / ADR (subtract) */
        if (rn == PC) {
            uint32_t base = (pc + 4u) & ~3u;
            cpu->r[rd] = op == 0u ? base + imm12 : base - imm12;
        } else {
            uint32_t result = op == 0u ? cpu->r[rn] + imm12 : cpu->r[rn] - imm12;
            if (rd == SP && (result & 3u)) { cpu_halt(cpu, "UNPREDICTABLE ADDW/SUBW to unaligned SP at 0x%08x", pc); return; }
            cpu->r[rd] = result;
        }
        return;
    case 0x04u: cpu->r[rd] = imm16; return;                               /* MOVW */
    case 0x0Cu: cpu->r[rd] = (cpu->r[rd] & 0xFFFFu) | (imm16 << 16); return; /* MOVT */
    case 0x10u: case 0x12u: case 0x18u: case 0x1Au: { /* SSAT / USAT */
        if (bad_reg(rn)) break;
        unsigned type = (op & 0x02u) ? SRTYPE_ASR : SRTYPE_LSL, srtype, amount;
        if (type == SRTYPE_ASR && imm5 == 0) break; /* SSAT16/USAT16: not modeled */
        decode_imm_shift(type, imm5, &srtype, &amount);
        uint32_t operand = shift_c(cpu->r[rn], srtype, amount, cpu->c).value;
        bool saturated;
        if (op < 0x18u) cpu->r[rd] = signed_sat(signed32(operand), width_field + 1u, &saturated);
        else cpu->r[rd] = unsigned_sat(signed32(operand), width_field, &saturated);
        if (saturated) cpu->q = true;
        return;
    }
    case 0x14u: case 0x1Cu: { /* SBFX / UBFX */
        if (bad_reg(rn)) break;
        unsigned lsb = imm5, width = width_field + 1u;
        if (lsb + width > 32u) break;
        uint32_t field = cpu->r[rn] >> lsb;
        if (width < 32u) field &= (UINT32_C(1) << width) - 1u;
        if (op == 0x14u && width < 32u && (field >> (width - 1u)) & 1u) field |= ~((UINT32_C(1) << width) - 1u);
        cpu->r[rd] = field;
        return;
    }
    case 0x16u: { /* BFI / BFC (Rn = PC) */
        unsigned lsb = imm5, msb = width_field;
        if (msb < lsb || rn == SP) break;
        uint32_t mask = (msb - lsb + 1u >= 32u ? UINT32_MAX : ((UINT32_C(1) << (msb - lsb + 1u)) - 1u)) << lsb;
        uint32_t source = rn == PC ? 0u : cpu->r[rn] << lsb;
        cpu->r[rd] = (cpu->r[rd] & ~mask) | (source & mask);
        return;
    }
    default:
        break;
    }
    cpu_halt(cpu, "plain-immediate op 0x%02x not implemented or UNPREDICTABLE at 0x%08x", op, pc);
}

static void exec_data_processing_register(cpu_t *cpu, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op1 = (hw1 >> 4) & 0xFu, op2 = (hw2 >> 4) & 0xFu;
    unsigned rn = hw1 & 0xFu, rd = (hw2 >> 8) & 0xFu, rm = hw2 & 0xFu;
    if ((hw2 & 0xF000u) != 0xF000u) goto undefined;
    if ((op1 & 0x8u) == 0 && op2 == 0) {
        /* LSL/LSR/ASR/ROR (register) T2: shift by the low BYTE of Rm. */
        if (bad_reg(rn) || bad_reg(rd) || bad_reg(rm)) {
            cpu_halt(cpu, "UNPREDICTABLE register shift operand at 0x%08x", pc);
            return;
        }
        shift_result_t result = shift_c(cpu->r[rn], (op1 >> 1) & 3u, cpu->r[rm] & 255u, cpu->c);
        cpu->r[rd] = result.value;
        if (op1 & 1u) { set_nz(cpu, result.value); cpu->c = result.carry; }
        return;
    }
    if ((op1 & 0x8u) == 0 && (op2 & 0x8u)) {
        /* SXTAH/SXTH, UXTAH/UXTH, SXTAB/SXTB, UXTAB/UXTB with rotation. */
        if (bad_reg(rd) || bad_reg(rm) || rn == SP) goto unpredictable;
        uint32_t rotated = ror32(cpu->r[rm], ((hw2 >> 4) & 3u) * 8u), value;
        switch (op1) {
        case 0x0u: value = (uint32_t)(int32_t)(int16_t)(uint16_t)rotated; break;
        case 0x1u: value = rotated & 0xFFFFu; break;
        case 0x4u: value = (uint32_t)(int32_t)(int8_t)(uint8_t)rotated; break;
        case 0x5u: value = rotated & 0xFFu; break;
        default: goto undefined; /* 16-bit dual extends are not modeled */
        }
        cpu->r[rd] = rn == PC ? value : cpu->r[rn] + value;
        return;
    }
    if ((op1 & 0xCu) == 0x8u && (op2 & 0xCu) == 0x8u) {
        /* Miscellaneous operations: Rm appears in both halfwords. */
        unsigned a = op1 & 3u, b = op2 & 3u;
        if (bad_reg(rd) || bad_reg(rm) || rn != rm) goto unpredictable;
        uint32_t x = cpu->r[rm];
        if (a == 1u && b == 0u) { cpu->r[rd] = (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24); return; }
        if (a == 1u && b == 1u) { cpu->r[rd] = ((x >> 8) & 0x00FF00FFu) | ((x << 8) & 0xFF00FF00u); return; }
        if (a == 1u && b == 2u) {
            uint32_t r = 0;
            for (unsigned i = 0; i < 32; ++i) if (x & (UINT32_C(1) << i)) r |= UINT32_C(1) << (31u - i);
            cpu->r[rd] = r; return;
        }
        if (a == 1u && b == 3u) { cpu->r[rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((x & 0xFFu) << 8) | ((x >> 8) & 0xFFu)); return; }
        if (a == 3u && b == 0u) {
            unsigned count = 0;
            while (count < 32u && !(x & (UINT32_C(0x80000000) >> count))) ++count;
            cpu->r[rd] = count; return;
        }
    }
undefined:
    cpu_halt(cpu, "undefined or unmodeled 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
    return;
unpredictable:
    cpu_halt(cpu, "UNPREDICTABLE register operand 0x%04x%04x at 0x%08x", hw1, hw2, pc);
}

static void exec_multiply(cpu_t *cpu, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op1 = (hw1 >> 4) & 7u, op2 = (hw2 >> 4) & 3u;
    unsigned rn = hw1 & 0xFu, ra = (hw2 >> 12) & 0xFu, rd = (hw2 >> 8) & 0xFu, rm = hw2 & 0xFu;
    if (bad_reg(rn) || bad_reg(rd) || bad_reg(rm) || ra == SP) {
        cpu_halt(cpu, "UNPREDICTABLE multiply/divide register at 0x%08x", pc);
        return;
    }
    if (op1 == 0u && op2 == 0u) {            /* MUL / MLA: no NZCV changes */
        cpu->r[rd] = cpu->r[rn] * cpu->r[rm] + (ra == PC ? 0u : cpu->r[ra]);
        return;
    }
    if (op1 == 0u && op2 == 1u && ra != PC) { /* MLS */
        cpu->r[rd] = cpu->r[ra] - cpu->r[rn] * cpu->r[rm];
        return;
    }
    if (op1 == 1u) {                          /* SMULxy / SMLAxy */
        int32_t a = (int16_t)(uint16_t)((hw2 & 0x20u) ? cpu->r[rn] >> 16 : cpu->r[rn]);
        int32_t b = (int16_t)(uint16_t)((hw2 & 0x10u) ? cpu->r[rm] >> 16 : cpu->r[rm]);
        int64_t result = (int64_t)a * b;
        if (ra != PC) {
            result += signed32(cpu->r[ra]);
            if (result != signed32((uint32_t)(result & 0xFFFFFFFF))) cpu->q = true;
        }
        cpu->r[rd] = (uint32_t)(result & 0xFFFFFFFF);
        return;
    }
    if (op1 == 5u && op2 <= 1u) {             /* SMMUL(R) / SMMLA(R) */
        int64_t result = (ra == PC ? 0 : (int64_t)((uint64_t)signed32(cpu->r[ra]) << 32)) +
                         signed32(cpu->r[rn]) * signed32(cpu->r[rm]);
        if (op2 == 1u) result += INT64_C(0x80000000);
        cpu->r[rd] = (uint32_t)((uint64_t)result >> 32);
        return;
    }
    cpu_halt(cpu, "multiply op %u/%u not implemented at 0x%08x", op1, op2, pc);
}

static void exec_long_multiply(cpu_t *cpu, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op1 = (hw1 >> 4) & 7u, op2 = (hw2 >> 4) & 0xFu;
    unsigned rn = hw1 & 0xFu, rdlo = (hw2 >> 12) & 0xFu, rdhi = (hw2 >> 8) & 0xFu, rm = hw2 & 0xFu;
    if (op1 == 1u || op1 == 3u) {
        /* SDIV/UDIV: DIV_0_TRP resets to 0 and this model keeps that fixed
         * policy: division by zero returns 0. SDIV INT_MIN/-1 wraps. */
        if (op2 != 0xFu || rdlo != PC) goto undefined;
        if (bad_reg(rn) || bad_reg(rdhi) || bad_reg(rm)) goto unpredictable;
        uint32_t n = cpu->r[rn], m = cpu->r[rm], result;
        if (m == 0) result = 0;
        else if (op1 == 3u) result = n / m;
        else if (n == 0x80000000u && m == 0xFFFFFFFFu) result = 0x80000000u;
        else result = (uint32_t)(signed32(n) / signed32(m));
        cpu->r[rdhi] = result;
        return;
    }
    if (bad_reg(rn) || bad_reg(rdhi) || bad_reg(rm) || bad_reg(rdlo) || rdhi == rdlo) goto unpredictable;
    uint64_t acc = ((uint64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo], result;
    if (op1 == 0u && op2 == 0u) result = (uint64_t)(signed32(cpu->r[rn]) * signed32(cpu->r[rm]));      /* SMULL */
    else if (op1 == 2u && op2 == 0u) result = (uint64_t)cpu->r[rn] * cpu->r[rm];                        /* UMULL */
    else if (op1 == 4u && op2 == 0u) result = acc + (uint64_t)(signed32(cpu->r[rn]) * signed32(cpu->r[rm])); /* SMLAL */
    else if (op1 == 6u && op2 == 0u) result = acc + (uint64_t)cpu->r[rn] * cpu->r[rm];                  /* UMLAL */
    else if (op1 == 6u && op2 == 6u)                                                                       /* UMAAL */
        result = (uint64_t)cpu->r[rn] * cpu->r[rm] + cpu->r[rdhi] + cpu->r[rdlo];
    else goto undefined;
    cpu->r[rdlo] = (uint32_t)result;
    cpu->r[rdhi] = (uint32_t)(result >> 32);
    return;
undefined:
    cpu_halt(cpu, "undefined or unmodeled 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
    return;
unpredictable:
    cpu_halt(cpu, "UNPREDICTABLE multiply/divide register at 0x%08x", pc);
}

static void exec_branch_misc(cpu_t *cpu, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op = (hw1 >> 4) & 0x7Fu;
    if ((hw2 & 0x5000u) == 0x0000u) {
        if ((op & 0x38u) != 0x38u) {
            /* B<cond> T3 (A7.7.12): raw J2/J1 bits, unlike BL. */
            unsigned cond = (hw1 >> 6) & 15u;
            if (in_it_block(cpu)) { cpu_halt(cpu, "UNPREDICTABLE B<cond> in IT block at 0x%08x", pc); return; }
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
        if ((op & 0x7Eu) == 0x38u && (hw2 & 0x0300u) == 0) {
            /* MSR (register): 1111 0011 100 0 Rn / 10 0 0 mask 0 0 SYSm. */
            unsigned rn = hw1 & 0xFu, mask = (hw2 >> 10) & 3u, sysm = hw2 & 0xFFu;
            if (bad_reg(rn) || mask == 0u || (sysm > 3u && mask != 2u)) {
                cpu_halt(cpu, "UNPREDICTABLE MSR encoding at 0x%08x", pc);
                return;
            }
            if (!write_special(cpu, sysm, mask, cpu->r[rn]))
                cpu_halt(cpu, "MSR SYSm 0x%02x not implemented at 0x%08x", sysm, pc);
            return;
        }
        if (hw1 == 0xF3AFu && (hw2 & 0xFF00u) == 0x8000u) {
            /* NOP.W / YIELD.W / WFE.W / WFI.W / SEV.W / DBG. */
            switch (hw2 & 0xFFu) {
            case 2: if (cpu->event_register) cpu->event_register = false; else cpu->sleeping = true; return;
            case 3: cpu->sleeping = true; return;
            case 4: cpu->event_register = true; return;
            default: return;
            }
        }
        if (hw1 == 0xF3BFu && (hw2 & 0xFF00u) == 0x8F00u) {
            /* CLREX, DSB, DMB, ISB: no caches, buffers or pipeline modeled. */
            unsigned kind = (hw2 >> 4) & 0xFu;
            if (kind == 2u) { cpu->exclusive = false; return; }
            if (kind == 4u || kind == 5u || kind == 6u) return;
        }
        if ((op & 0x7Eu) == 0x3Eu && hw1 == 0xF3EFu && (hw2 & 0xF000u) == 0x8000u) {
            /* MRS: 1111 0011 1110 1111 / 1000 Rd SYSm. */
            unsigned rd = (hw2 >> 8) & 0x0Fu, sysm = hw2 & 0xFFu;
            if (bad_reg(rd)) { cpu_halt(cpu, "UNPREDICTABLE MRS Rd=%u at 0x%08x", rd, pc); return; }
            bool ok;
            uint32_t value = read_special(cpu, sysm, &ok);
            if (!ok) { cpu_halt(cpu, "MRS SYSm 0x%02x not implemented at 0x%08x", sysm, pc); return; }
            cpu->r[rd] = value;
            return;
        }
        cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
        return;
    }
    /* B T4 (bit 12 set, bit 14 clear) and BL (both set). */
    if ((hw2 & 0x1000u) == 0) {
        cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
        return;
    }
    if (in_it_block(cpu) && !last_in_it_block(cpu)) {
        cpu_halt(cpu, "UNPREDICTABLE branch inside IT block at 0x%08x", pc);
        return;
    }
    uint32_t s = (hw1 >> 10) & 1u;
    uint32_t i1 = 1u ^ (((hw2 >> 13) & 1u) ^ s);
    uint32_t i2 = 1u ^ (((hw2 >> 11) & 1u) ^ s);
    uint32_t imm25 = (s << 24) | (i1 << 23) | (i2 << 22)
                   | ((uint32_t)(hw1 & 0x03FFu) << 12)
                   | ((uint32_t)(hw2 & 0x07FFu) << 1);
    int32_t offset = (int32_t)imm25;
    if (s != 0) offset -= INT32_C(33554432);
    if (hw2 & 0x4000u) cpu->r[LR] = (pc + 4u) | 1u;
    cpu->r[PC] = pc + 4u + (uint32_t)offset;
}

static void exec32(cpu_t *cpu, bus_t *bus, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op1 = (hw1 >> 11) & 3u, op2 = (hw1 >> 4) & 0x7Fu;
    if (op1 == 1u) {
        if ((op2 & 0x64u) == 0x00u) {
            /* Load/store multiple (A5.3.5): op = hw1[8:7]: 01 IA, 10 DB. */
            unsigned kind = (hw1 >> 7) & 3u, rn = hw1 & 0xFu;
            bool load = (hw1 & 0x10u) != 0, wback = (hw1 & 0x20u) != 0;
            uint32_t registers = hw2;
            if (kind == 0u || kind == 3u) {
                cpu_halt(cpu, "undefined 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
                return;
            }
            if (rn == PC || bit_count(registers) < 2u || (registers & (1u << SP)) ||
                (!load && (registers & (1u << PC))) ||
                (load && (registers & (1u << PC)) && (registers & (1u << LR))) ||
                (wback && (registers & (1u << rn)))) {
                cpu_halt(cpu, "UNPREDICTABLE LDM/STM register list at 0x%08x", pc);
                return;
            }
            if (load && (registers & (1u << PC)) && in_it_block(cpu) && !last_in_it_block(cpu)) {
                cpu_halt(cpu, "UNPREDICTABLE LDM to PC inside IT block at 0x%08x", pc);
                return;
            }
            load_store_multiple(cpu, bus, pc, rn, registers, load, kind == 2u, wback);
            return;
        }
        if ((op2 & 0x64u) == 0x04u) {
            exec_dual_exclusive_table(cpu, bus, pc, hw1, hw2);
            return;
        }
        if ((op2 & 0x60u) == 0x20u) {
            /* Data processing (shifted register), A5.3.11. */
            unsigned op = (hw1 >> 5) & 0xFu, rn = hw1 & 0xFu, rd = (hw2 >> 8) & 0xFu, rm = hw2 & 0xFu;
            bool setflags = (hw1 & 0x10u) != 0;
            if ((hw2 & 0x8000u) || (bad_reg(rm) && !(rm == SP && op == 2u && rn == PC && !setflags))) {
                cpu_halt(cpu, "UNPREDICTABLE shifted-register operand at 0x%08x", pc);
                return;
            }
            unsigned srtype, amount;
            decode_imm_shift((hw2 >> 4) & 3u, ((hw2 >> 12) & 7u) << 2 | ((hw2 >> 6) & 3u), &srtype, &amount);
            shift_result_t shifted = shift_c(cpu->r[rm], srtype, amount, cpu->c);
            data_processing(cpu, pc, op, setflags, rn, rd, shifted.value, shifted.carry, false);
            return;
        }
        cpu_halt(cpu, "coprocessor/FP instruction 0x%04x%04x not modeled at 0x%08x", hw1, hw2, pc);
        return;
    }
    if (op1 == 2u) {
        if (hw2 & 0x8000u) {
            exec_branch_misc(cpu, pc, hw1, hw2);
            return;
        }
        if ((op2 & 0x20u) == 0) {
            /* Data processing (modified immediate), A5.3.1. */
            unsigned op = (hw1 >> 5) & 0xFu, rn = hw1 & 0xFu, rd = (hw2 >> 8) & 0xFu;
            uint16_t imm12 = (uint16_t)((((hw1 >> 10) & 1u) << 11) | (((hw2 >> 12) & 7u) << 8) | (hw2 & 0xFFu));
            thumb_imm_t immediate;
            if (!thumb_expand_imm(imm12, cpu->c, &immediate)) {
                cpu_halt(cpu, "UNPREDICTABLE modified immediate 0x%03x at 0x%08x", (unsigned)imm12, pc);
                return;
            }
            data_processing(cpu, pc, op, (hw1 & 0x10u) != 0, rn, rd, immediate.value, immediate.carry, true);
            return;
        }
        exec_plain_immediate(cpu, pc, hw1, hw2);
        return;
    }
    /* op1 == 3 */
    if ((op2 & 0x71u) == 0x00u || (op2 & 0x67u) == 0x01u || (op2 & 0x67u) == 0x03u ||
        (op2 & 0x67u) == 0x05u) {
        exec_load_store_single(cpu, bus, pc, hw1, hw2);
        return;
    }
    if ((op2 & 0x70u) == 0x20u) { exec_data_processing_register(cpu, pc, hw1, hw2); return; }
    if ((op2 & 0x78u) == 0x30u) { exec_multiply(cpu, pc, hw1, hw2); return; }
    if ((op2 & 0x78u) == 0x38u) { exec_long_multiply(cpu, pc, hw1, hw2); return; }
    cpu_halt(cpu, "undefined or coprocessor 32-bit instruction 0x%04x%04x at 0x%08x", hw1, hw2, pc);
}

static void finish_step(cpu_t *cpu)
{
    cpu->steps++;
    if (cpu->step_hook) {
        cpu->step_hook(cpu->step_hook_ctx);
    }
}

int cpu_step(cpu_t *cpu, bus_t *bus)
{
    if (cpu->halted) {
        return -1;
    }

    /* A sleeping core (WFI/WFE) wakes on an exception that could preempt if
     * PRIMASK were clear. While asleep, each step executes no instruction
     * but still counts, so instruction-clocked timers keep running.
     */
    if (cpu->sleeping) {
        if (wake_pending(cpu)) {
            cpu->sleeping = false;
        } else {
            cpu->sleep_steps++;
            finish_step(cpu);
            return cpu->halted ? -1 : 0;
        }
    }

    /* Asynchronous exceptions are taken at instruction boundaries when the
     * winner's group priority is higher than the execution priority. A taken
     * handler executes its first instruction in this same step; returning and
     * re-entering stands in for tail-chaining (same architectural state).
     */
    uint32_t any_pending = 0;
    for (unsigned i = 0; i < 8; ++i) any_pending |= cpu->pending_exceptions[i];
    if (any_pending) {
        unsigned number = cpu_pending_winner(cpu);
        if (number && can_preempt(cpu, number, true)) {
            if (!exception_enter(cpu, bus, number, cpu->r[PC])) {
                if (bus_fault(bus)) cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
                return -1;
            }
            cpu_clear_pending_exception(cpu, number);
        }
    }

    uint32_t pc = cpu->r[PC];
    uint16_t hw1 = bus_read16(bus, pc);
    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
        return -1;
    }

    /* A5.1: prefixes 11101, 11110 and 11111 start 32-bit instructions. */
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
    uint8_t it_before = cpu->itstate;
    cpu->it_exec = it_before;
    cpu->itstate = it_advance(it_before);
    cpu->r[PC] = pc + (wide ? 4u : 2u);
    bool skip = (it_before & 0x0Fu) != 0 && !condition_passed(cpu, it_before >> 4) &&
                !(!wide && (hw1 & 0xFF00u) == 0xBE00u); /* BKPT is unconditional */
    if (!skip) {
        if (wide) exec32(cpu, bus, pc, hw1, hw2);
        else exec16(cpu, bus, pc, hw1);
    }
    cpu->it_exec = 0;

    /* cpu->r[PC] tracks the next instruction. A PC operand uses pc + 4 instead. */
    if (bus_fault(bus)) {
        cpu_halt(cpu, "bus fault: %s", bus_fault_msg(bus));
    }
    /* Keep register dumps at the instruction that stopped execution. */
    if (cpu->halted) {
        cpu->r[PC] = pc;
        cpu->itstate = it_before;
    }
    finish_step(cpu);
    return cpu->halted ? -1 : 0;
}
