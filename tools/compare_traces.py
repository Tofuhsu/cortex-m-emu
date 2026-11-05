#!/usr/bin/env python3
"""Compare post-instruction CSV register snapshots. No implicit ignored fields."""
import argparse
import csv
import json
from pathlib import Path
import sys

FIELDS = ['step', 'pc', *[f'r{i}' for i in range(16)], 'xpsr']


def read_trace(path):
    rows = []
    with Path(path).open(newline='') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            raise ValueError(f'{path}: expected exact header {",".join(FIELDS)}')
        for line, row in enumerate(reader, 2):
            if set(row) != set(FIELDS) or any(v is None for v in row.values()):
                raise ValueError(f'{path}:{line}: missing/extra columns')
            try:
                values = {k: int(v, 0) for k, v in row.items()}
            except (ValueError, TypeError) as exc:
                raise ValueError(f'{path}:{line}: invalid integer') from exc
            if values['step'] != len(rows):
                raise ValueError(f'{path}:{line}: steps must be consecutive from zero')
            if any(not 0 <= values[k] <= 0xFFFFFFFF for k in FIELDS[1:]):
                raise ValueError(f'{path}:{line}: register outside uint32')
            if values['pc'] != values['r15']:
                raise ValueError(f'{path}:{line}: pc and r15 disagree')
            rows.append(values)
    if not rows:
        raise ValueError(f'{path}: empty trace')
    return rows


def read_allowlist(path):
    if path is None:
        return []
    rules = json.loads(Path(path).read_text())
    if not isinstance(rules, list):
        raise ValueError('allowlist must be an array')
    keys = {'field', 'first_step', 'last_step', 'emu', 'renode', 'reason'}
    for rule in rules:
        if not isinstance(rule, dict) or set(rule) != keys:
            raise ValueError(f'allowlist requires exactly {sorted(keys)}')
        if not isinstance(rule['field'], str) or rule['field'] not in FIELDS[1:] or not isinstance(rule['reason'], str) or not rule['reason'].strip():
            raise ValueError('allowlist requires a known field and nonempty reason')
        for key in ('first_step', 'last_step', 'emu', 'renode'):
            if isinstance(rule[key], str):
                rule[key] = int(rule[key], 0)
            if type(rule[key]) is not int or not 0 <= rule[key] <= 0xFFFFFFFF:
                raise ValueError(f'invalid allowlist {key}')
        if rule['first_step'] > rule['last_step'] or rule['emu'] == rule['renode']:
            raise ValueError('allowlist has empty range or identical values')
    return rules


def compare(left, right, rules, steps=None):
    if steps is None:
        if len(left) != len(right):
            raise ValueError(f'trace length differs: {len(left)-1} versus {len(right)-1} steps')
        count = len(left)
    else:
        if steps < 1 or len(left) <= steps or len(right) <= steps:
            raise ValueError('requested prefix must contain at least one step and exist in BOTH traces')
        count = steps + 1
    if count < 2:
        raise ValueError('zero executed instructions is not a valid comparison')
    mismatches, allowed = [], []
    for a, b in zip(left[:count], right[:count]):
        step = a['step']
        for field in FIELDS[1:]:
            if a[field] == b[field]:
                continue
            rule = next((r for r in rules if r['field'] == field and
                         r['first_step'] <= step <= r['last_step'] and
                         r['emu'] == a[field] and r['renode'] == b[field]), None)
            item = (step, field, a[field], b[field])
            if rule:
                allowed.append((*item, rule['reason']))
            else:
                mismatches.append(item)
    return count - 1, mismatches, allowed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('emu', type=Path)
    parser.add_argument('renode', type=Path)
    parser.add_argument('--allowlist', type=Path)
    parser.add_argument('--steps', type=int, help='explicit prefix; never silently truncate')
    args = parser.parse_args()
    try:
        count, mismatches, allowed = compare(read_trace(args.emu), read_trace(args.renode),
                                              read_allowlist(args.allowlist), args.steps)
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        print(f'comparison error: {exc}', file=sys.stderr)
        return 2
    for step, field, a, b in mismatches[:30]:
        print(f'step {step} {field}: emu=0x{a:08x}, renode=0x{b:08x}')
    for step, field, a, b, reason in allowed[:10]:
        print(f'allowed step {step} {field}: 0x{a:08x} vs 0x{b:08x}: {reason}')
    print(f'compared {count} steps: {len(mismatches)} mismatches ({len(allowed)} explicitly allowed field differences)')
    return int(bool(mismatches))


if __name__ == '__main__':
    raise SystemExit(main())
