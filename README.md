# cortex-m-emu

An instruction-set emulator for the Arm Cortex-M4 (ARMv7-M, Thumb/Thumb-2), written in C11.
Goal: run my own bare-metal STM32F446 firmware (a separate project, not yet public) without hardware, with peripheral models for RCC, GPIO, USART and I2C (BME280).

**Status:** Week 3 of 5. The CPU core passes all 17 test programs and runs my STM32F446 blink firmware (`-O0` build) with RCC and GPIOA models. PA5 toggles every 3,500,022–3,500,024 instructions, matching the count from the disassembly (7-instruction delay loop × 500,000 iterations plus call overhead). Counts are instructions, not time.

The Week 3 implementation adds IPSR, MRS (IPSR and PRIMASK), SVC exception entry, exception return, SysTick with pending exceptions, CPSID/CPSIE for PRIMASK, and a USART2 TX model. The firmware now blinks from SysTick: PA5 toggles every 7,999,995–8,000,004 instructions with RVR = 15999 and 500 ticks. The boot banner over USART2 is the next milestone.

## Build and test

```bash
make         # build/cortex-m-emu
make test    # assembles tests/progs/*.s and checks registers/flags after each program
./build/cortex-m-emu -t build/progs/07_calls.bin
```

Needs a host C compiler and `arm-none-eabi-binutils` (for the test programs).

## How the tests are checked

Each program in `tests/progs/` ends with `BKPT`. The expected registers, flags and memory in `tests/test_main.c` were cross-checked by single-stepping the same binaries in [Renode](https://renode.io) 1.17 (2026-11-03/04). Where Renode and the Arm or ST manuals disagree, the tests follow the manuals: LR after reset (ARMv7-M ARM), and GPIO clock gating and BSRR set priority (RM0390). Each case is explained in the test file.

For SysTick, Renode is a reference for handler entry, the stacked frame and exception return. Exact trigger steps follow this emulator's instruction-based timer rules; they are not expected to match Renode's virtual-time scheduling. Renode also reads CSR.CLKSOURCE as 1 after a write of 0; see `tests/test_main.c`.

## Plan

| Week | Scope |
| --- | --- |
| 1 | Reset sequence, fetch/decode/execute loop, 16-bit Thumb instructions, flags |
| 2 | 32-bit Thumb-2 (BL, modified immediates, LDR.W/STR.W); RCC + GPIO models; run the blink firmware |
| 3 | Exceptions (stacking, EXC_RETURN), NVIC, SysTick; USART output to the terminal |
| 4 | I2C model + BME280 sensor model; VCD waveform output for PulseView |
| 5 | Differential testing against Renode (instruction-by-instruction register traces); optional GDB stub |

## Limits

Instruction-accurate, not cycle-accurate: no pipeline, wait states or bus timing. Results say nothing about real interrupt latency, power or electrical behavior.

- Only a subset of Thumb/Thumb-2 instructions is implemented. The current peripheral models are RCC, GPIOA, SysTick and USART2 (TX only); I2C/BME280 remains planned work.
- SysTick receives one tick after each executed instruction, including instructions in handlers. Exception entry and return do not add simulated clock cycles. Instruction counts cannot be converted to elapsed time or frequency.
- `CLKSOURCE` is stored and readable, but does not change the timer rate in this model. On STM32F446 hardware, `CLKSOURCE=0` selects AHB/8 and `CLKSOURCE=1` selects the processor clock (AHB), as documented in [PM0214](https://www.st.com/resource/en/programming_manual/pm0214-stm32-cortexm4-mcus-and-mpus-programming-manual-stmicroelectronics.pdf), Rev 10, section 4.5.1, page 247. AHB/8 division is not implemented.
- With a fixed nonzero reload value, the SysTick counter period is `RVR+1` instruction ticks.
- Exception handling currently supports the basic MSP frame and return to Thread mode using `EXC_RETURN=0xFFFFFFF9`. Nested exceptions, priority arbitration, PSP and floating-point frames are not modeled. Asynchronous SysTick requests wait while a handler is active and coalesce into one pending request.
- PRIMASK blocks asynchronous SysTick dispatch without stopping the timer or clearing pending state. It is not part of xPSR or the exception frame. The model assumes privileged execution; `MSR PRIMASK`, `CONTROL.nPRIV`, BASEPRI and FAULTMASK are not implemented. An SVC executed with PRIMASK set halts with an explicit HardFault-escalation message because HardFault handling is not implemented.
- USART2 TX completes immediately: no baud timing, RX, IRQ or DMA. BRR is stored but unused. Bytes reach the output only when PA2 is set to AF7; CR1 writes with unsupported bits (for example RE) are rejected.
