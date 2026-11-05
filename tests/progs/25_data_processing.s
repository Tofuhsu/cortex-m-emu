@ I2: exact ANDS/EORS/RSBS/MVNS examples and NEG zero/INT_MIN flags.
.include "vectors.inc"
_start:
    movs r2, #0xf0
    movs r3, #0xff
    ands r3, r2              @ 4013 => f0
    eors r3, r2              @ 4053 => 0
    movs r3, #0
    rsbs r3, r3, #0          @ 425b => 0, C=1 Z=1
    bcc fail
    bne fail
    ldr r3, minimum
    rsbs r3, r3, #0          @ INT_MIN: N=1 Z=0 C=0 V=1
    bvc fail
    bcs fail
    bpl fail
    movs r2, #0
    mvns r2, r2              @ ffffffff, preserve C=0 V=1
    bvc fail
    bcs fail
    bkpt #0
fail:
    bkpt #1
.align 2
minimum: .word 0x80000000
