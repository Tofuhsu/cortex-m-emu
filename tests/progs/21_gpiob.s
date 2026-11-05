@ GPIOB/AFRH. Renode may ignore clock gating; only enabled accesses here.
.include "vectors.inc"
_start:
    ldr r0, rcc
    movs r1, #2
    str r1, [r0, #0x30]
    ldr r0, gpiob
    ldr r2, [r0]            @ MODER reset = 0x280
    ldr r3, [r0, #0x0c]    @ PUPDR reset = 0x100
    ldr r4, [r0, #8]       @ OSPEEDR reset = 0xc0 (RM0390 Rev9 7.4.3)
    movs r1, #0x44
    adds r0, #0x20
    strb r1, [r0, #4]
    subs r0, #0x20
    ldr r5, [r0, #0x24]
    bkpt #0
.align 2
rcc: .word 0x40023800
gpiob: .word 0x40020400
