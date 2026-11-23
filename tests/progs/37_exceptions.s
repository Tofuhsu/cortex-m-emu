@ Exception model for an RTOS: Thread mode on PSP (CONTROL.SPSEL), SVC with
@ EXC_RETURN 0xFFFFFFFD, BASEPRI masking, IRQ priorities from NVIC IPR, a
@ nested higher-priority IRQ (EXC_RETURN 0xFFFFFFF1), and a lowest-priority
@ PendSV that waits for the IRQ to return and is then taken before Thread
@ code runs again. Handlers append words to a log at 0x20000004 (count at
@ 0x20000000).
    .syntax unified
    .cpu cortex-m4
    .thumb
    .equ LOG,   0x20000000
    .equ ICSR,  0xE000ED04
    .equ SHPR3, 0xE000ED20
    .equ ISER0, 0xE000E100
    .equ ISPR0, 0xE000E200
    .equ IPR0,  0xE000E400
    .text
    .word 0x20020000            @ 0: initial MSP
    .word _start                @ 1: Reset
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0  @ 2-10
    .word svc_handler           @ 11: SVCall
    .word 0, 0                  @ 12-13
    .word pendsv_handler        @ 14: PendSV
    .word 0                     @ 15: SysTick
    .word irq0_handler          @ 16: IRQ0
    .word irq1_handler          @ 17: IRQ1

    .thumb_func
    .global _start
_start:
    ldr r0, =SHPR3
    ldr r1, =0x00F00000         @ PendSV priority 0xF0 (lowest)
    str r1, [r0]
    ldr r0, =IPR0
    movs r1, #0x80
    strb r1, [r0]               @ IRQ0 priority 0x80
    movs r1, #0x40
    strb r1, [r0, #1]           @ IRQ1 priority 0x40 (higher)
    ldr r0, =ISER0
    movs r1, #3
    str r1, [r0]
    ldr r0, =0x20010000
    msr psp, r0
    movs r0, #2
    msr control, r0             @ Thread mode now uses PSP
    isb
    mov r4, sp                  @ r4 = PSP
    mrs r5, msp                 @ r5 = MSP
    svc #1
    movs r0, #0x80
    msr basepri, r0             @ masks priority 0x80 and lower
    ldr r0, =ISPR0
    movs r1, #1
    str r1, [r0]                @ pend IRQ0: masked by BASEPRI
    movs r0, #0xA0
    bl log_append
    movs r0, #0
    msr basepri, r0             @ IRQ0 is taken before the next instruction
    movs r0, #0xA1
    bl log_append
    ldr r0, =ICSR
    mov.w r1, #0x10000000       @ PENDSVSET from Thread mode
    str r1, [r0]
    movs r0, #0xA2
    bl log_append
    mrs r6, control
    bkpt #0

    .thumb_func
log_append:                     @ r0 = word to log; clobbers r1-r3
    ldr r1, =LOG
    ldr r2, [r1]
    adds r2, #1
    str r2, [r1]
    lsls r3, r2, #2
    str r0, [r1, r3]
    bx lr

    .thumb_func
irq0_handler:
    push {lr}
    movs r0, #0x20
    bl log_append
    ldr r0, =ISPR0
    movs r1, #2
    str r1, [r0]                @ pend IRQ1: preempts at once
    movs r0, #0x21
    bl log_append
    ldr r0, =ICSR
    mov.w r1, #0x10000000       @ pend PendSV: lower priority, stays pending
    str r1, [r0]
    movs r0, #0x22
    bl log_append
    pop {pc}

    .thumb_func
irq1_handler:
    push {lr}
    mov r0, lr
    bl log_append               @ 0xFFFFFFF1: return to Handler mode
    mrs r0, ipsr
    bl log_append               @ 17
    pop {pc}

    .thumb_func
svc_handler:
    push {lr}
    mov r0, lr
    bl log_append               @ 0xFFFFFFFD: return to Thread mode on PSP
    mrs r0, psp
    bl log_append               @ frame base on the process stack
    pop {pc}

    .thumb_func
pendsv_handler:
    push {lr}
    movs r0, #0x30
    bl log_append
    ldr r0, [sp]                @ the EXC_RETURN pushed on entry
    bl log_append
    pop {pc}
    .ltorg
