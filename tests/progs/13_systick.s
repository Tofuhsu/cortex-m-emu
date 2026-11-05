@ SysTick interrupt: RVR = 99 gives one interrupt every 100 ticks. In this model
@ one tick = one executed instruction (not a clock cycle, not time). The handler
@ counts interrupts in r4 (not stacked, so the count survives return) and records
@ what it sees on the first entry. Thread code waits for 3 interrupts, then stops.
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
    ldr r0, lit_systick         @ SYST_CSR
    movs r1, #99
    str r1, [r0, #4]            @ SYST_RVR = 99
    str r1, [r0, #8]            @ any write clears SYST_CVR to 0
    movs r4, #0
    movs r1, #3
    str r1, [r0, #0]            @ SYST_CSR = ENABLE | TICKINT
wait:
    cmp r4, #3
    bne wait
    movs r1, #0
    str r1, [r0, #0]            @ stop the counter
    ldr r6, [r0, #0]            @ CSR: COUNTFLAG was set by the last 1 -> 0
    ldr r7, [r0, #0]            @ CSR again: the first read cleared COUNTFLAG
    bkpt #0

    .thumb_func
systick_handler:
    cmp r4, #0
    bne count
    ldr r1, lit_sram
    mrs r2, ipsr
    str r2, [r1, #0]            @ [0x20000000] = IPSR
    mov r2, lr
    str r2, [r1, #4]            @ [0x20000004] = EXC_RETURN
    ldr r2, [sp, #24]
    str r2, [r1, #8]            @ [0x20000008] = stacked return address
count:
    adds r4, #1
    bx lr
    .align 2
lit_systick: .word 0xE000E010
lit_sram:    .word 0x20000000
