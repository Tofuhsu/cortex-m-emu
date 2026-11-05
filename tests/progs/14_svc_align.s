@ SVC with SP 4 mod 8: entry aligns the frame down to 8 bytes and sets bit 9 of
@ the stacked xPSR; return reads bit 9 and adds the 4 padding bytes back.
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text
    .word 0x20020000            @ 0: initial SP
    .word _start                @ 1: Reset
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0  @ 2-10
    .word svc_handler           @ 11: SVCall
    .word 0, 0, 0, 0            @ 12-15

    .thumb_func
    .global _start
_start:
    movs r0, #0                 @ flags are UNKNOWN after reset: set Z so the stacked xPSR is defined
    sub sp, #4                  @ SP = 0x2001FFFC (4 mod 8)
    mov r5, sp                  @ r5 = SP before SVC
    svc #0
    mov r6, sp                  @ r6 = SP after return (should equal r5)
    add sp, #4
    bkpt #0

    .thumb_func
svc_handler:
    ldr r1, lit_sram
    mov r2, sp
    str r2, [r1, #0]            @ [0x20000000] = frame base
    ldr r2, [sp, #28]
    str r2, [r1, #4]            @ [0x20000004] = stacked xPSR
    bx lr
    .align 2
lit_sram: .word 0x20000000
