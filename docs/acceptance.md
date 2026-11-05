# Acceptance status

Last updated 2026-11-05. Emulator and firmware built from source on macOS. Toolchain: host `cc`, GNU ARM `arm-none-eabi-gcc` 16.2.0 and binutils, Renode 1.17.0 (build f1dd1b4af).

| Item | Scope | Status | Evidence |
| --- | --- | --- | --- |
| A NVIC | ISER/ICER/ISPR/ICPR/IABR/IPR; IRQ n enters as exception 16+n; enable and PRIMASK gating | Passes emulator tests; Renode comparison pending | Programs 19, 20; `tests/test_extended.c` |
| B GPIOB | GPIOB reset values, AFRH, PB8/PB9 AF4, I2C1EN | Passes emulator tests | Program 21; `tests/test_extended.c` |
| C I2C1 | Polling master; 1-, 2- and N-byte receive; repeated START; NACK sets AF | Passes emulator tests; Renode BME280 run fails (see below) | Program 22; `tests/test_extended.c`; firmware `make test-host` |
| D BME280 model | Chip ID, calibration, physical value to raw value | Passes emulator tests | `tests/test_extended.c` (25.00 °C → 2500) |
| D2 Firmware | Read temperature over I2C1, print on USART2 | Emulator: pass. Renode: 3 of 4 Robot tests pass | See "Firmware runs" |
| New instructions | ADD reg T2; ANDS/EORS/NEGS/MVNS; LSLS/LSRS imm; UXTB; modified-immediate ADD/SUB/RSB/MVN/CMP/CMN; B\<cond\> T3; LSR.W reg; MUL; UDIV; LDR literal T2 | Passes emulator tests; Renode comparison pending | Programs 24–33; `tests/test_instructions.c` |
| E VCD | Deterministic GPIO/USART/I2C output | Byte-identical across runs; viewer check pending | `tests/test_cli.py`; `tools/check_vcd.py` |
| F Renode diff | Per-step register traces, strict compare, reset-LR allowlist only | Tools in place; full matrix not run | `tools/compare_traces.py`, `tools/run_trace_matrix.py` |
| G GDB stub | Optional | Deferred | — |

## Test suite

`make clean && make test`, all programs assembled with GNU as:
- `test_instructions`: 9 groups covering the new instructions, plus programs 24–33.
- `test_runner`: 20 of 20 checks.
- `test_extended`: NVIC, I2C/GPIOB, BME280 and PRIMASK groups, plus programs 19–23.
- Python tests for the trace and CLI tools.

## Firmware runs (cortex-m-emu)

Conditions: `-O0`, RVR = 15999, 500 ticks per half-period, one SysTick tick per executed instruction. Counts are instructions, not time.

| Input | UART output | PA5 transitions (step) | Intervals |
| --- | --- | --- | --- |
| `--temperature-centi 2500` | `sensor-node boot`, then `temperature: 25.00` after every second half-period | 5528, 8005556, 16007566, 24005554, 32007564 | 8,000,028; 8,002,010; 7,997,988; 8,002,010 |
| `--temperature-centi 3050` | `temperature: 30.50` | 5627, 8005655, 16007764, 24005648, 32007765 | 8,000,028; 8,002,109; 7,997,884; 8,002,117 |

The first interval includes startup. After that, intervals alternate because the sensor read and UART print run after the LED turns off, before the next set. Absolute deadlines keep each pair near 16,000,000, so the error does not accumulate.

## Renode (firmware repo, `make test-all`)

- Pass: Vector Table, User LED Blinks, UART Prints Boot Banner.
- Fail: BME280 Reading Reaches UART. The firmware prints `temperature: 0.00` because the calibration values it reads are all zero.
- I2C access log: in the calibration read (register 0x88), Renode's BME280 drops the register-address byte ("write without specifying address"). It then returns registers 0xD1–0xD6, continuing from the previous chip-ID read. Root cause is under investigation; no change to the firmware or Robot test has been made.

## Not yet done

- Renode single-step comparison for programs 19–33, and a firmware-prefix comparison.
- Opening the VCD output in PulseView or GTKWave.
- Hardware runs. Nothing here has been verified on a real board.
