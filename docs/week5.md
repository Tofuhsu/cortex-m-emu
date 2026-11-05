# Week 5: transaction VCD and differential register traces

## VCD

`--vcd out.vcd` records PA5 plus UART/I2C transaction vectors. The choice is vector transactions, not invented serial bit timing. `uart_event` and `i2c_event` counters distinguish repeated byte values. Final receive data is retained across STOP, including when RX and STOP occur in the same instruction.

Timestamps are executed-instruction step numbers. VCD requires a timescale; the file declares `1 ns`, but **one step is not one nanosecond**. A header comment states this. Multiple callbacks can share a timestamp. These vectors cannot be decoded as physical UART TX or I2C SCL/SDA by a serial decoder and say nothing about electrical behavior.

The writer uses stable IDs and no wall-clock date or file path. Tests rerun the same USART/I2C binary, compare bytes, parse declarations/value widths, and verify received data and ACK. `tools/check_vcd.py` checks this project's emitted subset, not the entire IEEE language. The output opens in Surfer 0.7.0; screenshots are in `docs/img/`. A final timestamp one step after the last instruction lets the viewer draw the last change.

```sh
./build/cortex-m-emu --vcd build/i2c.vcd build/progs/22_i2c_chip_id.bin
python3 tools/check_vcd.py build/i2c.vcd
```

## Register traces

`--trace-regs path.csv` writes an exact header `step,pc,r0,...,r15,xpsr`. Row zero is reset; subsequent rows are **after** a successfully fetched instruction attempt increments steps. Handler entry is not a separate row. A fatal pre-fetch failure adds no row. PC is duplicated as r15 and checked for consistency by the parser.

`tools/compare_traces.py` requires consecutive steps from zero, all 32-bit register values, equal full trace lengths by default, and at least one executed instruction. `--steps N` explicitly requests a prefix that must exist in both inputs; it never silently takes the shorter trace. Differences report step, field and both values. Mismatch counts count differing fields, not just differing steps. Exit codes: 0 match, 1 mismatch, 2 malformed/missing evidence.

No differences are hidden by default. An optional allowlist requires an exact field, step interval, expected emulator value, expected Renode value and reason. The supplied reset-LR rule only accepts FFFFFFFF-versus-zero; it cannot hide a wrong carry or a different LR value. I2C/GPIO/SysTick differences are not blanket-ignored. Concrete additional rules require observed evidence and a documented explanation.

```sh
python3 tools/compare_traces.py emu.csv renode.csv --steps 6 \
  --allowlist tools/reset_lr_allowlist.json
```

## Renode exporter and boundaries

`tools/export_renode_trace.robot` is intended for `renode-test`, using BIN, REPL, OUT and STEPS variables. It loads the identical binary, sets VTOR to flash and starts single-step mode so Renode takes SP/PC from the vector table, and declares common GP/NZCV-zero preconditions because those reset values are architecturally unspecified. LR remains at Renode's reset value and is handled only by the explicit allowlist. `cpu Step` and public register access are used; see [Renode testing API](https://renode.readthedocs.io/en/latest/basic/renode-testing-api.html) and [execution tracing](https://renode.readthedocs.io/en/latest/execution-tracing/execution-tracing.html). Renode also supports execution traces, but their normal PC/opcode format is not a complete post-step register snapshot.

The exporter stops before BKPT because Renode single-step BKPT behavior differs. Thus test 01, consisting only of BKPT, needs a reset-only state check and cannot honestly be advertised as a nonzero-instruction differential run. For the other programs compare an explicit pre-BKPT prefix. SysTick's virtual-time scheduling differs from instruction ticks: select and document a common pre-interrupt prefix, not an unexplained register mask. GPIO clock-gating/BSRR differences need per-program, evidence-based rules if included.

Stepping runs inside Renode (`tools/renode_step_dump.py`), so a 20000-step firmware trace takes seconds. The full matrix has been run against Renode 1.17; results and the explained differences are in [renode_differences.md](renode_differences.md).

## Comparator mutation test

`python3 tools/test_trace_mutation.py --report build/mutation` compiles a temporary CPU whose AddWithCarry carry comparison is inverted. It runs program 03 in the reference and intentionally broken emulator and requires the comparator to find a carry difference. Formal source stays untouched. Actual local result: first C mismatch at step 2, six field mismatches over seven instructions. This validates fault detection, not agreement with Renode.

## GDB stub

`cortex-m-emu --gdb PORT image.bin` waits for one GDB connection on 127.0.0.1 (port 0 picks a free one and prints it). Packets: `?`, `g`/`G`, `p`/`P`, `m`/`M`, `c`, `s`, `Z0`/`z0`, `k`, `D`, `qSupported` and `qXfer:features:read`, which sends the `org.gnu.gdb.arm.m-profile` description (r0–r12, sp, lr, pc, xpsr as register 25). Checksums are verified and bad packets are answered with `-`. Ctrl-C (0x03) during `continue` stops with SIGINT.

Design decisions:
- Breakpoints are an address table checked before each step, so flash is never patched. `continue` from a breakpoint executes that instruction first.
- GDB memory access covers flash and SRAM only (`bus_debug_byte`). Peripheral reads can have side effects (I2C SR1/SR2, DR), so they are refused rather than performed.
- Writing xpsr changes only N, Z, C and V.

Validation:
- `tests/test_gdbstub.py` (part of `make test`) speaks the protocol to program 07: initial SP/PC/xPSR, a step, a breakpoint reached through BL (LR = 0x0800000F), SRAM write and read, a refused peripheral read, a register write, continue to BKPT with r0 = 116, a bad checksum and `k`.
- `tools/gdb_firmware.sh` runs `arm-none-eabi-gdb` 18.1 in batch mode on the sensor firmware: `break main`, `continue`, `stepi`, `info registers`, `x/4wx 0x20000000`, `kill`. It stops at `main ()` at `src/main.c:140` and then at 0x08000920.
- Ctrl-C was checked by hand on the firmware: the reply was `S02`, and the UART output and PA5 steps were the same as without GDB.

Pressure/humidity firmware output and additional instructions are outside the current verified target result.
