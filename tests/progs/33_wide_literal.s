@ I10: real GNU assembly, U-/U+, instruction address2mod4, no flag updates.
.syntax unified
.cpu cortex-m4
.thumb
.text
.word 0x20020000
.word _start
before: .word 0x12345678
.global _start
.thumb_func
_start:
    movs r2, #0
    ldr.w r0, before        @ at0800000e: Align(pc+4,4)=08000010, U=0
    ldr.w r1, after
    bne fail                @ LDR must preserve Z1
    bkpt #0
fail:
    bkpt #1
.align 2
after: .word 0xabcdef00
