@ RM0390 Table 38: USART2 IRQ38 => exception54. PM0214 4.3 ISER/ISPR.
@ Register expectations are checked by test_extended.c; matches Renode up to IRQ entry (docs/renode_differences.md).
.syntax unified
.cpu cortex-m4
.thumb
.text
.word 0x20020000, _start
.rept 52
.word 0
.endr
.word irq_handler
.org 0x100
.thumb_func
.global _start
_start:
    ldr r0, iser1
    movs r1, #64
    str r1, [r0]
    ldr r0, ispr1
    str r1, [r0]
    nop
    bkpt #0
.org 0x160
.thumb_func
irq_handler:
    mrs r2, ipsr
    str r2, [sp, #0]
    bx lr
.org 0x180
iser1: .word 0xE000E104
ispr1: .word 0xE000E204
