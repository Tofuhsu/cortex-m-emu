@ RM0390 24.3.3, Bosch 6.2: pointer 0xd0, repeated START, one-byte NACK/STOP.
@ Test expects r7=0x60. Timing is not compared to Renode; poll flags instead.
.include "vectors.inc"
_start:
    ldr r0, rcc
    movs r1, #2
    str r1, [r0, #0x30]
    ldr r1, i2c1en
    str r1, [r0, #0x40]
    ldr r0, gpiob
    ldr r1, moder
    str r1, [r0]
    ldr r1, otype
    str r1, [r0, #4]
    movs r1, #0x44
    str r1, [r0, #0x24]
    ldr r0, i2c1
    movs r1, #16
    str r1, [r0, #4]
    movs r1, #80
    str r1, [r0, #0x1c]
    movs r1, #17
    str r1, [r0, #0x20]
    ldr r1, start_pe
    str r1, [r0]
wait_sb_write:
    ldr r1, [r0, #0x14]
    and.w r1, r1, #1
    cmp r1, #0
    beq wait_sb_write
    movs r1, #0xec
    str r1, [r0, #0x10]
wait_addr_write:
    ldr r1, [r0, #0x14]
    and.w r1, r1, #2
    cmp r1, #0
    beq wait_addr_write
    ldr r1, [r0, #0x18]
    movs r1, #0xd0
    str r1, [r0, #0x10]
wait_btf:
    ldr r1, [r0, #0x14]
    and.w r1, r1, #4
    cmp r1, #0
    beq wait_btf
    ldr r1, start_pe
    str r1, [r0]
wait_sb_read:
    ldr r1, [r0, #0x14]
    and.w r1, r1, #1
    cmp r1, #0
    beq wait_sb_read
    movs r1, #0xed
    str r1, [r0, #0x10]
wait_addr_read:
    ldr r1, [r0, #0x14]
    and.w r1, r1, #2
    cmp r1, #0
    beq wait_addr_read
    @ ACK was zero before ADDR clearing. Keep the ADDR/STOP critical window atomic.
    cpsid i
    ldr r1, [r0, #0x18]
    ldr r1, stop_pe
    str r1, [r0]
    cpsie i
wait_rxne:
    ldr r1, [r0, #0x14]
    and.w r1, r1, #64
    cmp r1, #0
    beq wait_rxne
    ldr r7, [r0, #0x10]
    bkpt #0
.align 2
rcc: .word 0x40023800
gpiob: .word 0x40020400
moder: .word 0x000a0280
otype: .word 0x00000300
i2c1en: .word 0x00200000
i2c1: .word 0x40005400
start_pe: .word 0x101
stop_pe: .word 0x201
