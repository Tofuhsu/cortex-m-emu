# Week 1: reset, fetch/decode/execute, 16-bit Thumb

Goal: `make test` passes `test_reset` through `test_cond_branches` (tests 01–08).
Tests 09–10 (32-bit Thumb-2) are week 2.

## Read first

- ARMv7-M Architecture Reference Manual (Arm DDI 0403E, free from developer.arm.com):
  - A2.3 (registers), A2.2 pseudocode for `AddWithCarry`
  - A5.1–A5.2: how to tell 16-bit from 32-bit, and the 16-bit encoding tables
  - A7.7: one page per instruction (encoding, pseudocode, flags)
  - "Reset behavior" in chapter B1
- `arm-none-eabi-objdump -d build/progs/<name>.elf` shows each test's encodings next to the source.

## Core ideas

1. **The loop.** `cpu_step` = fetch the halfword at PC → decide 16 or 32 bits → advance PC → execute.
   An emulator is a big `switch` on bit patterns.
2. **PC reads as +4.** In Thumb, an instruction that uses PC as an operand sees its own address + 4.
   `LDR r0, [PC, #imm]` (literal) also aligns that value down to a multiple of 4 first.
   Branch offsets are relative to the same +4 value.
3. **Flags.** N = bit 31 of the result, Z = result is 0. C and V come from `AddWithCarry`:
   C is the unsigned carry out (for subtraction: 1 means *no borrow*); V is signed overflow.
   Only instructions with `S` (MOVS, ADDS, SUBS, CMP) change flags.
4. **Thumb bit.** Addresses loaded into PC by `BX`, `POP {pc}` and the reset vector have bit 0 = 1
   to mean Thumb. Clear it before using the address. Cortex-M only runs Thumb; bit 0 = 0 is a fault.
5. **Halting.** `BKPT` should call `cpu_halt(cpu, "bkpt #%u", imm)`. The tests look for the "bkpt" prefix.
   Leave PC at the BKPT's own address, as a debugger halt reports it (`test_reset` checks this).
   The BKPT still counts as one step.

## 16-bit decode, top level (bits [15:10], ARM ARM A5.2)

| bits 15–10 | group | instructions the tests need |
| --- | --- | --- |
| `00xxxx` | shift, add, subtract, move, compare | MOVS imm8, ADDS imm3/imm8, SUBS imm8, CMP imm8 |
| `010000` | data processing (register) | (later) |
| `010001` | special data, branch and exchange | BX, MOV (register) |
| `01001x` | load from literal pool | LDR literal |
| `0101xx`, `011xxx`, `100xxx` | load/store single | STR/LDR imm5, LDRB imm5, STR/LDR SP-relative |
| `1011xx` | miscellaneous | PUSH, POP, ADD/SUB SP imm7, BKPT, NOP |
| `1101xx` | conditional branch, SVC | B<cond> |
| `11100x` | unconditional branch | B |

Write a `condition_passed(cpu, cond)` helper for B<cond> (ARM ARM A7.3, table of condition codes).

## Order that works well

1. `cpu_reset` + BKPT + the fetch loop → `test_reset`
2. MOVS → `test_movs`
3. `add_with_carry`, ADDS, SUBS → `test_adds_subs`
4. B<cond> → `test_branch_loop`
5. LDR literal → `test_ldr_literal`
6. LDR/STR immediate, SP-relative, ADD/SUB SP → `test_load_store`
7. PUSH/POP, BX, then BL (your first 32-bit instruction) → `test_calls`
8. CMP and the other condition codes → `test_cond_branches`

After each step: `make test`, then commit.

## Check yourself

- Why is the reset PC `0x08000008` when the vector holds `0x08000009`?
- `subs r0, #2` with r0 = 1: why is C = 0? What would `cmp r0, #2` do differently?
- PUSH stores the highest-numbered register at the highest address. Why does that make POP simple?
- What does Renode set LR to after reset, and why do the tests use 0xFFFFFFFF instead?
