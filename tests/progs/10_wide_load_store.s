    .include "vectors.inc"
_start:
    ldr r0, =0x20000100
    ldr r1, =0x12345678
    str.w r1, [r0, #0x40]
    ldr.w r2, [r0, #0x40]
    ldrb r3, [r0, #0x40]        @ little-endian: lowest byte is 0x78
    bkpt #0
    .ltorg
