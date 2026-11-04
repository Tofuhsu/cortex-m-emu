@ Shared header: minimal vector table, then _start. Each test ends with BKPT.
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text
    .word 0x20020000            @ initial SP
    .word _start                @ reset vector (linker sets bit 0: Thumb)
    .thumb_func
    .global _start
