#!/usr/bin/env python3
"""Rebuild with GNU ARM tools and retain every real Renode comparison result.

No generated Renode traces, no implicit allowlists, no shortened comparisons.
Requires Renode's renode-test launcher, not plain robot. This orchestration
still needs a real Renode run; syntax/preflight checks do not validate it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from compare_traces import compare, read_allowlist, read_trace


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log, cwd):
    with log.open('wb') as stream:
        result = subprocess.run(list(map(str, command)), cwd=cwd, stdout=stream,
                                stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f'exit {result.returncode}; see {log}')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-repo', type=Path,
                        default=root.parent / 'stm32-baremetal-sensor-node')
    parser.add_argument('--renode-test', default=os.environ.get('RENODE_TEST', 'renode-test'))
    parser.add_argument('--out', type=Path, default=root / 'build/trace-matrix')
    parser.add_argument('--firmware-steps', type=int, default=20000)
    args = parser.parse_args()
    if args.firmware_steps < 1:
        parser.error('--firmware-steps must be positive')
    firmware = args.firmware_repo.resolve()
    platform = firmware / 'renode/stm32f446_nucleo.repl'
    # Preflight before any rebuild. Do not destroy the owner's current build.
    required = ['make', 'arm-none-eabi-as', 'arm-none-eabi-ld',
                'arm-none-eabi-objcopy', 'arm-none-eabi-gcc', args.renode_test]
    missing = [name for name in required if not shutil.which(name)]
    if missing or not platform.is_file():
        print(f'NOT RUN: missing {missing}; platform exists={platform.is_file()}; '
              'no rebuild performed', file=sys.stderr)
        return 2
    renode_command = Path(shutil.which(args.renode_test)).resolve()
    output = args.out.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('output directory is not empty; select a new --out to preserve evidence')
    output.mkdir(parents=True, exist_ok=True)
    assembler = subprocess.check_output(['arm-none-eabi-as', '--version'], text=True)
    if 'GNU assembler' not in assembler:
        parser.error('GNU ARM assembler is required')
    (output / 'assembler-version.txt').write_text(assembler)
    # Force actual GNU reassembly, rather than reusing ZIP checkpoint fixtures.
    run(['make', '-B', 'all', 'progs'], output / 'emu-build.log', root)
    run(['make', '-B'], output / 'firmware-build.log', firmware)
    binary = root / 'build/cortex-m-emu'
    rules_path = root / 'tools/reset_lr_allowlist.json'
    rules = read_allowlist(rules_path)
    if len(rules) != 1 or rules[0]['field'] != 'r14' or \
            (rules[0]['emu'], rules[0]['renode']) != (0xFFFFFFFF, 0):
        raise RuntimeError('matrix permits only the exact reset-LR allowlist')
    cases = [(source.stem, root / f'build/progs/{source.stem}.bin', None)
             for source in sorted((root / 'tests/progs').glob('[0-9][0-9]_*.s'))]
    cases.append(('firmware-prefix', firmware / 'build/firmware.bin', args.firmware_steps))
    report = {'kind': 'actual-emulator-versus-Renode; no synthetic traces',
              'allowlist_sha256': sha(rules_path), 'platform_sha256': sha(platform),
              'cases': []}
    for name, image, prefix in cases:
        folder = output / name
        folder.mkdir()
        entry = {'case': name, 'image_sha256': sha(image), 'status': 'ERROR'}
        try:
            left, right = folder / 'emu.csv', folder / 'renode.csv'
            run([binary, '--temperature-centi', '2500', '-n', prefix or 100000,
                 '--trace-regs', left, image], folder / 'emu.log', root)
            snapshots = read_trace(left)
            log = (folder / 'emu.log').read_text()
            if prefix is None:
                if 'stopped: bkpt #0\n' not in log:
                    raise RuntimeError('program did not reach BKPT #0 within its limit')
                # BKPT changes Renode's PC: exclude it explicitly on both sides.
                snapshots = snapshots[:-1]
                steps = len(snapshots) - 1
            else:
                steps = prefix
                if len(snapshots) != steps + 1 or 'step limit reached' not in log:
                    raise RuntimeError('firmware did not execute the complete requested prefix')
            run([renode_command, root / 'tools/export_renode_trace.robot',
                 '--variable', f'BIN:{image}', '--variable', f'REPL:{platform}',
                 '--variable', f'OUT:{right}', '--variable', f'STEPS:{steps}'],
                folder / 'renode.log', folder)
            other = read_trace(right)
            if len(other) != steps + 1:
                raise RuntimeError('Renode stopped before the exact requested prefix')
            entry['steps'] = steps
            if steps == 0:
                # Program01 consists only of BKPT. Never call this a passed
                # instruction comparison or manufacture an extra trace row.
                entry.update(status='RESET_ONLY', reason='BKPT excluded; zero instructions compared')
                raise ValueError('reset-only case requires separate reset acceptance')
            count, mismatches, allowed = compare(snapshots, other, rules)
            entry.update(status='MISMATCH' if mismatches else 'MATCH', steps=count,
                         mismatches=mismatches, allowed=allowed)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            entry['detail'] = str(exc)
        report['cases'].append(entry)
        (output / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'{name}: {entry["status"]}', flush=True)
    lines = ['# Actual trace matrix', '',
             '| Case | Compared instructions | Result |', '| --- | ---: | --- |']
    for case in report['cases']:
        lines.append(f'| {case["case"]} | {case.get("steps", "—")} | {case["status"]} |')
    lines += ['', 'RESET_ONLY is not an instruction comparison. All mismatches remain in matrix.json.',
              'Peripheral timing/calibration differences are not automatically ignored.']
    (output / 'matrix.md').write_text('\n'.join(lines) + '\n')
    return int(any(c['status'] != 'MATCH' for c in report['cases']))


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'matrix failed: {error}', file=sys.stderr)
        raise SystemExit(2)
