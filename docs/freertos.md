# FreeRTOS support (branch `freertos-support`, 2026-11-23)

This branch adds what a FreeRTOS firmware ([stm32-freertos-logger](../../stm32-freertos-logger)) needs to run unchanged on the emulator. Below are the decisions behind each model and what each one leaves out. The models are instruction-accurate at most, so nothing here describes time on a real chip.

Section and table numbers of RM0390, PM0214, DDI0403E and the STM32F446/BME280 datasheets that appear in the comments of the files added on this branch were written from memory of those documents, without opening the PDFs. Treat them as pointers to check. The page-checked references are the ones listed in [manual_checks.md](manual_checks.md).

## CPU core

The decoder was rewritten around the A5 encoding tables of DDI0403E: every 16-bit and 32-bit integer Thumb encoding of ARMv7-M is decoded, including IT blocks, CBZ/CBNZ, TBB/TBH, LDM/STM, LDRD/STRD, LDREX/STREX (word, byte, halfword), all data-processing forms with shifts and modified immediates, bitfield and saturation instructions, extends with rotation, REV/RBIT/CLZ, MUL/MLA/MLS, SMULxy/SMLAxy, SMMUL/SMMLA, long multiplies and SDIV/UDIV.

Not modeled, and the CPU halts with a message when it meets them: coprocessor and floating-point instructions (no FPU), SIMD/DSP parallel add/subtract, SEL, the 16-bit dual extends (SXTB16 and similar), SSAT16/USAT16, QADD-family saturating arithmetic, PKHBT/PKHTB, LDR literal to PC (T2). GE flags are not kept.

IT blocks: the IT state is part of xPSR, is stacked and restored on exceptions, and a skipped instruction still counts as one step. Inside an IT block, 16-bit arithmetic does not set flags, as the architecture says. BKPT runs even when its condition fails.

## Exception model

- Priorities: SHPR bytes for the configurable system exceptions, NVIC IPR bytes for IRQs, four implemented bits each (as on STM32F4), AIRCR.PRIGROUP for group/subpriority. Reset, NMI and HardFault have fixed priorities -3, -2 and -1.
- Execution priority is the highest active group priority, raised by BASEPRI, PRIMASK (to 0) and FAULTMASK (to -1). A pending exception is taken when its group priority is strictly higher; ties go to the lower exception number.
- Nesting and active bits: any number of exceptions can be active. EXC_RETURN 0xFFFFFFF1 (Handler, MSP), 0xFFFFFFF9 (Thread, MSP) and 0xFFFFFFFD (Thread, PSP) are accepted. Returning to Thread mode with another exception still active, or an FP-frame EXC_RETURN, halts with a message (the architecture raises UsageFault).
- MSP/PSP: CONTROL.SPSEL picks the Thread-mode stack; exception entry pushes to the current stack and switches to MSP. MRS/MSR cover APSR/IPSR/EPSR forms, MSP, PSP, PRIMASK, BASEPRI, BASEPRI_MAX, FAULTMASK and CONTROL. CONTROL.nPRIV is stored and readable but not enforced: the model always runs privileged.
- SVC escalates to HardFault (HFSR.FORCED) when it cannot preempt, for example with PRIMASK set. At priority -1 that would be a lockup; the model halts instead.
- Tail-chaining and late arrival are not separate paths: an exception return followed by entry of the next pending exception gives the same register and memory state as tail-chaining, but the stack frame is written twice. No timing is modeled for either.
- WFI and WFE put the core to sleep. A sleeping core wakes when an exception could preempt if PRIMASK were clear. Each step while asleep runs no instruction but still counts as a step, so SysTick, DMA and IWDG keep running; `cpu.sleep_steps` counts them. SLEEPONEXIT and SLEEPDEEP are stored only.
- The local exclusive monitor is cleared on exception entry and return.

Faults other than the SVC escalation are not raised: undefined instructions, bus faults and invalid returns stop the emulator with a message, so a test run fails at the faulting instruction.

## Peripherals

