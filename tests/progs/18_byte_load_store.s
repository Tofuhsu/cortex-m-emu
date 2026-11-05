@ STRB/LDRB (immediate) T1: byte offsets are not scaled; LDRB zero-extends.
    .include "vectors.inc"
_start:
    ldr r0, lit_sram
    ldr r1, lit_pattern
    str r1, [r0, #0]            @ [0x20000000] = 0x80FF1234
    ldrb r2, [r0, #3]           @ r2 = 0x80 (zero-extended, not 0xFFFFFF80)
    ldrb r3, [r0, #2]           @ r3 = 0xFF
    movs r4, #0xA5
    strb r4, [r0, #1]           @ [0x20000000] = 0x80FFA534
    ldr r5, [r0, #0]
    strb r4, [r0, #31]          @ largest imm5: [0x2000001F] = 0xA5
    ldr r6, [r0, #28]
    bkpt #0
    .align 2
lit_sram:    .word 0x20000000
lit_pattern: .word 0x80FF1234
