# Remaining-work checkpoint — not final acceptance

Baseline input commits: emulator `fb7783a`; firmware `6151eed`.

| Item | Implemented | Evidence available | Outstanding acceptance |
| --- | --- | --- | --- |
| A NVIC | pending bitset, ISER/ICER/ISPR/ICPR/IABR/IPR, IRQ38->IPSR54, enable/PRIMASK gate | host tests; programs 19/20 execute via limited fixture encoding | clean GNU assembly; Renode single-step |
| B GPIOB | reset values, AFRH lanes, AF4/open-drain queries, I2C1EN | host tests; program 21 | GNU rebuild / manual review |
| C I2C1 | polling master, 1/2/N bytes, repeated START, NACK/AF, flag sequences, gating/events | host tests; actual firmware C driver; program 22 | real Renode transfer comparison |
| D BME280 | synthetic calibration, nearest raw T/P/H, reset/forced/normal/skip/shadow | independent full compensation from bus-read calibration/raw; 25.00->2500 | review design/quantization choices |
| D2 firmware | init, checked int32 compensation, UART formatting, periodic LED schedule, mask restoration | actual C driver runs against bus, including 25.00/30.50 and masked caller | ARM GCC build; identify/implement emitted missing CPU instructions; actual new binary UART/LED; Robot |
| E VCD | deterministic transaction vectors and honest step axis | byte equality and parser tests on actual emulator USART/I2C runs | actual viewer loading and screenshot |
| F diff | post-step exporter, strict comparator, exact allowlist, mutation test | wrong actual CPU carry caught at step 2 | real Renode exporter run and full per-program/prefix matrix |
| G GDB | deliberately deferred (optional) | none | deferred |
| H docs | README changes previewed; week4/week5/design notes | this report | final measured Status after target work |

## Actual local validation

- Existing C test runner: **20/20 passed**, using the baseline binary fixtures included in the ZIP.
- Four added host groups: NVIC, I2C/GPIOB, BME280 compensation and MSR PRIMASK: passed, including UBSan.
- Five new program fixtures (19–23): passed through CPU/bus/models. Because GNU ARM binutils are unavailable, these were encoded with a deliberately limited fixture encoder, not assembled with GNU tools. Normal Makefile still invokes GNU assembler; this distinction must not be lost.
- Python comparator tests and CLI/VCD tests: passed. UART test 16 emitted `OK\n` and halted at 51 steps. GPIO VCD events were steps 12/14/16. UART and I2C VCD repeated runs were byte-identical.
- Actual firmware C driver, host build with UBSan: init, one/two/N reads, temperatures 25.00/30.50/-12.34/endpoints, formatting, PRIMASK restoration and NACK recovery passed. This is **not** execution of ARM firmware instructions.
- Deliberately inverted AddWithCarry C in a temporary CPU: caught at step 2. This is not Renode comparison.
- The ZIP's **original** `firmware.bin` ran 20,000,000 steps through the updated emulator: CRLF boot banner, PA5 at 447, 8,000,470, 16,000,467. The intervals 8,000,023 and 7,999,997 describe the old UART/SysTick firmware only, not the new I2C firmware.

## Blocking environment gap

No `arm-none-eabi-gcc/as/ld/objdump`, Renode or PulseView executable is installed. Package retrieval attempts were blocked. Consequently `make clean && make test` from source, new firmware compilation and Renode/Robot/viewer checks have NOT completed. CI has not been run or observed for these changes. No push was performed.

Do not mark this checkpoint complete, quote new firmware PA5 instruction intervals, claim 0 Renode mismatches, or infer passing target tests from host tests.

## Smallest next input

After applying the firmware source changes on the user's ARM GCC host:

```sh
bash tools/build_review_artifacts.sh
```

Return `build/firmware-review-artifacts.zip`. It contains the actual ELF, binary, disassembly, map and source/compiler hashes. The emulator can then be run here against that exact new binary and missing instructions added based on actual objdump encodings, as required by the plan. Alternatively provide a Linux x86_64 GNU ARM toolchain to enable that work locally.

For the eventual full target check (requires GNU ARM plus Renode):

```sh
bash tools/validate_remaining.sh /absolute/path/stm32-baremetal-sensor-node
```

Run this from the emulator repo. It fails clearly if tools are missing, or if the emulator halts on an undefined instruction. It does not label an incomplete Renode matrix as complete. The Robot LED/UART/BME280 tests are now enabled so a future CI run will enforce them, but that run has not happened here.
