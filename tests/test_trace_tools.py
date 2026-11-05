import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('compare_traces', Path(__file__).resolve().parents[1] / 'tools/compare_traces.py')
trace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trace)
sys.modules['compare_traces'] = trace
MATRIX_SPEC = importlib.util.spec_from_file_location('run_trace_matrix',
    Path(__file__).resolve().parents[1] / 'tools/run_trace_matrix.py')
matrix = importlib.util.module_from_spec(MATRIX_SPEC)
MATRIX_SPEC.loader.exec_module(matrix)


def fixture():
    rows = []
    for step in range(4):
        row = dict.fromkeys(trace.FIELDS, 0)
        row.update(step=step, pc=0x08000000+2*step, r15=0x08000000+2*step, xpsr=0x01000000)
        rows.append(row)
    return rows


class Traces(unittest.TestCase):
    def test_equal_and_flag_mutation(self):
        a, b = fixture(), fixture()
        self.assertEqual(trace.compare(a, b, []), (3, [], []))
        b[2]['xpsr'] ^= 1 << 29  # deliberately wrong ADDS carry
        count, mismatches, _ = trace.compare(a, b, [])
        self.assertEqual(count, 3)
        self.assertEqual(mismatches[0][:2], (2, 'xpsr'))

    def test_lengths_and_explicit_prefix(self):
        with self.assertRaises(ValueError): trace.compare(fixture(), fixture()[:2], [])
        self.assertEqual(trace.compare(fixture(), fixture()[:2], [], 1)[0], 1)
        with self.assertRaises(ValueError): trace.compare(fixture(), fixture()[:2], [], 2)
        with self.assertRaises(ValueError): trace.compare(fixture()[:1], fixture()[:1], [])

    def test_exact_allowlist_is_not_blanket_ignore(self):
        a, b = fixture(), fixture()
        a[0]['r14'] = 0xFFFFFFFF
        rule = dict(field='r14', first_step=0, last_step=3, emu=0xFFFFFFFF, renode=0, reason='reset')
        self.assertEqual(len(trace.compare(a, b, [rule])[2]), 1)
        b[0]['r14'] = 1
        self.assertEqual(len(trace.compare(a, b, [rule])[1]), 1)
        a[0]['xpsr'] ^= 1 << 29
        self.assertEqual(len(trace.compare(a, b, [rule])[1]), 2)

    def test_parser_rejects_duplicates_and_empty(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'trace.csv'
            p.write_text(','.join(trace.FIELDS) + '\n')
            with self.assertRaises(ValueError): trace.read_trace(p)
            p.write_text(','.join(trace.FIELDS) + '\n' + ','.join(str(v) for v in fixture()[0].values()) + '\n' * 2)
            self.assertEqual(len(trace.read_trace(p)), 1)
            p.write_text(p.read_text() + ','.join(str(v) for v in fixture()[0].values()) + '\n')
            with self.assertRaises(ValueError): trace.read_trace(p)

    def test_reset_only_checks_still_detect_wrong_sp_and_flags(self):
        a, b = fixture()[0], fixture()[0]
        a['r14'] = 0xFFFFFFFF
        rule = dict(field='r14', first_step=0, last_step=0,
                    emu=0xFFFFFFFF, renode=0, reason='reset')
        mismatches, allowed = matrix.compare_reset(a, b, [rule])
        self.assertEqual(mismatches, [])
        self.assertEqual(len(allowed), 1)
        b['r13'] = 4
        b['xpsr'] ^= 1 << 29
        mismatches, _ = matrix.compare_reset(a, b, [rule])
        self.assertEqual([item[1] for item in mismatches], ['r13', 'xpsr'])
        self.assertTrue(all(item[0] == 0 for item in mismatches))

    def test_prefixes_cannot_silently_remove_cases_or_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'prefixes.json'
            accepted = {'13_systick': {'steps': 10, 'reason': 'before first asynchronous entry'}}
            path.write_text(json.dumps(accepted))
            self.assertEqual(matrix.load_prefixes(path, {'13_systick'}), accepted)
            for invalid in [
                {'unknown': {'steps': 10, 'reason': 'unknown program'}},
                {'01_reset': {'steps': 1, 'reason': 'must not invent an executed instruction'}},
                {'13_systick': {'steps': 0, 'reason': 'empty prefix'}},
                {'13_systick': {'steps': True, 'reason': 'bool is not a count'}},
                {'13_systick': {'steps': 10, 'reason': ' '}},
                {'13_systick': {'steps': 10, 'reason': 'no extra allowlist', 'ignore': 'xpsr'}},
            ]:
                path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    matrix.load_prefixes(path, {'13_systick', '01_reset'})


if __name__ == '__main__': unittest.main()
