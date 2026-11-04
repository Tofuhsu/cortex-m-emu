    .include "vectors.inc"
_start:
    ldr r0, =0x40023830         @ PC-relative load from the literal pool
    ldr r1, =0x7FFFFFFF
    adds r1, #1                 @ signed overflow: V=1, N=1
    bkpt #0
    .ltorg
