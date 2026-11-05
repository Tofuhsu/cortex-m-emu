@ I4: uxtb must not change NZCV, including when result is zero.
.include "vectors.inc"
_start:
    ldr r3, byte_source
    movs r0, #0
    cmp r0, #0              @ Z=1 C=1
    uxtb r3, r3             @ b2db =>80, but Z must remain1
    bne fail
    bcc fail
    ldr r2, zero_source
    uxtb r2, r2             @ 0, flags remain0110
    bne fail
    bcc fail
    bkpt #0
fail:
    bkpt #1
.align 2
byte_source: .word 0xabcd1280
zero_source: .word 0x12345600
