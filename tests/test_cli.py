import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / 'build/cortex-m-emu'
SPEC = importlib.util.spec_from_file_location('check_vcd', ROOT / 'tools/check_vcd.py')
vcd = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vcd)


class CLIOutputs(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([str(CLI), *map(str, args)], capture_output=True, check=False)

    def test_uart_vcd_is_deterministic_and_has_all_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            paths = [Path(d) / 'a.vcd', Path(d) / 'b.vcd']
            for path in paths:
                result = self.run_cli('--vcd', path, ROOT / 'build/progs/16_usart.bin')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith(b'OK\n'))
                self.assertIn(b'steps=51', result.stdout)
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())
            _, changes = vcd.read_vcd(paths[0])
            self.assertEqual([value for _, name, value in changes if name == 'uart_tx_byte'][1:], [0x4F, 0x4B, 0x0A])
            self.assertEqual([value for _, name, value in changes if name == 'uart_event'], [0,1,2,3])
            self.assertIn('NOT one ns', paths[0].read_text())

    def test_gpio_vcd_and_register_snapshot(self):
        with tempfile.TemporaryDirectory() as d:
            path, trace = Path(d) / 'gpio.vcd', Path(d) / 'gpio.csv'
            result = self.run_cli('--vcd', path, '--trace-regs', trace, ROOT / 'build/progs/11_gpio.bin')
            self.assertEqual(result.returncode, 0, result.stderr)
            _, changes = vcd.read_vcd(path)
            self.assertEqual([(s,v) for s,n,v in changes if n == 'pa5'], [(0,0),(12,1),(14,0),(16,1)])
            self.assertTrue(trace.read_text().startswith('step,pc,r0,'))
            self.assertEqual(len(trace.read_text().splitlines()), 20) # header, reset, 18 instructions
            # End marker one step after the last instruction, so the final PA5
            # change at step 16 has nonzero width in a viewer.
            stamps = [int(l[1:]) for l in path.read_text().splitlines() if l.startswith('#')]
            self.assertEqual(stamps[-1], 19)

    def test_i2c_vcd_keeps_final_nacked_byte_at_stop(self):
        with tempfile.TemporaryDirectory() as d:
            paths = [Path(d) / 'a.vcd', Path(d) / 'b.vcd']
            for path in paths:
                result = self.run_cli('--vcd', path, ROOT / 'build/progs/22_i2c_chip_id.bin')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn(b'note:', result.stdout)
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())
            _, changes = vcd.read_vcd(paths[0])
            self.assertEqual([v for _,n,v in changes if n == 'i2c_data'], [0,0xEC,0xD0,0xED,0x60])
            self.assertEqual([v for _,n,v in changes if n == 'i2c_ack'][-1],0)
            self.assertIn(1,[v for _,n,v in changes if n == 'i2c_repeated_start'])
            stamps = [int(l[1:]) for l in paths[0].read_text().splitlines() if l.startswith('#')]
            self.assertGreater(stamps[-1], max(s for s,_,_ in changes))

    def test_rejects_bad_arguments_and_unwritable_output(self):
        image = ROOT / 'build/progs/01_reset.bin'
        for args in [('-n','bad',image),('-n','-1',image),('--unknown',image),('--temperature-centi','8501',image)]:
            self.assertEqual(self.run_cli(*args).returncode, 2)
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.run_cli('--vcd',d,image).returncode,1)
            alias=Path(d)/'alias.bin'
            alias.symlink_to(image)
            self.assertEqual(self.run_cli('--vcd',alias,image).returncode,2)


if __name__ == '__main__': unittest.main()
