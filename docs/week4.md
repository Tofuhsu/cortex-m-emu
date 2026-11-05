# Week 4: GPIOB, I2C v1 and BME280

## References and checked values

- [RM0390 Rev 9](https://www.st.com/resource/en/reference_manual/dm00135183-stm32f446xx-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf): register boundaries in chapter 2; RCC 6.3.13; GPIO 7.4.1/3/4/10; I2C master receiver in 24.3.3 and register descriptions in 24.6. The full PDF exceeded the retrieval size limit; relevant indexed manual sections were consulted.
- [DS10693 Rev 11, table 11](https://www.st.com/resource/en/datasheet/stm32f446re.pdf): PB8/PB9 use AF4 for I2C1 SCL/SDA.
- [Bosch BME280 datasheet 1.24](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf): 4.2.3, 5.3–5.4, 6.2.

| Item | Implemented value |
| --- | --- |
| GPIOB / RCC GPIOBEN | 0x40020400 / AHB1ENR bit 1 |
| GPIOB MODER / OSPEEDR / PUPDR reset | 0x280 / 0xC0 / 0x100 |
| AFRH offset / PB8, PB9 | 0x24 / AF4, open-drain required |
| APB1ENR offset / I2C1EN / reset | 0x40 / bit 21 / 0 |
| I2C1 base | 0x40005400 |
| CR1, CR2, OAR1, OAR2, DR | 0x00, 0x04, 0x08, 0x0C, 0x10 |
| SR1, SR2, CCR, TRISE, FLTR | 0x14, 0x18, 0x1C, 0x20, 0x24 |
| CR1 PE/START/STOP/ACK/POS | bits 0/8/9/10/11 |
| SR1 SB/ADDR/BTF/RXNE/TXE/AF | bits 0/1/2/6/7/10 |
| SR2 MSL/BUSY/TRA | bits 0/1/2 |

GPIO supports natural byte/halfword/word lanes in AFRH, as in AFRL. The constructor accepts GPIOA or GPIOB with its corresponding enable bit. `gpio_pin_is_open_drain()` reports configuration, not an electrical voltage or pull-up model.

## I2C design decisions

Polling seven-bit master only: no slave mode, DMA, PEC, arbitration, bus timing or I2C IRQs. Register accesses accept aligned halfwords/words at register starts. CR2.FREQ/CCR/TRISE are stored; they do not validate a bus frequency or schedule clock edges. Unsupported control modes increment diagnostics rather than silently pretending to support them.

| Flag | Set | Clear |
| --- | --- | --- |
| SB | accepted START | SR1 read followed by DR address write |
| ADDR | address ACK | SR1 read followed by SR2 read |
| TXE/BTF | immediate ACKed byte completes | next DR write, then immediate completion can set them again |
| RXNE | at least one receive token | DR consumption, then refill if allowed |
| BTF in receive | both DR/shift-register positions occupied | consume DR, then refill if allowed |
| AF | address/data NACK | SR1 write-zero, or disable/reset |

Important RM0390 24.6.7 detail: an SR1 read can arm ADDR clearing even if ADDR becomes set **after** that read. `sr1_read` therefore records the read itself; `i2c_peek()` never arms it. The SB sequence separately records that SB was observed. These are observable protocol state, unlike the simplified USART's immediate TC completion.

Two receive tokens represent DR and the shift register. Bytes are requested from the device when software consumes DR; ACK decisions are latched in the tokens. This avoids speculative extra reads while supporting the documented one-byte NACK, two-byte POS, and final-three-byte sequences. N-byte reads finish with exactly one final NACK. STOP waits for already-buffered bytes to be consumed; this is a transaction-level simplification, not a claim about BUSY's cycle timing. A repeated START emits no STOP and preserves the device's register pointer.

Gating diagnostics are separate: APB1 clock off, PE off, or PB8/PB9 not AF4/open-drain (including GPIOB clock off). Invalid accesses and malformed sequences have separate counters. Observers report START/address/TX/RX/STOP and ACK/read/repeated status. Device contexts are borrowed and must outlive active transactions.

## BME280 design decisions

Calibration is synthetic, chosen for this model, not measured on a physical chip and not attributed to a datasheet example:

- T1=24000, T2=25600, T3=0.
- P1=31250; P2–P9=0.
- H1=0, H2=256, H3=0, H4=256, H5=H6=0.

For this specific calibration, compensated T and H are nondecreasing over the raw domain and P is decreasing. Binary search selects the nearest representable raw value. This does not assert global monotonicity for arbitrary real-chip calibration. Temperature is set in centidegrees C, humidity in Q22.10 %RH, pressure in integer Pa. Quantization can prevent an arbitrary requested humidity/pressure from being represented exactly.

Chip ID is 0x60 at 0xD0; writing 0xB6 to 0xE0 resets configuration while preserving physical environment inputs. Reading calibration is little-endian; sample fields follow Bosch's big-endian byte layout. Read bursts increment the register pointer. Multi-write uses register/data **pairs**, not automatic write increment.

Conversions are instantaneous, status measuring/im_update are zero. Forced mode takes one snapshot then sleeps; normal mode refreshes on host environment changes. Data bursts are shadowed at read-address time. `ctrl_hum` only takes effect after `ctrl_meas` is written. Skipped T/P/H channels produce 0x80000/0x80000/0x8000. Noise, oversampling resolution, filtering and standby/conversion times are not modeled; config fields are stored only.

Tests read calibration/raw bytes through I2C and run independent full Bosch integer formulas. They cover 25.00 C -> 2500, 30.50 C -> 3050, signed/end-point temperatures, humidity endpoints and pressure endpoints. Pressure is evaluated on the host with int64; the firmware intentionally prints temperature only.

## Firmware

`src/bme280.c` in the sibling firmware repo implements bounded polling, NACK recovery, repeated START and the 1/2/N receiver endings. Critical windows save/restore PRIMASK through MRS/CPSID/MSR. The CPU's new MSR PRIMASK support preserves an outer masked critical section. A thin MMIO seam lets the actual C driver execute against these models in host tests.

At unchanged reset PCLK1=16 MHz, standard-mode CCR=16,000,000/(2*100,000)=80; TRISE=16+1=17 and CR2.FREQ=16. PA5 scheduling uses a recurring SysTick deadline so I2C/UART work does not accumulate delay drift, provided service work completes within a period. This is not a real-time guarantee.

Target GCC compilation, missing-instruction discovery, new PA5 interval measurements and Renode/Robot verification are **pending**. Do not substitute host C tests for those checks. See `acceptance.md`.
