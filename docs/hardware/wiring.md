# Bench wiring

This is the fixed bench for every hardware check from 2026-11-18 on. Wire it exactly like this; if anything changes, change this file first.

Parts: NUCLEO-F446RE (MB1136 rev C), 128x64 SSD1306 I2C module (address jumper at 0x78, which is 0x3C as a 7-bit address), HW-611 BME280 module ("E/P 280" silkscreen, address 0x76), a full-size breadboard, a 24 MHz 8-channel FX2 logic analyzer.

Pin names below are the ones printed on the NUCLEO (CN5/CN6 Arduino headers) and the breadboard's own row numbers and column letters.

## Power: 3.3 V only

The BME280 module and the OLED both run from **3V3**. Only the rails on the f-j side are used; the a-e side rails carry nothing. The HW-611 module is wired to 3.3 V only on this bench; the 5V pin is never used.

## Wires

| # | From | To | Signal | Notes |
| --- | --- | --- | --- | --- |
| 1 | CN6 `3V3` | `+` rail on the f-j side, row 1 | 3.3 V | The pin next to it on CN6 is `5V`. Count from `IOREF`: NC, IOREF, RESET, **3V3**, 5V, GND, GND, VIN |
| 2 | CN6 `GND` (the one next to `5V`) | `-` rail on the f-j side, row 1 | GND | |
| 3 | rail `+` | row 11 (f-j side) | OLED VCC | |
| 4 | rail `-` | row 10 (f-j side) | OLED GND | |
| 5 | CN5 `D15` (PB8) | row 12, column h | SCL | |
| 6 | CN5 `D14` (PB9) | row 13, column h | SDA | |
| 7 | row 12 | row 22 | SCL to BME280 | Jumper wire |
| 8 | row 13 | row 23 | SDA to BME280 | Jumper wire |
| 9 | rail `+` (f-j side) | row 20 (a-e side) | BME280 VCC | Long wire across the board |
| 10 | rail `-` (f-j side) | row 21 (a-e side) | BME280 GND | Long wire across the board |
| 11 | rail `+` (f-j side) | row 24 | BME280 CSB | High (3.3 V) selects I2C. Before the breadboard it went to CN6 `IOREF` |
| 12 | rail `-` (f-j side) | row 25 | BME280 SDO | Low = address 0x76. Before the breadboard it went to CN6 `GND` |
| 13 | analyzer `CH0` (D0) | row 12 | SCL | Fitted only while capturing; not in the photo |
| 14 | analyzer `CH1` (D1) | row 13 | SDA | Fitted only while capturing; not in the photo |
| 15 | analyzer `GND` | `-` rail on the f-j side | GND | Fitted only while capturing. Earlier captures used the GND pin next to LD1 |

Module pin order, as placed on the breadboard:

- OLED, rows 10 to 13, f-j side: GND, VCC, SCL, SDA.
- BME280, rows 20 to 25, a-e side: VCC, GND, SCL, SDA, CSB, SDO.

Wire colors on the 2026-11-23 bench (from close-up photos; colors may change, the table does not):

- CN6 `3V3` olive, CN6 `GND` red. CN5 `D15` black, `D14` white.
- OLED power: orange from the `-` rail (about row 10) to row 10, yellow from the `+` rail (about row 11) to row 11.
- BME280, rows 20 to 25: teal (VCC, `+` rail), blue (GND, `-` rail), white (SCL, from row 12), black (SDA, from row 13), purple (CSB, `+` rail), gray (SDO, `-` rail).

The modules carry their own pull-up resistors on SCL and SDA. No extra pull-ups are fitted.

## Easy to get wrong

0. **Wire colors do not follow the usual code.** On this bench the 3V3 wire is olive and the GND wire is red. Go by this table, not by color.
1. **CN6 `5V` is right next to `3V3`.** Put the 3V3 wire on, then check it by counting from `IOREF` before plugging in USB.
2. **CN10 is one row off from the CN5 labels.** The morpho header CN10 sits next to CN5, and reading across from a CN5 label puts you one row away from the matching CN10 pin. Use the CN5 sockets (`D15`, `D14`) for the bus, not CN10.
3. **Analyzer channels changed between captures.** In `i2c_boot.sr` and `i2c_periodic.sr`, D2 = SCL and D0 = SDA. From `oled_boot.sr` on, and on this bench, **D0 = SCL and D1 = SDA**. Decode with `-P i2c:scl=D0:sda=D1`.
4. **Analyzer clips stay on the breadboard (rows 12 and 13), not on CN10.** Clipping header pins is how the channel mix-up happened.
5. **HW-611 CSB and SDO are both tied, never left open.** The BME280 datasheet uses CSB high for I2C (CSB pulled low selects SPI) and SDO to pick the address (low = 0x76).
6. **Re-wiring only with USB unplugged.** For the bus-stuck test later (SDA to GND), touch only the SDA row, never a row or rail that carries 3.3 V.

## Photo

`wiring_top.jpg`: the bench on 2026-11-23, taken from directly above with the NUCLEO upright; row numbers and column letters are readable. The logic analyzer is not fitted in the photo. The table above is the reference; the photo shows the layout.

## 10-second bench check

Run before every experiment. All three must pass before going further.

Terminal 1, UART (macOS resets the port settings when it closes, so `stty` and `cat` share one redirect):

```sh
( stty -f /dev/cu.usbmodem111103 115200 raw; cat ) < /dev/cu.usbmodem111103
```

Terminal 2, a 3-second capture started just before a reset:

```sh
sigrok-cli -d fx2lafw -c samplerate=4m --time 3s -C D0,D1 -o bench.sr &
sleep 0.5; openocd -f board/st_nucleo_f4.cfg -c "init; reset run; exit"; wait
sigrok-cli -i bench.sr -P i2c:scl=D0:sda=D1 -A i2c=address-write:address-read | sort | uniq -c
```

| Check | Pass |
| --- | --- |
| UART | `sensor-node boot`, no `ssd1306 not found`, no `bme280 init error`, then one `temperature:` line a second |
| I2C devices | The decode shows writes to both `3C` and `76`. Together with the UART lines above, this means both devices ACKed |
| Analyzer | The capture is not empty and the decode lists I2C transfers, so SCL and SDA are on D0 and D1 |

The firmware has no I2C scan command, so the boot traffic plus the absence of the two error lines stands in for a scan.
