@ USART2 transmit: enable clocks, route PA2 to AF7 (USART2_TX), set BRR and CR1,
@ then send "OK\n" polling TXE before each byte and TC after the last one.
@ The model completes each transfer at once; this program only needs TXE/TC = 1.
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text
    .word 0x20020000            @ initial SP
    .word _start                @ reset vector

    .thumb_func
    .global _start
_start:
    ldr r0, lit_rcc             @ RCC_AHB1ENR
    ldr r1, [r0, #0]
    orr.w r1, r1, #1            @ GPIOAEN
    str r1, [r0, #0]
    ldr r1, [r0, #0x10]         @ RCC_APB1ENR
    orr.w r1, r1, #0x20000      @ USART2EN (bit 17)
    str r1, [r0, #0x10]

    ldr r0, lit_gpioa
    ldr r1, [r0, #0]            @ MODER
    bic.w r1, r1, #0x30
    orr.w r1, r1, #0x20         @ PA2 = alternate function
    str r1, [r0, #0]
    ldr r1, [r0, #0x20]         @ AFRL
    orr.w r1, r1, #0x700        @ PA2 = AF7
    str r1, [r0, #0x20]

    ldr r0, lit_usart2
    movs r1, #0x8B
    str r1, [r0, #8]            @ BRR: 115200 baud at 16 MHz, OVER8 = 0
    movw r1, #0x2008
    str r1, [r0, #0x0C]         @ CR1 = UE | TE

    movs r1, #'O'
    bl putc
    movs r1, #'K'
    bl putc
    movs r1, #'\n'
    bl putc
wait_tc:
    ldr r2, [r0, #0]
    and.w r2, r2, #0x40
    cmp r2, #0x40
    bne wait_tc
    ldr r6, [r0, #0]            @ final SR
    ldr r7, [r0, #0x0C]         @ CR1 read-back
    bkpt #0

    .thumb_func
putc:                           @ r0 = USART2 base, r1 = byte
    ldr r2, [r0, #0]
    and.w r2, r2, #0x80
    cmp r2, #0x80
    bne putc                    @ wait for TXE
    str r1, [r0, #4]            @ DR
    bx lr
    .align 2
lit_rcc:    .word 0x40023830
lit_gpioa:  .word 0x40020000
lit_usart2: .word 0x40004400
