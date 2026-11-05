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


def load_prefixes(path, names):
    if path is None:
        return {}
    values = json.loads(path.read_text())
    if not isinstance(values, dict):
        raise ValueError('prefix file must map program names to steps/reason objects')
    for name, value in values.items():
        if name not in names or name == '01_reset':
            raise ValueError(f'unknown or reset-only prefix case: {name}')
        if not isinstance(value, dict) or set(value) != {'steps', 'reason'} or \
                type(value['steps']) is not int or value['steps'] < 1 or \
                not isinstance(value['reason'], str) or not value['reason'].strip():
            raise ValueError(f'{name}: prefix requires positive integer steps and a reason')
    return values


def compare_reset(left, right, rules):
    """Check only reset state; never manufacture an executed instruction."""
    mismatches, allowed = [], []
    for field in left:
        if field == 'step':
            continue
        if left[field] == right[field]:
            continue
        item = (0, field, left[field], right[field])
        rule = next((r for r in rules if r['field'] == field and
                     r['first_step'] <= 0 <= r['last_step'] and
                     r['emu'] == left[field] and r['renode'] == right[field]), None)
        if rule:
            allowed.append((*item, rule['reason']))
        else:
            mismatches.append(item)
    return mismatches, allowed


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
    parser.add_argument('--blink-ref', default='6151eed',
                        help='firmware commit with the SysTick blink and UART banner, compared up to '
                             'its first SysTick entry; empty string skips it')
    parser.add_argument('--prefixes', type=Path, default=root / 'tools/trace_prefixes.json',
                        help='JSON of explicit program prefix steps and reasons; no inferred truncation')
    args = parser.parse_args()
    if args.firmware_steps < 1:
        parser.error('--firmware-steps must be positive')
    firmware = args.firmware_repo.resolve()
    platform = firmware / 'renode/stm32f446_nucleo.repl'
    # The firmware platform replaces I2C1 with a patched model it ships as C#.
    model = firmware / 'renode/STM32F4_I2C_AckModel.cs'
    sources = sorted((root / 'tests/progs').glob('[0-9][0-9]_*.s'))
    prefixes = load_prefixes(args.prefixes, {source.stem for source in sources})
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
    blink = output / 'blink-src'
    if args.blink_ref:
        run(['git', '-C', firmware, 'worktree', 'add', '--detach', blink, args.blink_ref],
            output / 'blink-worktree.log', root)
    try:
        return run_cases(args, root, firmware, platform, model, sources, prefixes, output,
                         renode_command, blink if args.blink_ref else None)
    finally:
        if args.blink_ref:
            subprocess.run(['git', '-C', str(firmware), 'worktree', 'remove', '--force', str(blink)],
                           check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def first_exception_step(binary, image, folder, limit=200000):
    """Step of the first exception entry in the emulator (IPSR != 0), or None."""
    probe = folder / 'probe.csv'
    run([binary, '-n', limit, '--trace-regs', probe, image], folder / 'probe.log', folder)
    for row in read_trace(probe):
        if row['xpsr'] & 0x1FF:
            return row['step']
    return None


def run_cases(args, root, firmware, platform, model, sources, prefixes, output, renode_command, blink):
    binary = root / 'build/cortex-m-emu'
    rules_path = root / 'tools/reset_lr_allowlist.json'
    rules = read_allowlist(rules_path)
    if len(rules) != 1 or rules[0]['field'] != 'r14' or \
            (rules[0]['emu'], rules[0]['renode']) != (0xFFFFFFFF, 0):
        raise RuntimeError('matrix permits only the exact reset-LR allowlist')
    cases = [(source.stem, root / f'build/progs/{source.stem}.bin',
              prefixes.get(source.stem, {}).get('steps'), platform, model) for source in sources]
    cases.append(('firmware-prefix', firmware / 'build/firmware.bin', args.firmware_steps,
                  platform, model))
    if blink is not None:
        run(['make', '-B'], output / 'blink-build.log', blink)
        image = blink / 'build/firmware.bin'
        probe_dir = output / 'blink-probe'
        probe_dir.mkdir()
        entry_step = first_exception_step(binary, image, probe_dir)
        if entry_step is None or entry_step < 2:
            raise RuntimeError('blink firmware took no exception within the probe limit')
        # Compare every instruction before the step that enters the first SysTick handler.
        prefixes['blink-firmware'] = {
            'steps': entry_step - 1,
            'reason': f'{args.blink_ref}, up to the first SysTick entry (step {entry_step})'}
        cases.append(('blink-firmware', image, entry_step - 1, blink / 'renode/stm32f446_nucleo.repl',
                      blink / 'renode/STM32F4_I2C_AckModel.cs'))
    report = {'kind': 'actual-emulator-versus-Renode; no synthetic traces',
              'allowlist_sha256': sha(rules_path), 'platform_sha256': sha(platform),
              'cases': []}
    for name, image, prefix, case_platform, case_model in cases:
        folder = output / name
        folder.mkdir()
        entry = {'case': name, 'image_sha256': sha(image), 'status': 'ERROR'}
        entry['boundary_reason'] = prefixes.get(name, {}).get('reason',
            'explicit firmware prefix' if prefix else 'stop before BKPT')
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
                 '--variable', f'BIN:{image}', '--variable', f'REPL:{case_platform}',
                 '--variable', f'MODEL:{case_model if case_model.is_file() else ""}',
                 # The emulator completes I2C transfers at once; make the patched
                 # Renode model do the same so polling loops run the same count.
                 '--variable', f'AFTER_LOAD:{"sysbus.i2c1ack InstantTransfers true" if case_model.is_file() else ""}',
                 '--variable', f'OUT:{right}', '--variable', f'STEPS:{steps}'],
                folder / 'renode.log', folder)
            other = read_trace(right)
            if len(other) != steps + 1:
                raise RuntimeError('Renode stopped before the exact requested prefix')
            entry['steps'] = steps
            if steps == 0:
                # Program01 consists only of BKPT: reset acceptance is separate.
                mismatches, allowed = compare_reset(snapshots[0], other[0], rules)
                entry.update(status='MISMATCH' if mismatches else 'RESET_MATCH',
                             reason='reset state only; zero instructions compared',
                             mismatches=mismatches, allowed=allowed)
            else:
                count, mismatches, allowed = compare(snapshots, other, rules)
                entry.update(status='MISMATCH' if mismatches else 'MATCH', steps=count,
                             mismatches=mismatches, allowed=allowed)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            entry['detail'] = str(exc)
        report['cases'].append(entry)
        (output / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'{name}: {entry["status"]}', flush=True)
    lines = ['# Actual trace matrix', '',
             '| Case | Compared instructions | Result | Boundary |', '| --- | ---: | --- | --- |']
    for case in report['cases']:
        reason = case['boundary_reason'].replace('|', '/').replace('\n', ' ')
        lines.append(f'| {case["case"]} | {case.get("steps", "—")} | {case["status"]} | {reason} |')
    lines += ['', 'RESET_MATCH checks reset only, not instruction execution. All mismatches remain in matrix.json.',
              'Peripheral timing/calibration differences are not automatically ignored.']
    (output / 'matrix.md').write_text('\n'.join(lines) + '\n')
    return int(any(c['status'] not in ('MATCH', 'RESET_MATCH') for c in report['cases']))


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'matrix failed: {error}', file=sys.stderr)
        raise SystemExit(2)
