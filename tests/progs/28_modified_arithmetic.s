@ I5: modified immediates, arithmetic carry, CMP/CMN aliases and SP forms.
.include "vectors.inc"
_start:
    movs r7, #16
    cmp r7, r7               @ Z=1 C=1; non-S forms preserve NZCV
    add.w r3, r7, #8         @ f107 0308 =>24
    sub.w r3, r3, #65536     @ f5a3 3380 =>ffff0018
    rsb.w r2, r3, #0x80000000 @ f1c3 4200 =>8000ffe8
    mvn.w r1, #1024          @ f46f 6180 =>fffffbff
    bne fail
    bcc fail
    ldr r3, compare_value
    cmp.w r3, #32768         @ f5b3 4f00 =>Z1 C1, not expanded-immediate carry
    bne fail
    bcc fail
    ldr r3, negative_value
    cmn.w r3, #32768         @ f513 4f00 =>0, Z1 C1
    bne fail
    bcc fail
    ldr r3, overflow_value
    cmn.w r3, #32768         @ 80000000, N1 C0 V1
    bvc fail
    bpl fail
    bcs fail
    add.w sp, sp, #8
    sub.w sp, sp, #8
    bvc fail                 @ SP operations preserve flags
    cmp.w sp, #8             @ legal SP compare alias, N0 Z0 C1 V0
    bkpt #0
fail:
    bkpt #1
.align 2
compare_value: .word 0x00008000
negative_value: .word 0xffff8000
overflow_value: .word 0x7fff8000
