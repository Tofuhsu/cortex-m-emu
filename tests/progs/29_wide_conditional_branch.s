@ I6: exact f040 8118 at 0800028a, plus negative offset and not-taken.
.include "vectors.inc"
_start:
    b setup
.org 0x286
setup:
    movs r0, #1
    cmp r0, #0
    bne.w far_target         @ 0800028a +4 +0x230 =080004be
    bkpt #1
.org 0x4be
far_target:
    movs r7, #2
loop:
    subs r7, #1
    bne.w loop               @ negative signed 21-bit offset
    bkpt #0
