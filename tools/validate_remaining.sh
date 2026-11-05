#!/usr/bin/env bash
# Full TARGET acceptance, for a machine with GNU ARM + Renode installed.
# Failure is not skipped; logs retain the exact missing/undefined instruction.
set -euo pipefail
emu_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
firmware_repo=${1:-"$(dirname -- "$emu_repo")/stm32-baremetal-sensor-node"}
for tool in cc python3 arm-none-eabi-as arm-none-eabi-ld arm-none-eabi-objcopy arm-none-eabi-gcc arm-none-eabi-objdump; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool (no clean/build has been performed)" >&2; exit 2; }
done
make -C "$emu_repo" clean
mkdir -p "$emu_repo/build/acceptance"
evidence_dir="$emu_repo/build/acceptance"
make -C "$emu_repo" 2>&1 | tee "$evidence_dir/emu-build.log"
make -C "$emu_repo" test 2>&1 | tee "$evidence_dir/emu-tests.log"
make -C "$firmware_repo" clean
make -C "$firmware_repo" 2>&1 | tee "$evidence_dir/firmware-build.log"
make -C "$firmware_repo" test-host EMU_DIR="$emu_repo" 2>&1 | tee "$evidence_dir/firmware-host.log"
arm-none-eabi-objdump -d "$firmware_repo/build/firmware.elf" > "$evidence_dir/firmware.disasm"
"$emu_repo/build/cortex-m-emu" --temperature-centi 2500 -n 20000000 --vcd "$evidence_dir/blink.vcd" \
    "$firmware_repo/build/firmware.bin" 2>&1 | tee "$evidence_dir/firmware-25.log"
"$emu_repo/build/cortex-m-emu" --temperature-centi 3050 -n 2000000 \
    "$firmware_repo/build/firmware.bin" 2>&1 | tee "$evidence_dir/firmware-30.5.log"
python3 - "$evidence_dir" <<'PY'
from pathlib import Path
import re
import sys
p=Path(sys.argv[1])
for filename,temperature in [('firmware-25.log',b'25.00'),('firmware-30.5.log',b'30.50')]:
    data=(p/filename).read_bytes()
    assert b'sensor-node boot\r\n' in data, 'missing boot banner'
    assert b'temperature: '+temperature+b'\r\n' in data, 'missing compensated temperature'
    assert b'error' not in data.lower() and b'note:' not in data, 'check diagnostics'
steps=[int(x) for x in re.findall(rb'PA5 -> [01] at step (\d+)',(p/'firmware-25.log').read_bytes())]
assert len(steps)>=3,'need at least three actual LED transitions'
print('PA5 instruction intervals:',[b-a for a,b in zip(steps,steps[1:])])
PY
python3 "$emu_repo/tools/check_vcd.py" "$evidence_dir/blink.vcd"
python3 "$emu_repo/tools/test_trace_mutation.py" --report "$evidence_dir/mutation"
renode_test=${RENODE_TEST:-"$HOME/Applications/Renode.app/Contents/MacOS/renode-test"}
[[ -x "$renode_test" ]] || { echo "Renode acceptance NOT RUN: set RENODE_TEST to renode-test" >&2; exit 3; }
PATH="$firmware_repo/.venv/bin:$PATH" make -C "$firmware_repo" test-all RENODE_TEST="$renode_test" 2>&1 | tee "$evidence_dir/renode-tests.log"
# One explicit six-instruction arithmetic prefix (BKPT excluded).
"$emu_repo/build/cortex-m-emu" --trace-regs "$evidence_dir/emu-adds.csv" "$emu_repo/build/progs/03_adds_subs.bin" > "$evidence_dir/emu-adds.log"
"$renode_test" "$emu_repo/tools/export_renode_trace.robot" \
    --variable "BIN:$emu_repo/build/progs/03_adds_subs.bin" \
    --variable "REPL:$firmware_repo/renode/stm32f446_nucleo.repl" \
    --variable "OUT:$evidence_dir/renode-adds.csv" --variable "STEPS:6"
python3 "$emu_repo/tools/compare_traces.py" "$evidence_dir/emu-adds.csv" "$evidence_dir/renode-adds.csv" \
    --steps 6 --allowlist "$emu_repo/tools/reset_lr_allowlist.json" | tee "$evidence_dir/diff-adds.log"
echo "This checked one differential prefix, not the entire F matrix. Open blink.vcd in a VCD viewer for manual inspection."
echo "Acceptance evidence: $evidence_dir"
