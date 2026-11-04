    .include "vectors.inc"
_start:
    ldr r0, lit_sram            @ explicit label: `ldr r0, =imm` would become MOV.W (32-bit)
    movs r1, #0xAB
    str r1, [r0, #4]
    ldr r2, [r0, #4]
    sub sp, #8
    str r1, [sp, #4]
    ldr r3, [sp, #4]
    add sp, #8
    bkpt #0
    .align 2
lit_sram: .word 0x20000000
