    .include "vectors.inc"
_start:
    ldr r0, lit_rcc             @ PC-relative load from the literal pool
    ldr r1, lit_max             @ explicit label: `ldr r1, =imm` would become MVN.W (32-bit)
    adds r1, #1                 @ signed overflow: V=1, N=1
    bkpt #0
    .align 2
lit_rcc: .word 0x40023830
lit_max: .word 0x7FFFFFFF
