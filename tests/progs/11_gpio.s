    .include "vectors.inc"
_start:
    ldr r2, lit_odr
    movs r3, #0x20
    str r3, [r2]                @ GPIOA clock still off: this write must be ignored
    ldr r0, lit_ahb1enr
    movs r1, #1
    str r1, [r0]                @ RCC_AHB1ENR.GPIOAEN = 1
    ldr r4, lit_moder
    ldr r1, lit_moder_val
    str r1, [r4]                @ PA5 = general-purpose output (MODER5 = 01)
    ldr r5, [r4]                @ read back MODER
    ldr r6, lit_bsrr
    str r3, [r6]                @ BSRR set PA5   -> ODR = 0x20
    ldr r1, lit_reset5
    str r1, [r6]                @ BSRR reset PA5 -> ODR = 0
    ldr r1, lit_both5
    str r1, [r6]                @ set and reset PA5 together: set wins -> ODR = 0x20
    ldr r7, [r2]                @ read back ODR
    bkpt #0
    .align 2
lit_odr:       .word 0x40020014
lit_ahb1enr:   .word 0x40023830
lit_moder:     .word 0x40020000
lit_moder_val: .word 0xA8000400
lit_bsrr:      .word 0x40020018
lit_reset5:    .word 0x00200000
lit_both5:     .word 0x00200020
