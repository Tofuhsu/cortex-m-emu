# Renode 1.17 comparison results

Runtime: Renode 1.17.0, build `1.17.0+20260906gitf1dd1b4af`. That build pins `src/Infrastructure` to `renode-infrastructure` commit `cd4b002aac2398994c4a61a34a8b31b5700facf2`. Every Renode source line below refers to that commit, not to master.

## Instruction-by-instruction matrix

Command:

```
python3 tools/run_trace_matrix.py --firmware-repo ../stm32-baremetal-sensor-node
```

The matrix reassembles every program with GNU as and rebuilds the firmware. For each case it compares the emulator's register trace with a Renode single-step trace after every instruction.

Renode runs on the firmware repo's board file, `renode/stm32f446_nucleo.repl`. That file replaces I2C1 with `renode/STM32F4_I2C_AckModel.cs`, a patched copy of Renode's I2C model (see [the last section](#patched-i2c1-model)). The exporter loads that C# file with `include` and sets `i2c1ack InstantTransfers true`, so each I2C byte completes at once, as it does in the emulator; without it Renode completes a byte one byte period later in virtual time and flag-polling loops run a different number of times. The Robot tests keep Renode's default timing. Results with default timing and with the stock model are given where they differ.

Comparison rules:
- Only one known difference is allowed: reset LR (`0xFFFFFFFF` in the emulator, `0` in Renode), listed in `tools/reset_lr_allowlist.json`.
- Programs run up to, but not including, BKPT.
- Three programs are compared over a shorter prefix. Each prefix and its reason is in `tools/trace_prefixes.json`.
- The SysTick blink firmware (firmware commit `6151eed`, before the BME280 driver) is compared up to its first SysTick entry, which the tool finds from the emulator trace (`--blink-ref`).

| Case | Instructions compared | Result |
| --- | ---: | --- |
| 01 | 0 | Reset state matches (program is only BKPT) |
| 02–10, 12, 14, 16–18, 20, 23 | 2–50 each | Match |
| 24–35 (new Thumb/Thumb-2 instructions) | 4–25 each | Match |
| 13 SysTick | 106 (prefix) | Match up to the first SysTick entry |
| 15 PRIMASK | 52 (prefix) | Match up to the first SysTick entry |
| 19 NVIC software pend | 4 (prefix) | Match up to IRQ entry |
| 11 GPIOA | 17 | 1 difference at step 17 |
| 21 GPIOB | 12 | 1 difference at step 12 |
| 22 I2C chip ID | 60 | Match (default timing: differs from step 35; stock model: from step 28) |
| Blink firmware `6151eed`, up to the first SysTick entry | 16439 | Match (entry is step 16440) |
| Sensor firmware (BME280, UART interrupts, SSD1306), first 20000 steps | 20000 | Identical for 4299 steps; differs from step 4300, the first USART2 interrupt entry |

A mutation check confirms the comparison can catch a wrong flag. `tools/test_negs_against_renode.py` flips the C flag of NEGS in a temporary build. Compared against Renode's trace of program 25, the unmodified emulator matches all six steps, and the mutant is caught at step 6: xPSR is `0x41000000` in the mutant and `0x61000000` in Renode.

### Explained differences

- **13, 15: SysTick time base.** The emulator gives SysTick one tick per executed instruction. Renode counts in virtual time. The first exception entry therefore falls on a different step, and in 15 Renode takes no SysTick at all within the program.
- **19: step that records exception entry.** Renode records IRQ entry in the same step as the ISPR store that pends it. The emulator records it in the same step as the first handler instruction. From step 6 on, both traces agree again, including the return.
- **11: BSRR set and reset together.** RM0390 says set wins when both bits are written. The emulator reads back ODR = 0x20; Renode reads back 0.
- **21: AFRH byte write.** Renode's GPIO ignores a byte store to AFRH; a word store of the same value reads back correctly. RM0390 allows byte access to GPIO registers. The emulator stores the byte. The firmware writes AFRH with a word store, so it is not affected.
- **Sensor firmware: USART2 interrupt entry.** Step 4300 is the first USART2 TXE interrupt. Renode records the entry (IPSR = 54, PC = `USART2_IRQHandler`) in step 4300, the emulator in step 4301, together with the handler's first instruction; this is the same entry-step difference as program 19. From step 4301 the traces agree again until the next interrupt entry, at step 4402. No rule hides this; the blink firmware above is the case compared to its first exception.
- **Earlier BME280 firmware (before UART interrupts and the display): sensor calibration data.** Step 2028 is the first calibration byte read from I2C DR. The emulator's BME280 model returns 0xC0 (its declared synthetic calibration, T1 = 24000); Renode's model returns 0x00 (its fixed calibration, T1 = T2 = 0x8000, T3 = 0, `BME280.cs` line 174). The two sensor models hold different constants, so the compensation arithmetic differs from here on. Both print `temperature: 25.00`. This is a difference in the sensor models' data, not in instruction execution.
- **22 and firmware with default Renode timing: BTF timing.** Without `InstantTransfers`, program 22 waits for BTF at step 35: the emulator reads SR1 = 0x84 at once, Renode reads 0x80 until one byte period has passed, so it polls once more. The BME280 firmware's step 1019 is the same wait.
- **22 with the stock model: SR1 after the address byte.**
  - At step 28, stock Renode reads SR1 = 0x82 (ADDR and TxE). The emulator reads 0x02 (ADDR only).
  - RM0390 Rev 9 §24.6.6 (p768) says TxE is not set during the address phase, and Figure 275 in §24.3.3 (p752) shows TxE = 1 at EV8_1, after ADDR is cleared. The emulator and the patched model follow this: TxE is set when ADDR is cleared.
  - The manual does not list every flag combination at each register read, so this does not prove that 0x82 cannot be read on hardware.
- **Firmware with the stock model: I2C1 CR1.ACK.** Step 765 is the first `read32(CR1) | START` in the BME280 driver. The emulator reads 0x401 (PE and ACK); stock Renode reads 0x1, because its I2C model does not store ACK (see below). The patched model stores ACK, and this step matches.

## Robot test "BME280 Reading Reaches UART" with the stock model

With Renode's stock I2C1 model, the firmware prints `bme280 init error`. It prints `temperature: 25.00` on this emulator. Earlier firmware, without the all-zero calibration check, printed `temperature: 0.00` in Renode.

What happens, from a Renode debug log (`logLevel -1` on `i2c1` and `i2c1.bme280`):

1. **Chip-ID read (one byte).**
   - The firmware follows the single-byte master-receiver ending in RM0390 §24.3.3: clear ACK, clear ADDR, set STOP, wait for RXNE, read DR.
   - The sensor model logs `Write D0` and `Read 60`.
   - No `Stopping transmission` follows, so the controller never calls `FinishTransmission()` on the sensor.
2. **Calibration read.**
   - The register byte 0x88 is forwarded to the sensor, which logs `Trying to write without specifying address, byte is omitted`.
   - The six reads that follow return registers 0xD1–0xD6. All of them read as 0.

Cause in the Renode models:

1. `STM32F1_I2C.cs:107` defines ACK as a tagged flag, so ACK is not modeled.
2. In receive mode, `UpdateDataReceive` (lines 382–397) sets BTF one byte period after RXNE. It does this whether or not a second byte is acknowledged. Here BTF is set before the firmware sets STOP.
3. STOP in receive mode only moves the controller to `LastRead` (lines 157–176).
4. On the DR read, `HandleDataRead` copies BTF into RXNE (line 476). With BTF set, `LastRead` treats the read as not yet the last one and jumps to `ReadData` (lines 482–487). This skips the only path that calls `child.FinishTransmission()` (lines 489–503).
5. `BME280.cs` leaves its `Reading` state only in `FinishTransmission()` (lines 80–89). While in `Reading`, it discards written bytes instead of using them as the register address (lines 57–59).

With ACK = 0, the master NACKs the only byte and no second byte arrives. RM0390 Rev 7 §24.6.6 (p770) defines BTF in reception as a new byte received while DR still holds an unread byte. So on hardware BTF should not set in this sequence.

Decision: the firmware keeps the manual's single-byte sequence. No delay, extra byte, different polling condition or changed read length is added to make the Renode test pass. The Renode model is patched instead.

## Patched I2C1 model

`renode/STM32F4_I2C_AckModel.cs` in the firmware repo is a copy of `STM32F1_I2C.cs` at the commit above (MIT license, header kept). Each change is marked `ACK model` in the source:

1. CR1.ACK and CR1.POS are stored and read back.
2. In receive mode, another byte arrives (BTF) only after an ACKed byte. With ACK = 0 before a one-byte read, BTF stays clear, so the DR read after STOP takes the path that calls `FinishTransmission()`.
3. In receive mode, the first byte is received when ADDR is cleared (SR1 then SR2 read), not when the address byte is written. The master stretches SCL while ADDR is set (RM0390 §24.3.3), which is what lets firmware clear ACK in time.
4. In transmit mode, TxE is set when ADDR is cleared, not during the address phase (RM0390 Rev 9 §24.6.6, p768).

Changes 2 and 3 are both needed: with change 2 only, the stock receive timing still sets RxNE before the firmware clears ACK, and the log still shows the dropped address.

The board file removes the stock `i2c1` (`i2c1: @none`) and registers the copy at 0x40005400 as `i2c1ack`, with the same interrupt lines, because a repl entry cannot change an existing peripheral's type. The Robot test and `sensor_node.resc` load the C# file with `include` first. With the patched model, the debug log shows `Stopping transmission` after the chip-ID read, the sensor accepts 0x88 as the register address, and all four Robot tests pass.
