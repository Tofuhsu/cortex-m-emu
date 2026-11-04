    .include "vectors.inc"
_start:
    ldr r1, =0xA8000000
    bic.w r1, r1, #0xC00        @ clear PA5 mode bits
    orr.w r1, r1, #0x400        @ PA5 = output
    eor.w r1, r1, #0x20
    movs r2, #0
    orr.w r2, r2, #0x00AB00AB   @ modified immediate, pattern 0x00XY00XY
    bkpt #0
    .ltorg
