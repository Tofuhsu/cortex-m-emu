@ IT blocks: then/else conditions, skipped instructions, 16-bit arithmetic that
@ does not set flags inside the block and a 32-bit load inside a block.
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text
    .word 0x20020000            @ 0: initial SP
    .word _start                @ 1: Reset

    .thumb_func
    .global _start
_start:
    movs r0, #5
    cmp r0, #5                  @ Z=1 C=1
    ite eq
    moveq r1, #1                @ executes
    movne r1, #2                @ skipped
    itt ne
    movne r2, #9                @ skipped
    addne r1, r1, #100          @ skipped
    movs r2, #0                 @ Z=1
    itte eq
    addeq r3, r2, #7            @ executes; ADD inside IT leaves Z=1
    addeq r3, r3, #1            @ executes -> r3 = 8
    subne r3, r3, #50           @ skipped
    ldr r5, =0x20000000
    movw r6, #0xBEEF
    str r6, [r5]
    cmp r0, #10                 @ 5 - 10: N=1 Z=0 C=0 V=0
    itet lt
    ldrlt.w r7, [r5]            @ executes: r7 = 0xBEEF
    movge r7, #0                @ skipped
    lsllt r4, r0, #2            @ executes: r4 = 20, flags unchanged
    itt hs
    addhs.w r4, r4, #1          @ skipped (C=0)
    addhs r4, #2                @ skipped
    bkpt #0
    .ltorg
