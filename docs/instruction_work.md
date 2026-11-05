# Instruction integration after the checkpoint

The emulator already contains the eight checkpoint commits through `696215b`;
the firmware contains its two commits through `ce798b6`. Reapplying those same
patches is unnecessary. Their original authorship is preserved. No push is
performed; the existing README changes remain local pending the owner's list.

Reference: [Arm DDI0403E.e](https://documentation-service.arm.com/static/606dc36485368c4c2b1bf62f).
Page labels below are the printed manual labels, not viewer offsets.

## I1

ADD register T2 is checked against A7.7.4, A7-192–193, and the SP aliases
against A7.7.6, A7-196–197. PC operands use instruction address plus four;
writing PC uses ALUWritePC, not BX interworking. Both operands PC are rejected.
A5.1.3, A5-127, defines SP[1:0] as SBZP: unaligned SP writes halt explicitly.
The ADD 44xx and MOV 46xx masks are disjoint. Tests exercise all source/destination
registers, every initial NZCV state, the exact 441a example, and unsigned wrap.
Program 24 checks high registers and SP while preserving a known NZCV state.

Direct-opcode C tests are unit checks. They do not substitute for GNU assembly
of the .s files or real Renode traces; those remain pending in this environment.

## I2–I4 and shared helpers

DDI0403E.e A7.7.9 (A7-201) supplies AND's implicit LSL0; A7.7.119
(A7-341) supplies NEG/RSB flags; A7.4.2 (A7-183) distinguishes the zero
immediate encodings. Arithmetic/shift helpers are grouped near AddWithCarry.
Logical operations preserve C/V; NEG uses AddWithCarry. Immediate ASR and the
other byte-extension instructions are intentionally left unsupported. Program
25 checks NEG zero/INT_MIN; 26 checks LSL0/LSR32/carry; 27 checks UXTB while
preserving flags. Direct tests cover all low register fields and initial flags.

## I5–I10

Arithmetic modified immediates now handle ADD/SUB/RSB, MVN, CMP and CMN.
Compare aliases are decoded before the PC destination check. ADD/SUB with SP
are supported, including SP comparison aliases; unaligned SP results halt
before changing registers or flags. Logical S=1, ORN, ADDW/SUBW remain explicit
unsupported cases. Arithmetic NZCV uses AddWithCarry, never immediate carry.

B conditional T3 uses the raw J2/J1 bits and keeps conditions 14/15 outside
the branch decoder. MRS/MSR still decode first. The supplied `f040 8118`
example reaches 080004be from 0800028a. Register shifts implement LSR only:
the low byte selects the amount, with separate cases for zero, 32 and greater
than 32. MUL truncates unsigned multiplication; UDIV explicitly returns zero
for a zero divisor under the fixed reset DIV_0_TRP=0 policy. SCB/CCR writes
are not implemented. LDR literal T2 supports both offset signs and aligned
PC+4; PC destinations remain unsupported. Failed loads preserve Rt.

Reference locators: DDI0403E.e A5.3.1–2, A7.7.12 (A7-205–206), LSR register
(A7-285), UDIV (A7-426), A4.4.6 and B3.2.8; LDR literal is A7.7.44 in this
edition. Programs 28–33 exercise these families using assembly mnemonics,
including both branch directions, SP aliases and both literal offset signs.
Program 19 is unchanged: its GNU wide literal must work without forcing `.n`.

## Verification boundary, 2026-11-05

The following ran locally after the changes:

- Instruction unit checks: all ten groups pass, including all 16 initial NZCV
  states, source/destination aliases and the supplied objdump vectors.
- The same instruction unit checks pass with GCC undefined-behavior sanitizer.
- The existing test runner passes 20/20 against the original supplied binaries.
- Extended host checks pass for NVIC, I2C/GPIOB, BME280 and MSR PRIMASK.
- Firmware host driver/compensation tests pass; this is host C execution.

The Makefile's full `test` target now runs programs 24–33 through
`test_instructions --programs`. The `test-host` target does not require ARM
assembly. GNU ARM tools, Renode and a waveform viewer are absent here, so a
clean GNU build, actual program 24–33 execution, Robot 4/4, differential matrix
and viewer screenshot remain pending. No hand-encoded program fixtures were
created for these tests. Existing firmware.bin is from the older firmware;
it is not evidence for the new BME280 firmware. Do not mark acceptance passed.

## L: full-matrix orchestration prepared, integration not run

`python3 tools/run_trace_matrix.py --renode-test /absolute/path/to/renode-test`
forces GNU assembly of every program, builds the target firmware, and exports
both traces for programs 01–33 plus a 20,000-instruction firmware prefix.
It keeps per-case errors/mismatches and exact input hashes in a fresh output
directory, rather than stopping at the first mismatch or comparing only 03.
The default prefix is provisional: inspect the target disassembly/trace to
confirm I2C initialization is covered; increase it if it is not.

Only the existing exact reset-LR allowlist is accepted. BKPT is explicitly
excluded from both traces. Program01 contains only BKPT, so it is reported
RESET_ONLY and cannot count as a passed instruction comparison. Peripheral
timing/calibration mismatches are retained, never auto-allowlisted. In
particular instruction-ticked SysTick and Renode's virtual-time SysTick do
not share an event schedule, and the two BME280 calibrations are different.
Those inputs need a deliberate comparison policy after observing real traces.

The script's Python syntax and missing-tool preflight have been checked here.
Its Renode integration has not been executed. Running it with tools missing
returns NOT RUN before rebuilding anything. It performs no clean, push or
README update. It returns nonzero for incomplete or mismatching matrices,
including the explicitly separate reset-only case.
