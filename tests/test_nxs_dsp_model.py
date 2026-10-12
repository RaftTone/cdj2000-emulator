"""CDJ_NXS_DSP_MODEL=1: the behavioural DSP answers MAIN's host port as the
stock DSP does in runs/fork-stock-obey-1, without executing any C674x code."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]
CONTROL, ADDRESS, FIXED = 0xc000000, 0xc040000, 0xc0c0000


def test_model_boot_handshake_and_mailboxes():
    qemu = Path(os.environ.get('CDJ_TEST_QEMU', ROOT / 'build/qemu/build/qemu-system-sh4'))
    if not qemu.is_file(): pytest.skip('requires the custom QEMU build')
    with tempfile.TemporaryDirectory(prefix='cdj-dspm-', dir='/tmp') as directory:
        root = Path(directory)
        rom = root / 'reset.bin'
        rom.write_bytes(struct.pack('<2H', 0xaffe, 0x0009))   # bra . ; nop
        endpoint = root / 'qtest.sock'
        environment = dict(os.environ, CDJ_NXS_DSP_MODEL='1')
        process = subprocess.Popen([str(qemu), '-M', 'cdj2000nxs-main', '-S', '-display', 'none',
            '-nodefaults', '-accel', 'qtest', '-bios', str(rom),
            '-qtest', f'unix:{endpoint},server=on,wait=off'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            env=environment)
        try:
            deadline = time.monotonic() + 10
            while not endpoint.exists():
                if process.poll() is not None:
                    pytest.fail(process.stderr.read().decode(errors='replace'))
                if time.monotonic() > deadline: pytest.fail('qtest socket startup timed out')
                time.sleep(.05)
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(5)
                sock.connect(str(endpoint))
                stream = sock.makefile('rwb', buffering=0)

                def command(text):
                    stream.write((text + '\n').encode())
                    response = stream.readline().decode().strip()
                    assert response.startswith('OK'), response
                    return response.split()[1:]

                def write(address, value): command(f'writel {address:#x} {value:#x}')
                def read(address): return int(command(f'readl {address:#x}')[0], 0)
                def put(dsp, value): write(ADDRESS, dsp); write(FIXED, value)
                def get(dsp): write(ADDRESS, dsp); return read(FIXED)
                def hint(): return not int(command('readw 0xfff10040')[0], 0) & 0x10
                def phase(value): command(f'writew 0xfff1005c {value:#x}')

                command('writew 0xfff10054 0x40')            # release DSP reset
                write(CONTROL, 0x01050105)                     # HWOB, ack ROM HINT
                assert not hint()
                put(0x11800000, 0x11802000)                    # boot entry word
                write(CONTROL, 0x014b014b)                     # DSPINT: start
                assert not read(CONTROL) & 2, 'stage 1 acks DSPINT'
                assert get(0x1183fff4) == 1
                phase(2)
                assert hint()
                write(CONTROL, 0x014d014d)
                assert not hint()
                phase(0)
                put(0x11837bc0, 0xdeadbeef)                    # second-record image
                phase(3)
                assert hint()
                write(CONTROL, 0x014d014d)
                phase(0)
                assert get(0x11837bc0) == 0
                assert get(0x11837bf8) == 1 and get(0x11837cc8) == 0x1a24
                for dsp, value in ((0x11837c9c, 0x1c), (0x11837ba0, 1), (0x11837cb0, 1),
                                   (0x11838140, 0x01010100), (0x11838100, 3)):
                    put(dsp, value)
                put(0x11837bd0, 0x3c)                          # MAIN-owned rate block
                write(CONTROL, 0x014b014b)
                assert not read(CONTROL) & 2
                for dsp in (0x11837c9c, 0x11837ba0, 0x11837cb0, 0x11838140, 0x11838100):
                    assert get(dsp) == 0, hex(dsp)
                assert get(0x11837bd0) == 0x3c
                put(0x11838144, 0x28)                          # 40 frames
                put(0x11838140, 0x01010100)
                put(0x11838100, 7)                             # unaccepted command
                put(0x118381c4, 1)
                write(CONTROL, 0x014b014b)
                assert get(0x11838100) == 7 and get(0x118381c4) == 0
                assert get(0x11838140) == 0
                assert get(0x11837cd0) == 40 and get(0x11837cc8) == 0x1a24 - 40
                # Play at 1.0: the position follows virtual time, ahead
                # frames become behind ones, and it stops at the buffer end.
                put(0x11837bc0, 0x100000)
                put(0x11837ba0, 2)
                write(CONTROL, 0x014b014b)
                assert get(0x11837ba0) == 0 and get(0x11837bf8) == 2
                command('clock_step 200000000')                # 0.2 s = 15 frames
                write(CONTROL, 0x014b014b)
                assert get(0x11837c10) == 15 and get(0x11837c50) == 15
                assert get(0x11837cd0) == 25 and get(0x11837ccc) == 15
                left = get(0x11837bf4)
                assert left >> 16 == left & 0xffff and 0 < left & 0xffff <= 588
                command('clock_step 1000000000')               # past 40 frames
                write(CONTROL, 0x014b014b)
                assert get(0x11837c10) == 40 and get(0x11837cd0) == 0
                put(0x11837ba0, 4)                             # cue: stops
                write(CONTROL, 0x014b014b)
                command('clock_step 200000000')
                write(CONTROL, 0x014b014b)
                assert get(0x11837bf8) == 4 and get(0x11837c10) == 40
                stream.close()
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            process.stderr.close()
