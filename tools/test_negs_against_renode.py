#!/usr/bin/env python3
"""Compare a temporary NEGS-C mutant with a real program25 Renode trace.

Requires GNU-assembled program25 and the separately exported Renode CSV.
No synthetic reference trace is generated. First require the correct emulator
to match the same six-instruction prefix, then change only NEGS carry.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from compare_traces import compare, read_allowlist, read_trace


def mutated_source(source):
    anchor = 'result = add_with_carry(~cpu->r[rm], 0, true, &cpu->c, &cpu->v);'
    if source.count(anchor) != 1:
        raise ValueError('NEGS mutation anchor changed; refusing to guess')
    return source.replace(anchor, anchor + '\n            cpu->c = !cpu->c; /* Deliberate test mutation. */')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, type=Path,
                        help='actual program25 CSV exported by Renode')
    parser.add_argument('--image', type=Path, default=root/'build/progs/25_data_processing.bin')
    parser.add_argument('--out', type=Path, default=root/'build/negs-renode-mutation')
    args = parser.parse_args()
    # The sixth instruction of the checked-in GNU source is RSBS r3,r3,#0.
    image = args.image.resolve()
    if image.read_bytes()[18:20] != b'\x5b\x42':
        raise ValueError('image is not the expected GNU program25 layout')
    reference = read_trace(args.reference)
    if len(reference) < 7:
        raise ValueError('actual Renode reference must include reset and six instructions')
    output = args.out.resolve()
    if output.exists():
        raise ValueError('output already exists; select a new --out to preserve evidence')
    output.mkdir(parents=True)
    rules = read_allowlist(root/'tools/reset_lr_allowlist.json')
    if len(rules) != 1 or rules[0]['field'] != 'r14' or \
            (rules[0]['emu'], rules[0]['renode']) != (0xFFFFFFFF, 0):
        raise ValueError('only the exact reset-LR rule is permitted')
    source = mutated_source((root/'src/cpu.c').read_text())
    with tempfile.TemporaryDirectory(prefix='negs-mutation-') as directory:
        temp = Path(directory)
        changed = temp/'cpu_mutated.c'; changed.write_text(source)
        sources = [root/'src'/name for name in ('main.c','bus.c','loader.c','vcd.c')]
        sources += sorted((root/'src/periph').glob('*.c'))
        command = [*shlex.split(os.environ.get('CC','cc')), '-std=c11','-Wall','-Wextra',
                   '-Werror','-O2','-I'+str(root/'src'), *map(str,sources),str(changed),
                   '-o',str(temp/'mutated')]
        subprocess.run(command, check=True)
        for name, executable in [('control',root/'build/cortex-m-emu'),('mutant',temp/'mutated')]:
            with (output/f'{name}.log').open('wb') as log:
                subprocess.run([str(executable),'-n','6','--trace-regs',str(output/f'{name}.csv'),str(image)],
                               stdout=log,stderr=subprocess.STDOUT,check=True)
        _, control, _ = compare(read_trace(output/'control.csv'),reference,rules,6)
        if control:
            raise ValueError('unmodified control disagrees with Renode; do not claim mutation detection')
        count, mismatches, allowed = compare(read_trace(output/'mutant.csv'),reference,rules,6)
        wrong_c = [item for item in mismatches if item[0] == 6 and item[1] == 'xpsr'
                   and item[2] ^ item[3] == 1 << 29]
        if not wrong_c:
            raise ValueError('NEGS carry mutation was not detected against the supplied Renode reference')
        report = {'kind':'temporary-NEGS-mutant-versus-user-supplied-actual-Renode-reference',
                  'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
                  'reference_sha256':hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                  'compared_steps':count,'control_mismatches':0,
                  'first_negs_carry_mismatch_step':6,'mismatches':mismatches,'allowed':allowed}
        (output/'mutation.json').write_text(json.dumps(report,indent=2)+'\n')
        print('PASS: correct control matches Renode; wrong NEGS C detected at instruction6')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'NOT PASSED: {error}',file=sys.stderr)
        raise SystemExit(2)
