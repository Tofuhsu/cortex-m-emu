# cortex-m-emu

An instruction-set emulator for the Arm Cortex-M4 (ARMv7-M, Thumb/Thumb-2), written in C11.
Goal: run my own bare-metal STM32F446 firmware ([stm32-baremetal-sensor-node](../stm32-baremetal-sensor-node)) without hardware, with peripheral models for RCC, GPIO, USART and I2C (BME280).

**Status:** week 1. Memory bus, loader and test suite exist; the CPU core is being written.

## Build and test

```bash
make         # build/cortex-m-emu
make test    # assembles tests/progs/*.s and checks registers/flags after each program
./build/cortex-m-emu -t build/progs/07_calls.bin
```

Needs a host C compiler and `arm-none-eabi-binutils` (for the test programs).

## How the tests are checked

Each program in `tests/progs/` ends with `BKPT`. The expected registers, flags and memory in `tests/test_main.c` were cross-checked by single-stepping the same binaries in [Renode](https://renode.io) 1.17 (2026-11-03/04). Where Renode and the Arm or ST manuals disagree, the tests follow the manuals: LR after reset (ARMv7-M ARM), and GPIO clock gating and BSRR set priority (RM0390). Each case is explained in the test file.

## Plan

| Week | Scope |
| --- | --- |
| 1 | Reset sequence, fetch/decode/execute loop, 16-bit Thumb instructions, flags |
| 2 | 32-bit Thumb-2 (BL, modified immediates, LDR.W/STR.W); RCC + GPIO models; run the blink firmware |
| 3 | Exceptions (stacking, EXC_RETURN), NVIC, SysTick; USART output to the terminal |
| 4 | I2C model + BME280 sensor model; VCD waveform output for PulseView |
| 5 | Differential testing against Renode (instruction-by-instruction register traces); optional GDB stub |

## Limits

Instruction-accurate, not cycle-accurate: no pipeline, wait states or bus timing. Results say nothing about real interrupt latency, power or electrical behavior. Only the instructions and peripherals listed above are modeled.

## AI assistance

Written with an AI assistant: Makefile, CI, `src/bus.c`, `src/loader.c`, `src/main.c`, the test programs and test runner, and the docs.
Written by the author: the CPU core (`src/cpu.c`) and peripheral models. This split is kept accurate as the project grows.
