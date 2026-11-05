@ PRIMASK: CPSID i masks SysTick. The counter keeps running and the request stays
@ pending (several 1 -> 0 events coalesce into one pending exception). CPSIE i
@ unmasks it, and the pending exception is taken before the next instruction.
@ The handler stops SysTick first, so it runs exactly once.
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text
    .word 0x20020000            @ 0: initial SP
    .word _start                @ 1: Reset
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0  @ 2-10
    .word 0                     @ 11: SVCall
    .word 0, 0, 0               @ 12-14
    .word systick_handler       @ 15: SysTick

    .thumb_func
    .global _start
_start:
    cpsid i                     @ PRIMASK = 1
    ldr r0, lit_systick
    movs r1, #9
    str r1, [r0, #4]            @ SYST_RVR = 9: an event every 10 ticks
    str r1, [r0, #8]            @ clear SYST_CVR
    movs r4, #0
    movs r1, #3
    str r1, [r0, #0]            @ ENABLE | TICKINT
    movs r5, #20
spin:
    subs r5, #1                 @ 40 instructions: several SysTick events while masked
    bne spin
    mov r7, r4                  @ r7 = handler count while masked (0)
    mrs r6, primask             @ r6 = 1
    cpsie i                     @ PRIMASK = 0: the pending SysTick is taken here
after_cpsie:
    nop                         @ stacked return address
    mrs r3, primask             @ r3 = 0
    bkpt #0

    .thumb_func
systick_handler:
    movs r1, #0
    str r1, [r0, #0]            @ stop SysTick
    adds r4, #1
    ldr r1, lit_sram
    ldr r2, [sp, #24]
    str r2, [r1, #0]            @ [0x20000000] = stacked return address
    bx lr
    .align 2
lit_systick: .word 0xE000E010
lit_sram:    .word 0x20000000
