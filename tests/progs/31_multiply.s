@ I8: low 32 bits, high registers, destination aliases, preserved flags.
.include "vectors.inc"
_start:
    ldr r2, source
    movs r3, #3
    cmp r3, r3              @ Z1 C1
    mul r3, r2, r3          @ fb02 f303 =>fffffffa, S=0
    bne fail
    bcc fail
    mov r8, r3
    mul r8, r8, r3          @ (-6)*(-6) modulo2^32 =>36
    bne fail
    bcc fail
    mul r2, r2, r2          @ (-2)^2 =>4
    bne fail
    bcc fail
    bkpt #0
fail:
    bkpt #1
.align 2
source: .word 0xfffffffe
