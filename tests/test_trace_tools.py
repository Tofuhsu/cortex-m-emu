import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('compare_traces', Path(__file__).resolve().parents[1] / 'tools/compare_traces.py')
trace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trace)


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


if __name__ == '__main__': unittest.main()
