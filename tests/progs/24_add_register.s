@ I1: ADD register T2; no flag changes, high register and SP operands.
@ DDI0403E.e A7.7.4/A7.7.6. Must be assembled by GNU as and traced in Renode.
.include "vectors.inc"
_start:
    movs r2, #5
    movs r3, #7
    movs r0, #4
    cmp r2, r2             @ NZCV=0110
    add r2, r3             @ exact objdump example: 441a; r2=12, flags unchanged
    mov r8, r2
    add r8, r3             @ r8=19
    mov r4, sp
    add r4, r3             @ r4=initial SP+7; destination is not SP
    add sp, r0
    mov r5, sp             @ r5=0x20020004
    sub sp, #4
    bkpt #0
