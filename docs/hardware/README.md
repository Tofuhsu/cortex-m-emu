# Hardware checks, 2026-11-18

NUCLEO-F446RE (MB1136 rev C) with a HW-611 BME280 module on I2C1 (PB8 = SCL, PB9 = SDA, address 0x76) and, later the same day, a 128x64 SSD1306 OLED module at 0x3C on the same bus, both powered from 3.3 V. Firmware: stm32-baremetal-sensor-node commit 49765e3, reset clock (16 MHz HSI). Debug probe: the on-board ST-LINK (V2J33M25) with OpenOCD 0.12.0. Logic analyzer: a 24 MHz, 8-channel FX2 board read with sigrok-cli 0.7.2 (fx2lafw driver) at 4 MHz.

| File | What it shows |
| --- | --- |
| `gdb_first_session.txt` | First GDB session on the board: reset vector, `break main`, `stepi`, registers, memory |
| `emu_regs.txt` | The same GDB commands against `cortex-m-emu --gdb`; registers at `main+8` are identical |
| `uart_bme280_boot.txt`, `uart_bme280_touch.txt` | UART after reset, and 30 s of readings with a finger on the sensor |
| `i2c_boot.sr`, `i2c_boot.vcd`, `i2c_boot_decode.txt` | Capture across a reset: chip ID (0xD0 -> 0x60, NACK, STOP), then the calibration read from 0x88 |
| `i2c_periodic.sr`, `i2c_periodic_decode.txt` | Three seconds of the once-a-second temperature read |
| `gdb_txe_ev6.txt` | SR1 read by GDB with the CPU halted in `clear_addr` (0x0082 before SR2 is read, 0x0080 after) |
| `ev6.gdb`, `ev6_runs.txt` | SR1 value the firmware itself read, at full speed, when it first saw ADDR (0x0082 in 3 of 3 runs) |
| `uart_oled_boot.txt` | UART after a reset with the display connected; no `ssd1306 not found` line |
| `oled_boot.sr`, `oled_decode.txt` | Capture across a reset with both devices on the bus: BME280 setup, SSD1306 init, the first full frame and the once-a-second updates |
| `oled_closeup.png`, `setup.jpg` | The display showing `SENSOR NODE` and `T 27.93 C`, and the whole setup |
| `wiring.md`, `wiring_top.jpg` | Bench wiring for every check from 2026-11-18 on, and the 10-second bench check |

The `.sr` files open in PulseView or with `sigrok-cli -i FILE -P i2c:scl=D2:sda=D0`. In the two BME280 captures D2 is SCL and D0 is SDA; in `oled_boot.sr` D0 is SCL and D1 is SDA (`-P i2c:scl=D0:sda=D1`).

## TxE at EV6

`ev6.gdb` puts a hardware breakpoint on the instruction in `wait_flag` that runs only when the polled flag is set. The polling loop itself runs at full speed, so the value at the breakpoint is the first SR1 read in which ADDR was set. For the write half of the chip-ID read that value is 0x0082. With the CPU halted instead (`gdb_txe_ev6.txt`) the value is the same. TxE is therefore set by the time ADDR is set, and the emulator now models it that way.

## SSD1306

`oled_boot.sr` starts just before the reset, so its first transfers belong to the previous run. After the reset:

- 0x3C ACKs every transfer. The init transfer sends 25 command bytes, all ACKed, identical to the sequence in the firmware's `src/ssd1306.c`.
- The first full frame (8 pages of 128 bytes) matches `tests/emu/oled_25.00.pbm` in the firmware repo byte for byte in pages 0, 1 and 3 to 7. Page 2 is blank at that point because the temperature has not been drawn yet.
- Each later page-2 update differs from the emulator frame only in the columns of the temperature digits; `T 2`, the decimal point and ` C` are identical. The reading rose during the capture while a finger was on the sensor.

The photos were taken separately from the capture. They show the same layout, font and orientation as the emulator frame.

## Limits

One board, one BME280 module, one SSD1306 module, room temperature, 100 kHz. The register comparison covers a single stopping point, not a trace. Nothing here measures cycle timing. The frame comparison covers the bytes sent over I2C; the panel itself was compared with the emulator frame by eye, from a photo.