| Model | Behaviour | Left out |
| --- | --- | --- |
| SCB | CPUID (Cortex-M4 r0p1), ICSR (PendSV/SysTick/NMI set and clear, VECTACTIVE, VECTPENDING, RETTOBASE, ISRPENDING), VTOR, AIRCR with VECTKEY and SYSRESETREQ, SHPR1-3, SHCSR active bits, HFSR, CCR (STKALIGN reads 1), CPACR stored | CFSR always 0; CCR traps (DIV_0_TRP, UNALIGN_TRP) stored, not applied |
| RCC | AHB1/APB1/APB2 enables; CSR with LSION/LSIRDY and the reset flags; a system reset keeps flags and adds IWDGRSTF or SFTRSTF | Clock tree, PLL, oscillator start-up times |
| GPIOC | Same model as GPIOA/B; inputs are injected (B1 on PC13 is high while released) | Electrical levels, pull resistors |
| SYSCFG/EXTI | EXTICR1-4 port selection, IMR, RTSR/FTSR edges, SWIER, PR (write 1 to clear); IRQs 6-10, 23, 40 | Event (EMR) routing to WFE, lines 16-22 |
| SPI1 | Master, 8-bit frames, SSM=1/SSI=1, frame completes inside the DR write, OVR, MODF, RXNE/TXE interrupts and DMA requests. Pins PB3/PB4/PB5 must be AF5 | BR (baud) is stored only; BSY never set; slave mode, 16-bit frames, CRC, hardware NSS, TI mode |
| BME280 on SPI | SPI front end of the same BME280 model: first byte is the register with bit 7 = read, reads auto-increment, writes are control/data pairs, CS high ends a transaction | The SPI mode bits (CPOL/CPHA) are not checked |
| DMA1/DMA2 | Eight streams each, direct mode, P2M/M2P/M2M (M2M on DMA2 only), normal and circular, CHSEL matching, TC/HT flags and interrupts, IFCR, write protection while EN=1. One item per enabled stream per step, highest PL first | FIFO mode, bursts, double buffer, PFCTRL, transfer errors (an access to an unmapped address halts the emulator like a CPU bus fault) |
| USART2 | CR3.DMAT/DMAR DMA requests; DR also accepts byte and halfword accesses | Unchanged otherwise (immediate TX) |
| ADC1 | Software-started single conversion of SQR3's first channel; temperature sensor (channel 18) and VREFINT (channel 17) behind TSVREFE; EOC, EOCIE (IRQ 18), resolution and alignment | Scan, continuous, injected, DMA, external triggers; external pins read 0; no noise or offset |
| Calibration words | VREFINT_CAL 1502, TS_CAL1 959, TS_CAL2 1207 at 0x1FFF7A2A/2C/2E, from datasheet typical values (see `src/periph/adc.h`) | Real chips differ; these are not from a part |
| IWDG | KR keys, PR/RLR write access, start, refresh; on reaching zero the runner resets the machine | One LSI period = `--iwdg-steps-per-lsi` steps (default 500, as if the core ran at 16 MHz and LSI were exactly 32 kHz). The real LSI is 17-47 kHz, so a timeout in steps is not a time |
| Flash interface | KEYR unlock sequence, LOCK, PG programming with PSIZE (PGPERR on a width mismatch, PGSERR on a write without PG), sector and mass erase, EOP with EOPIE. Programming ANDs into the old content | Operations finish inside the access (BSY never seen); no program/erase time, no CPU stall, no option bytes or write protection, no read-while-write effects |

## Runner

`main.c` builds the whole machine, routes every interrupt request to its NVIC line (RM0390 vector table positions), ticks SysTick, DMA1, DMA2 and IWDG once per step, and resets the machine (CPU, peripherals, RCC flags; SRAM and flash kept; BME280/SSD1306 not reset) on IWDG expiry or SYSRESETREQ. Flash after the loaded image reads as erased (0xFF). `--flash-out` saves the whole flash, so a second run with that file models a power cycle that keeps the log. `--uart-rx-at` and `--button-press` schedule host input by step number.

## Tests

- Programs 36 (IT blocks) and 37 (PSP, SVC, BASEPRI, IRQ priorities, a nested IRQ, PendSV after return) are assembled with GNU as and checked in `tests/test_rtos.c`, together with host checks of every model above, SVC escalation and WFI.
- Earlier tests that expected encodings such as SXTH, ANDS.W, ORN, ADDW, LSL.W (register), MLA and SDIV to halt now check their results instead.
- The sensor-node firmware still passes its emulator check (`tests/emu/run.sh` in that repo), also when built with `-O2` and `-Os`.
- The FreeRTOS logger's integration tests run in that repo (`make test-emu`).

No Renode comparison was run for the new instructions or models on this branch.
