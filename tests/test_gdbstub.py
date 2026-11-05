"""GDB Remote Serial Protocol tests for `cortex-m-emu --gdb`.

These speak the protocol directly, so CI needs no arm-none-eabi-gdb.
tools/gdb_firmware.sh runs the same commands through a real GDB.
"""
from pathlib import Path
import re
import socket
import struct
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / 'build/cortex-m-emu'
PROG = ROOT / 'build/progs/07_calls.bin'


def checksum(data: bytes) -> bytes:
    return b'%02x' % (sum(data) & 0xFF)


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buffer = b''

    def read_byte(self) -> int:
        if not self.buffer:
            self.buffer = self.sock.recv(4096)
            if not self.buffer:
                raise EOFError('stub closed the connection')
        byte, self.buffer = self.buffer[0], self.buffer[1:]
        return byte

    def send_raw(self, data: bytes) -> int:
        self.sock.sendall(data)
        return self.read_byte()

    def request(self, command: str) -> str:
        data = command.encode()
        ack = self.send_raw(b'$' + data + b'#' + checksum(data))
        assert ack == ord('+'), f'no ack for {command!r}: {ack!r}'
        while self.read_byte() != ord('$'):
            pass
        body = bytearray()
        while (byte := self.read_byte()) != ord('#'):
            body.append(byte)
        received = bytes([self.read_byte(), self.read_byte()])
        assert received == checksum(bytes(body)), 'bad reply checksum'
        self.sock.sendall(b'+')
        return body.decode()

    def register(self, number: int) -> int:
        return struct.unpack('<I', bytes.fromhex(self.request('p%x' % number)))[0]


class GdbStub(unittest.TestCase):
    def setUp(self):
        self.proc = subprocess.Popen([str(CLI), '--gdb', '0', str(PROG)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        line = self.proc.stderr.readline().decode()
        match = re.match(r'gdb: listening on 127\.0\.0\.1:(\d+)', line)
        self.assertIsNotNone(match, line)
        self.client = Client(int(match.group(1)))

    def tearDown(self):
        self.client.sock.close()
        self.proc.wait(timeout=10)
        self.proc.stdout.close()
        self.proc.stderr.close()

    def test_session(self):
        c = self.client
        self.assertIn('qXfer:features:read+', c.request('qSupported:multiprocess+'))
        xml = c.request('qXfer:features:read:target.xml:0,fff')
        self.assertTrue(xml.startswith('l'))
        self.assertIn('org.gnu.gdb.arm.m-profile', xml)
        self.assertEqual(c.request('?'), 'S05')

        regs = bytes.fromhex(c.request('g'))
        self.assertEqual(len(regs), 17 * 4)
        values = struct.unpack('<17I', regs)
        self.assertEqual(values[13], 0x20020000)  # SP from the vector table
        self.assertEqual(values[15], 0x08000008)  # PC = _start
        self.assertEqual(values[16], 0x01000000)  # xPSR: T bit only

        self.assertEqual(c.request('s'), 'S05')  # movs r0, #1
        self.assertEqual(c.register(15), 0x0800000A)
        self.assertEqual(c.register(0), 1)

        # Breakpoint at f1 (0x08000016), reached through the 32-bit BL.
        self.assertEqual(c.request('Z0,8000016,2'), 'OK')
        self.assertEqual(c.request('c'), 'S05')
        self.assertEqual(c.register(15), 0x08000016)
        self.assertEqual(c.register(14), 0x0800000F)  # return address | 1
        self.assertEqual(c.request('z0,8000016,2'), 'OK')

        # Memory: SRAM read/write, flash read, peripherals refused.
        self.assertEqual(c.request('M20000000,4:efbeadde'), 'OK')
        self.assertEqual(c.request('m20000000,4'), 'efbeadde')
        self.assertEqual(c.request('m8000008,2'), '0120')  # movs r0, #1 = 0x2001
        self.assertEqual(c.request('m40020014,4'), 'E14')
        self.assertEqual(c.request('P1=78563412'), 'OK')
        self.assertEqual(c.register(1), 0x12345678)

        # Continue to BKPT: r0 = 1 + 5 + 100 + 10.
        self.assertEqual(c.request('c'), 'S05')
        self.assertEqual(c.register(0), 116)
        self.assertEqual(c.register(15), 0x08000014)
        self.assertEqual(c.register(25) & 0xF0000000, 0)  # NZCV after adds r0, #10

    def test_bad_checksum_is_nacked(self):
        self.assertEqual(self.client.send_raw(b'$g#00'), ord('-'))
        self.assertEqual(self.client.request('?'), 'S05')

    def test_kill_ends_the_process(self):
        self.client.sock.sendall(b'$k#6b')
        self.assertEqual(self.client.read_byte(), ord('+'))
        self.assertEqual(self.proc.wait(timeout=10), 0)


if __name__ == '__main__':
    unittest.main()
