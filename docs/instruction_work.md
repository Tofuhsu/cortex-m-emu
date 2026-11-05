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
