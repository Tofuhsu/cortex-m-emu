#!/usr/bin/env python3
"""Strict checker for this project's transaction-vector VCD subset (not a viewer)."""
from pathlib import Path
import re
import sys


def read_vcd(path):
    signals, changes = {}, []
    step = 0
    definitions_done = False
    for number, text in enumerate(Path(path).read_text().splitlines(), 1):
        line = text.strip()
        if not line:
            continue
        if line.startswith('$var '):
            if definitions_done:
                raise ValueError(f'{number}: declaration after definitions')
            parts = line.split()
            if len(parts) != 6 or parts[1] != 'wire' or parts[5] != '$end':
                raise ValueError(f'{number}: malformed variable')
            width, code, name = int(parts[2]), parts[3], parts[4]
            if width < 1 or code in signals:
                raise ValueError(f'{number}: invalid/duplicate variable')
            signals[code] = (width, name)
        elif line == '$enddefinitions $end':
            definitions_done = True
        elif line.startswith('$'):
            if line not in ('$dumpvars', '$end') and not line.endswith('$end'):
                raise ValueError(f'{number}: unterminated directive')
        elif line.startswith('#'):
            current = int(line[1:])
            if not definitions_done or current < step:
                raise ValueError(f'{number}: backwards timestamp or missing definitions')
            step = current
        else:
            if not definitions_done:
                raise ValueError(f'{number}: value before definitions')
            if line.startswith('b'):
                match = re.fullmatch(r'b([01]+) (\S+)', line)
                if match is None:
                    raise ValueError(f'{number}: malformed vector')
                bits, code = match.groups()
            else:
                if len(line) < 2 or line[0] not in '01':
                    raise ValueError(f'{number}: invalid scalar')
                bits, code = line[0], line[1:]
            if code not in signals or len(bits) > signals[code][0]:
                raise ValueError(f'{number}: unknown variable or width overflow')
            changes.append((step, signals[code][1], int(bits, 2)))
    if not definitions_done or not signals or not changes:
        raise ValueError('incomplete/empty VCD')
    return signals, changes


if __name__ == '__main__':
    try:
        signals, changes = read_vcd(sys.argv[1])
    except (OSError, ValueError, IndexError) as exc:
        print(f'VCD error: {exc}', file=sys.stderr)
        raise SystemExit(1)
    print(f'valid transaction VCD: {len(signals)} signals, {len(changes)} changes')
