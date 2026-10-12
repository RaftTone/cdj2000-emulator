"""Test the actual QEMU NXS host port and DMA without proprietary firmware."""
import os
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_host_addressing_and_fixed_port_dma():
    qemu = Path(os.environ.get('CDJ_TEST_QEMU', ROOT / 'build/qemu/build/qemu-system-sh4'))
    if not qemu.is_file(): pytest.skip('requires the custom QEMU build')
    with tempfile.TemporaryDirectory(prefix='cdj-hpi-', dir='/tmp') as directory:
        root = Path(directory)
        rom = root / 'reset.bin'
        # Minimal SH-4 program: VBR=04000000, privileged SR with IMASK=0,
        # then loop. It runs only for the final interrupt-delivery assertion.
        rom.write_bytes(struct.pack('<8H2I', 0xd003, 0x402e, 0xd003, 0x400e,
                                    0xaffe, 0x0009, 0x0009, 0x0009,
                                    0x04000000, 0x40000000))
        endpoint = root / 'qtest.sock'
        qmp_endpoint = root / 'qmp.sock'
        history = root / 'dsp-fault-history.jsonl'
        environment = os.environ.copy()
        environment['CDJ_NXS_DSP_FAULT_HISTORY'] = str(history)
        process = subprocess.Popen([str(qemu), '-M', 'cdj2000nxs-main', '-S', '-display', 'none',
            '-nodefaults', '-bios', str(rom), '-qtest', f'unix:{endpoint},server=on,wait=off',
            '-qmp', f'unix:{qmp_endpoint},server=on,wait=off'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            env=environment)
        try:
            deadline = time.monotonic() + 10
            while not endpoint.exists():
                if process.poll() is not None:
                    pytest.fail(process.stderr.read().decode(errors='replace'))
                if time.monotonic() > deadline: pytest.fail('qtest socket startup timed out')
                time.sleep(.05)
            sock = socket.socket(socket.AF_UNIX)
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
            control, address, auto, fixed = 0xc000000, 0xc040000, 0xc080000, 0xc0c0000
            # Reset defaults high on active-low HINT and holds HPIRST. Releasing
            # CPU_DSP_RST enters the documented ROM HPI boot path, which
            # releases HPIRST and asserts HINT low for MAIN.
            assert read(control) == 0x00c800c8
            assert int(command('readw 0xfff10040')[0], 0) & 0x10
            command('writew 0xfff10054 0x40')
            assert read(control) == 0x004c004c
            assert not (int(command('readw 0xfff10040')[0], 0) & 0x10)
            write(control, 0x01050105)
            # HPIC includes reserved reset-one fields 6 and 3; HWOB also
            # appears in read-only HWOBSTAT bit 8 and is mirrored per halfword.
            assert read(control) == 0x01490149
            assert int(command('readw 0xfff10040')[0], 0) & 0x10
            write(address, 0x11801da0)
            write(auto, 0x12345678)
            write(auto, 0x90abcdef)
            assert read(address) == 0x11801da8
            write(address, 0x11801da0)
            assert read(fixed) == 0x12345678
            assert read(fixed) == 0x12345678
            assert read(address) == 0x11801da0
            assert read(auto) == 0x12345678
            assert read(auto) == 0x90abcdef
            # SPRS377F Table 3-4 maps the C6747's 128 KiB shared RAM at
            # 0x80000000. It is reachable through the same UHPI data ports.
            write(address, 0x80000020)
            write(fixed, 0xa5c33c5a)
            assert read(fixed) == 0xa5c33c5a
            assert read(address) == 0x80000020
            # Boot DMA register block, incrementing RAM source / fixed HPID.
            write(0x04001000, 0x11223344)
            write(0x04001004, 0x55667788)
            write(address, 0x11802000)
            write(0x1f608080, 0x04001000)
            write(0x1f608084, auto)
            write(0x1f608088, 2)
            command('writew 0x1f608060 1')
            write(0x1f60808c, 0x1431)
            assert read(0x1f608080) == 0x04001008
            assert read(0x1f608084) == auto
            assert read(0x1f608088) == 0
            assert read(0x1f60808c) & 2
            write(address, 0x11802000)
            assert read(auto) == 0x11223344
            assert read(auto) == 0x55667788
            # NXS PCM uses DMINT3: board register index 5, not boot index 8.
            # Completion must assert its own request, then the firmware ISR
            # acknowledges by clearing IE while leaving TE set.
            pcm, status = 0x1f608050, 0xffd4004c
            for acknowledge in (0x40001412, 0x40001414):
                write(address, 0x11802400)
                write(pcm, 0x04001000)
                write(pcm + 4, auto)
                write(pcm + 8, 2)
                write(pcm + 12, 0x40001415)
                assert read(pcm + 8) == 0
                assert read(pcm + 12) == 0x40001416
                assert read(status) & 8, 'HPI DMA completed without DMINT3'
                write(pcm + 12, acknowledge)
                assert not (read(status) & 8)
            # Polling transfers with IE clear must not request an interrupt.
            write(pcm, 0x04001000)
            write(pcm + 8, 2)
            write(pcm + 12, 0x40001411)
            assert read(pcm + 8) == 0
            assert not (read(status) & 8)
            # Prove delivery to the guest, not just a diagnostic pending bit.
            # ISR: record INTEVT, clear CHCR.IE (as NXS MAIN does), RTE.
            handler = struct.pack('<16H3I',
                0xd107, 0x6212, 0xd007, 0x2022, 0xd107, 0x6212,
                0xe3fb, 0x2239, 0x2122, 0x002b, 0x0009,
                0x0009, 0x0009, 0x0009, 0x0009, 0x0009,
                0xff000028, 0x04002000, 0xff60805c)
            command(f'write 0x04000600 {len(handler)} 0x{handler.hex()}')
            write(pcm, 0x04001000)
            write(pcm + 8, 2)
            write(pcm + 12, 0x40001415)
            with socket.socket(socket.AF_UNIX) as qmp:
                qmp.settimeout(5)
                qmp.connect(str(qmp_endpoint))
                with qmp.makefile('rwb', buffering=0) as qm:
                    assert 'QMP' in json.loads(qm.readline())
                    def execute(name):
                        qm.write((json.dumps({'execute': name}) + '\n').encode())
                        while True:
                            reply = json.loads(qm.readline())
                            if 'event' not in reply:
                                assert 'return' in reply, reply
                                return
                    execute('qmp_capabilities')
                    execute('cont')
                    deadline = time.monotonic() + 3
                    while read(0x04002000) == 0 and time.monotonic() < deadline:
                        time.sleep(.01)
                    execute('stop')
            assert read(0x04002000) == 0x6a0
            assert not (read(pcm + 12) & 4)
            assert not (read(status) & 8)
            # Run enough NOP packets to wrap the fault-history ring, then
            # fault. Verify the actual recorder keeps the newest 4096 phases
            # and emits both interrupt task-state registers.
            write(address, 0x11800000)
            write(fixed, 0x11802000)
            image = bytes(2100 * 4) + struct.pack('<I', 0xffffffff)
            # Keep each qtest line short; a single 8 KiB write timed out on
            # this QEMU build.
            for offset in range(0, len(image), 256):
                chunk = image[offset:offset + 256]
                command(f'write {0x04003000 + offset:#x} {len(chunk)} 0x{chunk.hex()}')
            write(address, 0x11802000)
            write(0x1f608080, 0x04003000)
            write(0x1f608084, auto)
            write(0x1f608088, 2101)
            write(0x1f60808c, 0x1431)
            assert read(0x1f608088) == 0
            write(control, 0x01030103)
            rows = [json.loads(line) for line in history.read_text().splitlines()]
            assert len(rows) == 4096
            assert {row['phase'] for row in rows} == {0, 1}
            assert all('tsr' in row and 'itsr' in row for row in rows)
            assert rows[0]['pc'] > 0x11802000
            assert rows[-1]['pc'] == 0x11802000 + 2100 * 4
            stream.close(); sock.close()
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            process.stderr.close()
