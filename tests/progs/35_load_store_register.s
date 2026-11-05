@ I12: 16-bit load/store (register) T1, address = Rn + Rm: STR, STRH, STRB,
@ LDR, LDRH, LDRSH, LDRB, LDRSB. Sign extension of 0xaabb and 0xbb.
.include "vectors.inc"
_start:
    ldr r0, base            @ r0 = 0x20000200
    ldr r2, pattern         @ r2 = 0x8899aabb
    movs r1, #4
    str r2, [r0, r1]        @ word at 0x20000204
    movs r1, #8
    strh r2, [r0, r1]       @ halfword at 0x20000208
    movs r1, #12
    strb r2, [r0, r1]       @ byte at 0x2000020c
    movs r1, #4
    ldr r3, [r0, r1]        @ r3 = 0x8899aabb
    movs r1, #8
    ldrh r4, [r0, r1]       @ r4 = 0x0000aabb
    ldrsh r5, [r0, r1]      @ r5 = 0xffffaabb
    movs r1, #12
    ldrb r6, [r0, r1]       @ r6 = 0x000000bb
    ldrsb r7, [r0, r1]      @ r7 = 0xffffffbb
    bkpt #0
.align 2
base: .word 0x20000200
pattern: .word 0x8899aabb
