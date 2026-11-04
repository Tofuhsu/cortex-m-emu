@ SVC exception: entry stacks 8 words and loads LR with EXC_RETURN; BX LR returns.
@ The handler records what it sees in SRAM, then changes the caller's r0 through
@ the stacked frame (writing r0 directly would be undone by unstacking).
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text
    .word 0x20020000            @ 0: initial SP
    .word _start                @ 1: Reset
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0  @ 2-10: NMI ... reserved
    .word svc_handler           @ 11: SVCall
    .word 0, 0, 0, 0            @ 12-15: DebugMonitor, reserved, PendSV, SysTick

    .thumb_func
    .global _start
_start:
    movs r0, #1
    movs r1, #2
    movs r2, #3
    movs r3, #4
    svc #5
    bkpt #0

    .thumb_func
svc_handler:
    mrs r2, ipsr                @ exception number of the active exception
    ldr r3, lit_sram
    str r2, [r3, #0]            @ [0x20000000] = IPSR
    mov r2, lr
    str r2, [r3, #4]            @ [0x20000004] = EXC_RETURN
    mov r2, sp
    str r2, [r3, #8]            @ [0x20000008] = SP inside the handler (frame base)
    movs r2, #42
    str r2, [sp, #0]            @ stacked r0 = 42
    bx lr                       @ exception return
    .align 2
lit_sram: .word 0x20000000
