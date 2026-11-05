@ Pending IRQ38 without ISER: no handler. r7 remains zero; ISPR bank1 bit6 remains.
@ Renode single-step comparison pending: runtime unavailable in authoring environment.
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
    movs r7, #0
    ldr r0, ispr1
    movs r1, #64
    str r1, [r0]
    nop
    bkpt #0
.thumb_func
irq_handler:
    movs r7, #99
    bx lr
.align 2
ispr1: .word 0xE000E204
