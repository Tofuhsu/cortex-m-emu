# Acceptance status

Last updated 2026-11-05. Emulator and firmware built from source on macOS. Toolchain: host `cc`, GNU ARM `arm-none-eabi-gcc` 16.2.0 and binutils, Renode 1.17.0 (build f1dd1b4af), Surfer 0.7.0.

| Item | Scope | Status | Evidence |
| --- | --- | --- | --- |
| A NVIC | ISER/ICER/ISPR/ICPR/IABR/IPR; IRQ n enters as exception 16+n; enable and PRIMASK gating | Passes emulator tests; 19 and 20 match Renode (19 up to IRQ entry) | Programs 19, 20; `tests/test_extended.c` |
| B GPIOB | GPIOB reset values, AFRH, PB8/PB9 AF4, I2C1EN | Passes emulator tests; Renode ignores the AFRH byte store in program 21 | Program 21; `tests/test_extended.c` |
| C I2C1 | Polling master; 1-, 2- and N-byte receive; repeated START; NACK sets AF | Passes emulator tests; program 22 differs from Renode in SR1 flag timing; Renode BME280 run fails (see below) | Program 22; `tests/test_extended.c`; firmware `make test-host` |
| D BME280 model | Chip ID, calibration, physical value to raw value | Passes emulator tests | `tests/test_extended.c` (25.00 °C → 2500) |
| D2 Firmware | Read temperature over I2C1, print on USART2 | Emulator: pass. Renode: 3 of 4 Robot tests pass; cause found in Renode's I2C model | See "Firmware runs" |
| New instructions | ADD reg T2; ANDS/EORS/NEGS/MVNS; LSLS/LSRS imm; UXTB; modified-immediate ADD/SUB/RSB/MVN/CMP/CMN; B\<cond\> T3; LSR.W reg; MUL; UDIV; LDR literal T2 | Passes emulator tests; all match Renode instruction by instruction | Programs 24–33; `tests/test_instructions.c` |
| E VCD | Deterministic GPIO/USART/I2C output | Byte-identical across runs; opened in Surfer 0.7.0 | `tests/test_cli.py`; `tools/check_vcd.py`; `docs/img/` |
| F Renode diff | Per-step register traces, strict compare, reset-LR allowlist only | Run: 30 of 33 programs match (01 at reset only; 13, 15, 19 over documented prefixes); 11, 21, 22 differ for documented reasons; NEGS mutation caught at step 6 | [renode_differences.md](renode_differences.md) |
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
| `--temperature-centi 2500` | `sensor-node boot`, then `temperature: 25.00` after every second half-period | 5597, 8005625, 16007635, 24005623, 32007633 | 8,000,028; 8,002,010; 7,997,988; 8,002,010 |
| `--temperature-centi 3050` | `temperature: 30.50` | 5696, 8005724, 16007833, 24005717, 32007834 | 8,000,028; 8,002,109; 7,997,884; 8,002,117 |

The first interval includes startup. After that, intervals alternate because the sensor read and UART print run after the LED turns off, before the next set. Absolute deadlines keep each pair near 16,000,000, so the error does not accumulate.

## Renode (firmware repo, `make test-all`)

- Pass: Vector Table, User LED Blinks, UART Prints Boot Banner.
- Fail: BME280 Reading Reaches UART. The firmware prints `bme280 init error`: it rejects the all-zero calibration it reads.
- Cause: Renode's `STM32F1_I2C` model does not finish the one-byte chip-ID read, so its BME280 model drops the next register address and returns registers 0xD1–0xD6. Source lines and log: [renode_differences.md](renode_differences.md). The firmware keeps the RM0390 single-byte sequence; the test is not changed to pass.

## VCD

Two runs of each command give byte-identical files. The VCD ends with a timestamp one step after the last executed instruction, so viewers draw the final change. The time axis is instruction steps, not nanoseconds.

- `--vcd blink.vcd -n 20000000 firmware.bin`: PA5 falls near step 8,000,000 and rises near 16,000,000; the UART and I2C bursts mark the sensor read. [Screenshot](img/vcd_blink.png).
- `--vcd i2c.vcd build/progs/22_i2c_chip_id.bin`: address EC, register D0, repeated START, address ED, then 0x60 with NACK. [Screenshot](img/vcd_i2c_22.png).

## CI check

`make && make test` passes in an Ubuntu 24.04 container (GCC 13.3, binutils 2.42), the same packages as the GitHub workflow. All 33 programs assemble to the same bytes as with binutils from GNU ARM 16.2.0.

## Not yet done

- Whether TxE is set together with ADDR at EV6 (program 22 difference): check against RM0390.
- Hardware runs. Nothing here has been verified on a real board.
