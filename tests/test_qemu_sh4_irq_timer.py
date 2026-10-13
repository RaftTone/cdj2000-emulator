"""Exercise the patched SH-4 INTC and TMU in QEMU without deck firmware."""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

import pytest


ROOT = Path(__file__).resolve().parents[1]
QEMU = Path(os.environ.get('CDJ_TEST_QEMU', ROOT / 'build/qemu/build/qemu-system-sh4'))

# qtest and QMP are reached over Unix sockets.
pytestmark = pytest.mark.skipif(
    sys.platform == 'win32' or not hasattr(socket, 'AF_UNIX'),
    reason='needs Unix-domain sockets for qtest/QMP')


@contextmanager
def machine(rom: bytes, *, virtual_clock: bool = False, icount: bool = False):
    if not QEMU.is_file():
        pytest.skip('requires the custom QEMU build')
    # A short directory: Unix socket paths are limited to about 104 bytes.
    short = '/tmp' if os.path.isdir('/tmp') else None
    with tempfile.TemporaryDirectory(prefix='cdj-sh4-tmu-', dir=short) as directory:
        root = Path(directory)
        image = root / 'reset.bin'
        image.write_bytes(rom)
        qtest_path, qmp_path = root / 'qtest.sock', root / 'qmp.sock'
        command = [str(QEMU), '-M', 'cdj2000nxs-main', '-display', 'none',
                   '-nodefaults', '-bios', str(image)]
        if virtual_clock:
            command += ['-accel', 'qtest']
        else:
            command += ['-S']
            if icount:
                # Guest time from instructions only: what the guest does
                # between two timer ticks is the same on every host.
                command += ['-icount', 'shift=0,sleep=off']
        command += ['-qtest', f'unix:{qtest_path},server=on,wait=off',
                    '-qmp', f'unix:{qmp_path},server=on,wait=off']
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10
            while not (qtest_path.exists() and qmp_path.exists()):
                if process.poll() is not None:
                    pytest.fail(process.stderr.read().decode(errors='replace'))
                if time.monotonic() > deadline:
                    pytest.fail('QEMU control sockets did not open')
                time.sleep(.01)
            with socket.socket(socket.AF_UNIX) as qtest, socket.socket(socket.AF_UNIX) as qmp:
                qtest.settimeout(5)
                qmp.settimeout(5)
                qtest.connect(str(qtest_path))
                qmp.connect(str(qmp_path))
                qt = qtest.makefile('rwb', buffering=0)
                qm = qmp.makefile('rwb', buffering=0)
                assert 'QMP' in json.loads(qm.readline())

                def request(line):
                    qt.write((line + '\n').encode())
                    answer = qt.readline().decode().strip().split()
                    assert answer and answer[0] == 'OK', (line, answer)
                    return int(answer[1], 0) if len(answer) > 1 else None

                def execute(name):
                    qm.write((json.dumps({'execute': name}) + '\n').encode())
                    while True:
                        answer = json.loads(qm.readline())
                        if 'event' not in answer:
                            assert 'return' in answer, answer
                            return answer['return']

                execute('qmp_capabilities')
                yield request, execute
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            process.stderr.close()


def test_tmu_stop_keeps_underflow_and_reset_restores_registers():
    with machine(bytes(64), virtual_clock=True) as (request, execute):
        request('writel 0xffd80008 2')  # TMU0 TCOR
        request('writel 0xffd8000c 2')  # TMU0 TCNT
        request('writew 0xffd80010 0x20')  # TCR.UNIE
        request('writeb 0xffd80004 1')  # TSTR: start TMU0
        request('clock_step 1000000')
        assert request('readw 0xffd80010') & 0x100  # TCR.UNF

        # Stopping the count does not clear UNF; software writes it to 0.
        request('writeb 0xffd80004 0')
        assert request('readw 0xffd80010') & 0x100
        request('writew 0xffd80010 0x20')
        assert not request('readw 0xffd80010') & 0x100

        # A pending underflow must not survive a reset either.
        request('writeb 0xffd80004 1')
        request('clock_step 1000000')
        assert request('readw 0xffd80010') & 0x100

        execute('system_reset')
        assert request('readl 0xffd80008') == 0xffffffff
        assert request('readl 0xffd8000c') == 0xffffffff
        assert request('readw 0xffd80010') == 0
        assert request('readb 0xffd80004') == 0


