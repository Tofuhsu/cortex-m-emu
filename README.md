# cortex-m-emu

An instruction-set emulator for the Arm Cortex-M4 (ARMv7-M, Thumb/Thumb-2), written in C11.
Goal: run my own bare-metal STM32F446 firmware (a separate project, not yet public) without hardware, with peripheral models for RCC, GPIO, USART and I2C (BME280).

**Status:** The original 20 checks across 18 assembly programs pass. This update implements external NVIC pending/enable state, GPIOB/AFRH, polling I2C1, a configurable BME280 device, transaction VCD output and register-trace comparison tools. New host tests pass; the new firmware's ARM build, emitted-instruction support and Renode acceptance remain pending. See [the acceptance checkpoint](docs/acceptance.md); this is not a claim that the remaining plan is finished.

Historical measurements are retained: the first busy-wait blink (`-O0`, 500,000 iterations) toggled PA5 every 3,500,022–3,500,024 instructions. The later UART/SysTick blink (RVR=15999, 500 ticks) had steady intervals of 7,999,995–8,000,004 instructions. These measurements precede the new BME280 firmware and do not establish its intervals. Counts are instructions, not time.

## Build and test

```bash
make         # build/cortex-m-emu
make test    # assembles tests/progs/*.s and checks registers/flags after each program
./build/cortex-m-emu -t build/progs/07_calls.bin
make test-host # host models/comparator only; no GNU ARM rebuild implied
./build/cortex-m-emu --vcd out.vcd build/progs/22_i2c_chip_id.bin
./build/cortex-m-emu --trace-regs regs.csv build/progs/03_adds_subs.bin
# BME280 input in hundredths Celsius; default 2500:
./build/cortex-m-emu --temperature-centi 3050 firmware.bin
```

Needs a host C compiler and `arm-none-eabi-binutils` (for the test programs).

## How the tests are checked

Original tests retain their existing Renode 1.17 cross-check notes. Where Renode and manuals disagree, the tests follow the manuals (reset LR, GPIO clock gating and BSRR set priority). SysTick uses instruction ticks rather than Renode virtual time; CSR.CLKSOURCE behavior also differs.

Programs 19–23 and `tests/test_extended.c` add NVIC, GPIOB, I2C and PRIMASK checks. Locally, those program fixtures were encoded with a limited encoder because GNU ARM tools were unavailable; a clean GNU rebuild and fresh Renode comparison are pending. Host firmware C tests are separate from ARM instruction execution. See [NVIC decisions](docs/nvic.md), [week 4](docs/week4.md) and [week 5](docs/week5.md) for exact scope and validation limits.

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

- Only a subset of Thumb/Thumb-2 instructions is implemented. The current peripheral models are RCC, GPIOA/B, SysTick, NVIC, USART2 TX, polling I2C1 and BME280. GPIO models enforce clock gating and configuration, not electrical levels.
- SysTick receives one tick after each executed instruction, including instructions in handlers. Exception entry and return do not add simulated clock cycles. Instruction counts cannot be converted to elapsed time or frequency.
- `CLKSOURCE` is stored and readable, but does not change the timer rate in this model. On STM32F446 hardware, `CLKSOURCE=0` selects AHB/8 and `CLKSOURCE=1` selects the processor clock (AHB), as documented in [PM0214](https://www.st.com/resource/en/programming_manual/pm0214-stm32-cortexm4-mcus-and-mpus-programming-manual-stmicroelectronics.pdf), Rev 10, section 4.5.1, page 247. AHB/8 division is not implemented.
- With a fixed nonzero reload value, the SysTick counter period is `RVR+1` instruction ticks.
- Exception handling currently supports the basic MSP frame and return to Thread mode using `EXC_RETURN=0xFFFFFFF9`. Nested exceptions, priority arbitration, PSP and floating-point frames are not modeled. Asynchronous SysTick requests wait while a handler is active and coalesce into one pending request.
- PRIMASK blocks asynchronous SysTick and external IRQ dispatch without stopping the timer or clearing pending state. It is not part of xPSR or the exception frame. The model assumes privileged execution; `MRS/MSR PRIMASK` and CPSID/CPSIE are implemented; CONTROL.nPRIV, BASEPRI and FAULTMASK are not. An SVC executed with PRIMASK set halts with an explicit HardFault-escalation message because HardFault handling is not implemented.
- USART2 TX completes immediately: no baud timing, RX, IRQ or DMA. BRR is stored but unused. Bytes reach the output only when PA2 is set to AF7; CR1 writes with unsupported bits (for example RE) are rejected.

- External IRQs use enabled-and-pending gating. Arbitration currently chooses the lowest exception number; IPR stores four priority bits but does not affect selection. No nesting, grouping, STIR, tail-chaining or interrupt latency. The 240-IRQ capacity is architectural, not the F446 vector map.
- I2C1 supports polling seven-bit master transactions, 1/2/N byte receive endings and repeated START. No slave/IRQ/DMA/PEC or electrical/clock timing. CR2.FREQ/CCR/TRISE are stored only. Invalid configuration and gated transfers have diagnostic counters.
- BME280 uses declared synthetic calibration and nearest representable raw inputs. Conversion is instantaneous; oversampling/filter/standby timing and sensor noise are absent. The firmware currently targets temperature output; pressure/humidity compensation is tested in the host model.
- VCD's numeric timestamps are instruction steps despite its required `1 ns` declaration. UART/I2C are vectors, not synthesized serial waveforms. Viewer loading/screenshot are pending.
- Register comparison is strict by default. Reset-LR differences need an exact explicit allowlist; BKPT and asynchronous timer boundaries need documented prefixes. No full Renode matrix has been run for this update.
- Optional GDB RSP is deferred.

## AI assistance for this update

The changes described in the acceptance checkpoint—including CPU/NVIC extensions, peripheral models, CLI/tools, added tests and documentation—were prepared with AI assistance and are awaiting the author's target validation/review. Earlier contributions remain recorded in the existing history.
