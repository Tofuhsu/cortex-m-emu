    .include "vectors.inc"
_start:
    movs r0, #1
    subs r0, #2                 @ 1 - 2 = 0xFFFFFFFF (borrow)
    movs r1, #200
    adds r1, r1, #7             @ 3-bit immediate form
    movs r2, #5
    subs r2, #5                 @ 0: Z=1, C=1 (no borrow)
    bkpt #0
