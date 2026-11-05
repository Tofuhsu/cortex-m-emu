# Manual verification notes, 2026-11-05

These are source checks, not Renode execution results. Page numbers are printed
page labels. RM0390 full-PDF fetches exceeded the retrieval limit; its indexed
ST-hosted sections were checked individually. Some indexed URLs retain Rev7;
do not silently treat their page numbers as Rev9. Rows that still cite Rev7
say so.

## Register locators

| Item | Checked value | Primary locator |
| --- | --- | --- |
| NVIC bank bases | ISER E000E100, ICER E180, ISPR E200, ICPR E280, IABR E300, IPR E400 | PM0214 Rev10 4.3.1 p208; 4.3.2–7 pp210–215 |
| USART2 vector | IRQ38, exception54, vector offset D8 | RM0390 Rev9 table38 p237 |
| GPIOB base | 40020400 | RM0390 table1 p58, Rev7 and Rev9 |
| GPIOB MODER reset | 00000280 | RM0390 Rev9 7.4.1 p185 |
| GPIOB OSPEEDR/PUPDR reset | 000000C0 / 00000100 | RM0390 Rev9 7.4.3–4 p186 |
| GPIOA OSPEEDR reset | 0C000000 | RM0390 Rev9 7.4.3 p186 |
| BSRR conflict | set wins; reads return0 | RM0390 Rev9 7.4.7 note p188 |
| AFRH | offset24, reset0 | RM0390 Rev9 7.4.10 p189; table26 p192 confirms map |
| PB8/PB9 | AF4 I2C1 SCL/SDA | DS10693 Rev11 table11 p59 |
| APB1ENR | offset40, reset0, I2C1EN bit21 | RM0390 Rev9 6.3.13 p145 |
| AHB1ENR reset | 00000000 | RM0390 Rev7 6.3.10 p143; model's existing reset0 is correct |
| I2C CR1/CR2 | offsets00/04; PE0, START8, STOP9, ACK10, POS11; FREQ[5:0] | RM0390 24.6.1–2; Rev9 contents pp762/764; bit text retrieved in Rev7 |
| I2C DR/SR1 | offsets10/14; SB0 ADDR1 BTF2 RXNE6 TXE7 AF10 | RM0390 Rev9 24.6.5–6 p766 |
| I2C TxE at address phase | "TxE is not set during address phase"; TxE=1 at EV8_1, after ADDR is cleared | RM0390 Rev9 24.6.6 p768; 24.3.3 figure275 p752 |
| I2C SR2 | offset18; MSL0 BUSY1 TRA2 | RM0390 24.6.7, Rev9 contents p769; indexed bit table checked |
| I2C CCR/TRISE | offsets1C/20 | RM0390 24.6.8–9, Rev9 contents pp770–771; formula text also checked in Rev7 pp771–772 |
| BME280 identity/calibration | D0=60; T1/T2/T3 at88–8D | Bosch revision1.24 5.4.1 p27; 4.2.2 table16 p24 |
| Humidity latch/skipped samples | ctrl_hum applies on ctrl_meas write; T/P80000, H8000 | Bosch 5.4.3 p27; tables20/23/24 pp28–29 |

Standard-mode arithmetic from CCR's high/low periods: at PCLK1=16 MHz and
100 kHz, CCR=16000000/(2*100000)=80. With 1 us maximum standard-mode rise,
TRISE=16+1=17. These configure firmware registers; this model does not simulate
electrical rise time or baud/bus clock timing.

Sources: [PM0214](https://www.st.com/resource/en/programming_manual/pm0214-stm32-cortexm4-mcus-and-mpus-programming-manual-stmicroelectronics.pdf),
[RM0390 Rev9](https://www.st.com/resource/en/reference_manual/DM00135183-.pdf),
[RM0390 indexed Rev7](https://www.st.com/st-web-ui/static/active/en/resource/technical/document/reference_manual/DM00135183.pdf),
[DS10693](https://www.st.com/resource/en/datasheet/stm32f446re.pdf),
[Bosch datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf).

## Questions about this implementation

Pending has one authoritative copy in **CPU**, shared by core and external
exceptions. NVIC owns enables/IPR and exposes the external pending bits through
register aliases. It does not own a second pending array. PRIMASK gates entry;
requests remain set until successful entry or explicit clear. This differs from
the premise of the proposed interview question; it is a deliberate API choice.

For one-byte I2C receive, ACK must be off before clearing ADDR because that
releases reception of the only byte. POS shifts the ACK decision to the next
byte for the two-byte ending. The SR1/SR2 read sequence changes ADDR state and
starts receive buffering, so a debugger's read can change behavior. A host peek
must not arm it. This is why the sequence state exists in I2C but not in the
instantaneous USART model.

BME raw inversion uses binary search for the chosen monotone synthetic
calibration. The host test now checks every one of the 12,501 supported integer
centidegree inputs through I2C, with an independent full compensation formula:
all return exactly the requested centidegree and none returns the skipped raw
sentinel. This zero error is limited to this calibration and temperature API;
it says nothing about arbitrary real chips or Renode's calibration. Humidity
and pressure quantization are separate and remain covered by their tolerances.

Firmware multiply checks magnitudes before multiplying so invalid calibration
cannot cause signed overflow, and avoids pulling int64 libgcc routines into
the target. It conservatively rejects magnitude 2^31 too. Portable asr uses
unsigned shifts and explicit sign extension: negative signed right shift is
implementation-defined in C11. Critical I2C receive endings save/restore the
caller's PRIMASK, so a previously masked caller stays masked.

VCD output uses deterministic declaration/event order, integer instruction
steps and no wall-clock date. Identical bytes still need an actual viewer
check; the parser is insufficient evidence. The trace allowlist matches the
exact FFFFFFFF-versus-zero reset-LR pair only. A wildcard LR exclusion could
hide a broken BL or exception return and is not used.
