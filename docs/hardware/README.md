# Hardware checks, 2026-11-18

NUCLEO-F446RE (MB1136 rev C) with a HW-611 BME280 module on I2C1 (PB8 = SCL, PB9 = SDA, address 0x76). Firmware: stm32-baremetal-sensor-node commit 49765e3, reset clock (16 MHz HSI). Debug probe: the on-board ST-LINK (V2J33M25) with OpenOCD 0.12.0. Logic analyzer: a 24 MHz, 8-channel FX2 board read with sigrok-cli 0.7.2 (fx2lafw driver) at 4 MHz.

| File | What it shows |
| --- | --- |
| `gdb_first_session.txt` | First GDB session on the board: reset vector, `break main`, `stepi`, registers, memory |
| `emu_regs.txt` | The same GDB commands against `cortex-m-emu --gdb`; registers at `main+8` are identical |
| `uart_bme280_boot.txt`, `uart_bme280_touch.txt` | UART after reset, and 30 s of readings with a finger on the sensor |
| `i2c_boot.sr`, `i2c_boot.vcd`, `i2c_boot_decode.txt` | Capture across a reset: chip ID (0xD0 -> 0x60, NACK, STOP), then the calibration read from 0x88 |
| `i2c_periodic.sr`, `i2c_periodic_decode.txt` | Three seconds of the once-a-second temperature read |
| `gdb_txe_ev6.txt` | SR1 read by GDB with the CPU halted in `clear_addr` (0x0082 before SR2 is read, 0x0080 after) |
| `ev6.gdb`, `ev6_runs.txt` | SR1 value the firmware itself read, at full speed, when it first saw ADDR (0x0082 in 3 of 3 runs) |

The `.sr` files open in PulseView or with `sigrok-cli -i FILE -P i2c:scl=D2:sda=D0`. In both captures D2 is SCL and D0 is SDA.

## TxE at EV6

`ev6.gdb` puts a hardware breakpoint on the instruction in `wait_flag` that runs only when the polled flag is set. The polling loop itself runs at full speed, so the value at the breakpoint is the first SR1 read in which ADDR was set. For the write half of the chip-ID read that value is 0x0082. With the CPU halted instead (`gdb_txe_ev6.txt`) the value is the same. TxE is therefore set by the time ADDR is set, and the emulator now models it that way.

## Limits

One board, one BME280 module, room temperature, 100 kHz. The register comparison covers a single stopping point, not a trace. Nothing here measures cycle timing.