def test_tmu_keeps_underflows_the_guest_has_not_seen():
    """qemu-sh-tmu-catch-up.patch: each underflow is one interrupt."""
    with machine(bytes(64), virtual_clock=True) as (request, execute):
        # P-phi/4 is 13.5 MHz on this board: TCOR 13499 is a 1 ms tick.
        request('writel 0xffd80008 13499')
        request('writel 0xffd8000c 13499')
        request('writew 0xffd80010 0x20')  # TCR.UNIE
        request('writeb 0xffd80004 1')
        request('clock_step 3500000')  # three underflows, none acknowledged

        def clears_until_unf_is_clear():
            clears = 0
            while request('readw 0xffd80010') & 0x100:
                request('writew 0xffd80010 0x20')
                clears += 1
                assert clears <= 5
            return clears

        # UNF stays set until the guest has cleared it once for each.
        assert clears_until_unf_is_clear() == 3
        request('clock_step 1000000')
        assert clears_until_unf_is_clear() == 1

        # Stopping the channel drops what it kept; UNF stays until cleared.
        request('clock_step 3000000')
        request('writeb 0xffd80004 0')
        assert clears_until_unf_is_clear() == 1


def test_tmu_drops_what_it_kept_leaving_unf_as_the_chip_has_it():
    """qemu-sh-tmu-catch-up.patch: a UNF set again for a kept underflow goes
    when the channel stops or UNIE is cleared, as the chip's would be clear."""
    with machine(bytes(64), virtual_clock=True) as (request, execute):
        request('writel 0xffd80008 13499')
        request('writel 0xffd8000c 13499')
        request('writew 0xffd80010 0x20')  # TCR.UNIE
        request('writeb 0xffd80004 1')

        def unf():
            return bool(request('readw 0xffd80010') & 0x100)

        # A handler that clears UNF, then stops its channel: after the clear
        # the chip's UNF is clear, so the stop leaves no interrupt behind.
        request('clock_step 3500000')  # three underflows, none acknowledged
        request('writew 0xffd80010 0x20')
        assert unf()  # one of the two kept
        request('writeb 0xffd80004 0')
        assert not unf()

        # Clearing UNIE drops them too, also when UNF is written back as 1
        # (a read-modify-write of TCR): the next underflow is one interrupt.
        request('writeb 0xffd80004 1')
        request('clock_step 3500000')
        request('writew 0xffd80010 0x100')  # UNIE clear
        request('writew 0xffd80010 0x120')  # UNIE set again
        request('clock_step 1000000')
        assert unf()
        request('writew 0xffd80010 0x20')
        assert not unf()


