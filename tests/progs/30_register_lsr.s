@ I7: count is low byte; 0,32,33,256 and flag-setting versus preserving.
.include "vectors.inc"
_start:
    ldr r2, source
    movs r3, #0
    cmp r3, r3              @ Z1 C1
    lsr.w r3, r2, r3        @ fa22 f303, S=0 preserves Z1 C1
    bne fail
    bcc fail
    movs r4, #32
    lsrs.w r5, r2, r4       @ zero, C=bit31=1
    bne fail
    bcc fail
    movs r4, #33
    lsrs.w r6, r2, r4       @ zero C0
    bne fail
    bcs fail
    ldr r4, count256
    lsrs.w r7, r2, r4       @ count0, source unchanged, C0 N1
    bpl fail
    bcs fail
    bkpt #0
fail:
    bkpt #1
.align 2
source: .word 0x80000001
count256: .word 256
