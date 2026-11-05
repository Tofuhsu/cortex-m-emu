@ I9: unsigned truncation, aliases, reset DIV_0_TRP=0, no flag changes.
.include "vectors.inc"
_start:
    ldr r2, source
    movs r3, #3
    movs r4, #0
    cmp r3, r3              @ Z1 C1
    udiv r3, r2, r3         @ fbb2 f3f3 =>55555555
    bne fail
    bcc fail
    udiv r5, r2, r4         @ divide by zero =>0 under reset policy
    bne fail
    bcc fail
    udiv r2, r2, r2         @ alias destination and both inputs =>1
    bne fail
    bcc fail
    bkpt #0
fail:
    bkpt #1
.align 2
source: .word 0xffffffff
