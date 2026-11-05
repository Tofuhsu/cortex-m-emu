#!/usr/bin/env python3
"""Compile a deliberately wrong CPU in a temporary directory; comparator must catch C.
This is a comparator mutation test against a reference emulator, NOT a Renode run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import os
from compare_traces import compare, read_trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    image = root / 'build/progs/03_adds_subs.bin'
    original = root / 'src/cpu.c'
    old = '*carry_out = (uint64_t)result != unsigned_sum;'
    new = '*carry_out = (uint64_t)result == unsigned_sum;'
    source = original.read_text()
    if source.count(old) != 1:
        raise SystemExit('mutation anchor changed: refusing to guess')
    with tempfile.TemporaryDirectory() as tmp:
        temp = Path(tmp)
        changed = temp / 'cpu_mutated.c'
        changed.write_text(source.replace(old, new))
        sources = [root / 'src' / n for n in ['main.c','bus.c','loader.c','vcd.c']]
        sources += sorted((root / 'src/periph').glob('*.c'))
        command = [*shlex.split(os.environ.get('CC','cc')), '-std=c11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'src'),
                   *map(str,sources),str(changed),'-o',str(temp/'mutated')]
        subprocess.run(command, check=True)
        files = [temp/'reference.csv',temp/'mutated.csv']
        for binary, trace in zip([root/'build/cortex-m-emu',temp/'mutated'],files):
            subprocess.run([str(binary),'--trace-regs',str(trace),str(image)],check=True,capture_output=True)
        count, mismatches, _ = compare(read_trace(files[0]),read_trace(files[1]),[])
        flag_mismatches = [m for m in mismatches if m[1]=='xpsr' and (m[2]^m[3])&(1<<29)]
        if not flag_mismatches:
            raise SystemExit('FAIL: changed CPU carry was not detected')
        first = flag_mismatches[0]
        report = dict(kind='reference-emulator-vs-deliberately-mutated-emulator; NOT Renode',
                      image=str(image.relative_to(root)), image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                      compared_steps=count, field_mismatches=len(mismatches), first_carry_mismatch_step=first[0],
                      expected_xpsr=hex(first[2]),mutated_xpsr=hex(first[3]))
        if args.report:
            args.report.mkdir(parents=True,exist_ok=True)
            (args.report/'mutation.json').write_text(json.dumps(report,indent=2)+'\n')
            for path in files:
                (args.report/path.name).write_bytes(path.read_bytes())
        print(f'PASS: deliberately inverted AddWithCarry C caught at step {first[0]} ({len(mismatches)} field mismatches over {count} steps); no Renode claim')


if __name__ == '__main__':
    main()
