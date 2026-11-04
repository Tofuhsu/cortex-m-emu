    .include "vectors.inc"
_start:
    movs r0, #3
    movs r1, #0
loop:
    adds r1, #2
    subs r0, #1
    bne loop
    bkpt #0
