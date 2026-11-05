@ I11: 32-bit load/store immediate forms the sensor firmware needs:
@ imm8 with negative offset, pre-index and post-index writeback (T3/T4),
@ and imm12 STRB.W/LDRB.W/STRH.W/LDRH.W (T2). No flag changes.
.include "vectors.inc"
_start:
    ldr r0, base            @ r0 = 0x20000100
    ldr r1, pattern         @ r1 = 0x8899aabb
    movs r2, #0             @ Z1 for the flag check at the end
    strb r1, [r0, #-4]      @ T3, P1 U0 W0: byte at 0x200000fc
    ldrb r2, [r0, #-4]      @ r2 = 0xbb
    strh r1, [r0, #-8]!     @ T3 pre-index: halfword at 0x200000f8, r0 = 0x200000f8
    ldrh r3, [r0], #8       @ T3 post-index: r3 = 0xaabb, r0 = 0x20000100
    ldr r4, [r0, #-4]!      @ T4 pre-index: r4 = 0x000000bb, r0 = 0x200000fc
    str r1, [r0], #4        @ T4 post-index: word at 0x200000fc, r0 = 0x20000100
    ldr r5, [r0, #-4]       @ r5 = 0x8899aabb
    strb.w r1, [r0, #0x101] @ T2 imm12
    ldrb.w r6, [r0, #0x101] @ r6 = 0xbb
    strh.w r1, [r0, #0x102] @ T2 imm12
    ldrh.w r7, [r0, #0x102] @ r7 = 0xaabb
    bne fail                @ loads and stores keep Z1
    bkpt #0
fail:
    bkpt #1
.align 2
base: .word 0x20000100
pattern: .word 0x8899aabb
