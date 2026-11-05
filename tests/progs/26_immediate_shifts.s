@ I3: LSL0 preserves C, LSR encoded zero means 32, carry-out one, zero result.
.include "vectors.inc"
_start:
    movs r0, #0
    cmp r0, #0              @ C=1
    movs r3, #1
    lsls r3, r3, #0
    bcc fail
    ldr r2, msb
    lsrs r2, r2, #32        @ zero immediate encoding, result0 C=1 Z=1
    bcc fail
    bne fail
    ldr r3, shift_source
    lsls r3, r3, #8         @ exact 021b =>80000100, C=1
    bcc fail
    ldr r2, shift_right
    lsrs r2, r2, #4         @ exact 0912 =>08000000, C=1
    bcc fail
    bkpt #0
fail:
    bkpt #1
.align 2
msb: .word 0x80000000
shift_source: .word 0x01800001
shift_right: .word 0x80000008