def _priority_rom():
    # Boot with SR.BL set. Poll a RAM word until the test has observed both
    # pending TMU flags, then clear BL so the first accepted vector is stable.
    code = bytearray(struct.pack('<32H', *([0x0009] * 32)))

    def instruction(address, opcode):
        struct.pack_into('<H', code, address, opcode)

    def literal(address, value):
        struct.pack_into('<I', code, address, value)

    def mov_l(pc, register, address):
        return 0xd000 | (register << 8) | ((address - ((pc + 4) & ~3)) // 4)

    for pc, register, address in ((0, 0, 0x30), (4, 0, 0x34),
                                  (8, 1, 0x38), (0x10, 0, 0x3c)):
        instruction(pc, mov_l(pc, register, address))
    for pc, opcode in ((2, 0x402e),   # LDC R0,VBR
                       (6, 0x400e),   # LDC R0,SR (BL set)
                       (0x0a, 0x6012), (0x0c, 0x2008), (0x0e, 0x89fc),
                       (0x12, 0x400e), (0x14, 0xaffe)):
        instruction(pc, opcode)
    for address, value in ((0x30, 0x04000000), (0x34, 0x50000000),
                           (0x38, 0x04003000), (0x3c, 0x40000000)):
        literal(address, value)
    return bytes(code)


def _stopping_handler(clear_first):
    # TMU0's handler at VBR + 0x600: clear UNF and stop the channel, in either
    # order, count the interrupt in RAM at 0x04002000, and return.
    clear = [1, 0xe020, 0x2101]  # R1 = TCR0; MOV #0x20,R0; MOV.W R0,@R1
    stop = [2, 0xe000, 0x2200]   # R2 = TSTR; MOV #0,R0; MOV.B R0,@R2
    count = [3, 0x6032, 0x7001, 0x2302]  # R3 = counter; R0 = @R3 + 1; @R3 = R0
    code = (clear + stop if clear_first else stop + clear) + count + [0x002b, 0x0009]
    assert len(code) % 2 == 0  # the literals follow, 4-byte aligned
    pool = 2 * len(code)
    literals = (0xffd80010, 0xffd80004, 0x04002000)  # TCR0, TSTR, counter
    for index, word in enumerate(code):
        if word < 4:  # MOV.L @(disp,PC),Rn of literal n
            pc = 2 * index
            address = pool + 4 * (word - 1)
            code[index] = 0xd000 | (word << 8) | ((address - ((pc & ~3) + 4)) // 4)
    return struct.pack(f'<{len(code)}H3I', *code, *literals)


@pytest.mark.parametrize('clear_first', [True, False],
                         ids=['clear-then-stop', 'stop-then-clear'])
def test_tmu_handler_that_stops_its_channel_takes_one_interrupt(clear_first):
    """qemu-sh-tmu-catch-up.patch: underflows kept for a handler that then
    stops its channel bring no interrupt after the stop, as on the chip."""
    with machine(_priority_rom(), icount=True) as (request, execute):
        handler = _stopping_handler(clear_first)
        request(f'write 0x04000600 {len(handler)} 0x{handler.hex()}')
        request('writel 0xffd40000 0x04000000')  # TMU0 level 4
        request('writel 0xffd80008 13499')  # 1 ms
        request('writel 0xffd8000c 13499')
        request('writew 0xffd80010 0x20')  # TCR.UNIE
        request('writeb 0xffd80004 1')
        execute('cont')

        # Three underflows while the guest runs with SR.BL set: the first
        # sets UNF, the catch-up keeps the other two.
        wraps, count = 0, request('readl 0xffd8000c')
        deadline = time.monotonic() + 5
        while wraps < 3:
            last, count = count, request('readl 0xffd8000c')
            wraps += count > last
            if time.monotonic() > deadline:
                pytest.fail('TMU0 did not underflow three times')

        request('writel 0x04003000 1')  # the guest clears SR.BL
        deadline = time.monotonic() + 5
        while not request('readl 0x04002000'):
            if time.monotonic() > deadline:
                pytest.fail('the guest took no TMU0 interrupt')
            time.sleep(.01)
        time.sleep(.1)  # any further interrupt comes at once
        assert request('readl 0x04002000') == 1
        assert not request('readw 0xffd80010') & 0x100


def test_intc_selects_higher_priority_pending_tmu_vector():
    with machine(_priority_rom()) as (request, execute):
        # The handler records INTEVT then loops with SR.BL set. Thus the result
        # is the first accepted interrupt even while both timer lines stay high.
        handler = struct.pack('<16H2I', 0xd107, 0x6212, 0xd007, 0x2022,
                              0xaffe, *([0x0009] * 11),
                              0xff000028, 0x04002000)
        request(f'write 0x04000600 {len(handler)} 0x{handler.hex()}')
        request('writel 0xffd40000 0x04080000')  # TMU0 level 4, TMU1 level 8
        for tcor, tcnt, tcr in ((0xffd80008, 0xffd8000c, 0xffd80010),
                                (0xffd80014, 0xffd80018, 0xffd8001c)):
            request(f'writel {tcor:#x} 2')
            request(f'writel {tcnt:#x} 2')
            request(f'writew {tcr:#x} 0x20')
        request('writeb 0xffd80004 3')
        execute('cont')

        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if (request('readw 0xffd80010') & 0x100 and
                    request('readw 0xffd8001c') & 0x100):
                break
            time.sleep(.01)
        else:
            pytest.fail('both TMU lines did not become pending')

        request('writel 0x04003000 1')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            vector = request('readl 0x04002000')
            if vector:
                break
            time.sleep(.01)
        else:
            pytest.fail('the guest did not accept a TMU interrupt')
        assert vector == 0x5a0  # TMU1, despite its later source order
