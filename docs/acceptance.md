# Acceptance status

Last updated 2026-11-05. Emulator and firmware built from source on macOS. Toolchain: host `cc`, GNU ARM `arm-none-eabi-gcc` 16.2.0 and binutils, Renode 1.17.0 (build f1dd1b4af), Surfer 0.7.0.

| Item | Scope | Status | Evidence |
| --- | --- | --- | --- |
| A NVIC | ISER/ICER/ISPR/ICPR/IABR/IPR; IRQ n enters as exception 16+n; enable and PRIMASK gating | Passes emulator tests; 19 and 20 match Renode (19 up to IRQ entry) | Programs 19, 20; `tests/test_extended.c` |
| B GPIOB | GPIOB reset values, AFRH, PB8/PB9 AF4, I2C1EN | Passes emulator tests; Renode ignores the AFRH byte store in program 21 | Program 21; `tests/test_extended.c` |
| C I2C1 | Polling master; 1-, 2- and N-byte receive; repeated START; NACK sets AF | Passes emulator tests; program 22 matches Renode with instant transfers (differs in BTF timing with default timing); the Renode BME280 test passes with the patched I2C1 model (see below) | Program 22; `tests/test_extended.c`; firmware `make test-host` |
| D BME280 model | Chip ID, calibration, physical value to raw value | Passes emulator tests | `tests/test_extended.c` (25.00 °C → 2500) |
| D2 Firmware | Read temperature over I2C1, print on USART2 | Emulator: pass. Renode: 4 of 4 Robot tests pass with a patched copy of Renode's I2C1 model (stock model: 3 of 4) | See "Firmware runs" |
| New instructions | ADD reg T2; ANDS/EORS/NEGS/MVNS; LSLS/LSRS imm; UXTB; modified-immediate ADD/SUB/RSB/MVN/CMP/CMN; B\<cond\> T3; LSR.W reg; MUL; UDIV; LDR literal T2; STR/LDR(B/H) imm8 with negative offset, pre/post-index (T3/T4) and imm12 (T2); 16-bit load/store register incl. LDRSB/LDRSH | Passes emulator tests; all match Renode instruction by instruction | Programs 24–35; `tests/test_instructions.c` |
| E VCD | Deterministic GPIO/USART/I2C output | Byte-identical across runs; opened in Surfer 0.7.0 | `tests/test_cli.py`; `tools/check_vcd.py`; `docs/img/` |
| F Renode diff | Per-step register traces, strict compare, reset-LR allowlist only | Run with the firmware board file (patched I2C1, instant transfers): 33 of 35 programs match (01 at reset only; 13, 15, 19 over documented prefixes); 11 and 21 differ where Renode and RM0390 disagree; blink firmware `6151eed` matches for all 16,439 steps before its first SysTick entry; the sensor firmware is identical for 4299 steps, up to its first USART2 interrupt entry, which Renode records one step earlier (as in 19); NEGS mutation caught at step 6 | [renode_differences.md](renode_differences.md) |
| G GDB stub | RSP over TCP: break, continue, stepi, registers, memory, Ctrl-C | Done. Protocol tests in `make test`; a real `arm-none-eabi-gdb` 18.1 batch session on the firmware | `tests/test_gdbstub.py`; `tools/gdb_firmware.sh`; [week5.md](week5.md) |

## Test suite

`make clean && make test`, all programs assembled with GNU as:
- `test_instructions`: 9 groups covering the new instructions, plus programs 24–33.
- `test_runner`: 20 of 20 checks.
- `test_extended`: NVIC, I2C/GPIOB, BME280 and PRIMASK groups, plus programs 19–23.
- Python tests for the trace, CLI and GDB stub tools.

## Firmware runs (cortex-m-emu)

Conditions: `-O0`, RVR = 15999, 500 ticks per half-period, one SysTick tick per executed instruction. Counts are instructions, not time.

| Input | UART output | PA5 transitions (step) | Intervals |
| --- | --- | --- | --- |
| `--temperature-centi 2500` | `sensor-node boot`, then `temperature: 25.00` after every second half-period | 156249, 8156284, 16182307, 24156291, 32182258 | 8,000,035; 8,026,023; 7,973,984; 8,025,967 |
| `--temperature-centi 3050` | `temperature: 30.50` | 156365, 8156400, 16182539, 24156411, 32182494 | 8,000,035; 8,026,139; 7,973,872; 8,026,083 |

The first PA5 set comes after startup, which now includes the SSD1306 init and a full 1 KiB frame (about 150,000 instructions over I2C). After that, intervals alternate because the sensor read, UART print and one-page display update run after the LED turns off, before the next set. Absolute deadlines keep each pair near 16,000,000, so the error does not accumulate.

The firmware before UART interrupts and the display (commit `b133d9d`) gave 5597, 8005625, 16007635, 24005623, 32007633 for 2500 (intervals 8,000,028; 8,002,010; 7,997,988; 8,002,010).

`make test-emu` in the firmware repo runs the build with `--uart-rx 'hello\rtemp\r'` and checks: `echo: hello`, a second `temperature: 25.00` from the `temp` command, no `ssd1306 not found`, and a byte-identical frame dump against `tests/emu/oled_25.00.pbm` (`SENSOR NODE` on page 0, `T 25.00 C` on page 2).

## Renode (firmware repo, `make test-all`)

- All seven tests pass: Vector Table, User LED Blinks, UART Prints Boot Banner, BME280 Reading Reaches UART (25.00, then 30.50), UART Receive Interrupt Echoes A Line, UART Temp Command Prints A Reading (within 0.2 s of virtual time) and Missing Display Is Reported. Two consecutive runs gave the same result. With the `temp` command deliberately broken, the command test fails.
- The board file replaces I2C1 with `renode/STM32F4_I2C_AckModel.cs`, a copy of Renode's `STM32F1_I2C` with changes that follow RM0390 (see [renode_differences.md](renode_differences.md)). The firmware's BME280 read sequence is unchanged.
- With the stock model, the BME280 test fails: the one-byte chip-ID read never finishes, the BME280 model drops the next register address, and the firmware rejects the all-zero calibration with `bme280 init error`.

## VCD

Two runs of each command give byte-identical files. The VCD ends with a timestamp one step after the last executed instruction, so viewers draw the final change. The time axis is instruction steps, not nanoseconds.

- `--vcd blink.vcd -n 20000000 firmware.bin` (firmware `b133d9d`, before UART interrupts and the display): PA5 falls near step 8,000,000 and rises near 16,000,000; the UART and I2C bursts mark the sensor read. [Screenshot](img/vcd_blink.png).
- `--vcd i2c.vcd build/progs/22_i2c_chip_id.bin`: address EC, register D0, repeated START, address ED, then 0x60 with NACK. [Screenshot](img/vcd_i2c_22.png).

## CI check

`make && make test` passes in an Ubuntu 24.04 container (GCC 13.3, binutils 2.42), the same packages as the GitHub workflow. All 33 programs assemble to the same bytes as with binutils from GNU ARM 16.2.0.

## Not yet done

- Program 22 SR1 at EV6: RM0390 Rev 9 says TxE is not set during the address phase. The emulator and the patched Renode model follow that; stock Renode reads 0x82. A board read would settle what hardware shows (see renode_differences.md).
- Hardware runs. Nothing here has been verified on a real board.
