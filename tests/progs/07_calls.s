    .include "vectors.inc"
_start:
    movs r0, #1
    bl f1                       @ 32-bit BL, LR = return address | 1
    bl f2
    adds r0, #10
    bkpt #0
    .thumb_func
f1:
    push {r7, lr}
    adds r0, #5
    pop {r7, pc}                @ return by popping into PC
    .thumb_func
f2:
    adds r0, #100
    bx lr
