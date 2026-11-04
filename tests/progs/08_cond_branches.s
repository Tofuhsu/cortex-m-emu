    .include "vectors.inc"
_start:
    movs r0, #5
    movs r1, #0
    movs r2, #0
    movs r3, #0
    cmp r0, #3
    bhi 1f                      @ unsigned higher: taken
    movs r1, #1
1:
    movs r2, #2
    cmp r0, #9
    blt 2f                      @ signed less than: taken
    movs r3, #0xEE
2:
    cmp r0, #5
    bne 3f                      @ not taken
    adds r3, #1
3:
    bkpt #0
