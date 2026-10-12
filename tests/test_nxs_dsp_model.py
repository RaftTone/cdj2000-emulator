"""CDJ_NXS_DSP_MODEL=1: the behavioural DSP answers MAIN's host port as the
stock DSP does in runs/fork-stock-obey-1, without executing any C674x code."""
import contextlib
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


@contextlib.contextmanager
def model_board():
    """A model board under qtest, booted to the runtime (phase 3)."""
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

                class Board:
                    def command(self, text):
                        stream.write((text + '\n').encode())
                        response = stream.readline().decode().strip()
                        assert response.startswith('OK'), response
                        return response.split()[1:]

                    def write(self, address, value): self.command(f'writel {address:#x} {value:#x}')
                    def read(self, address): return int(self.command(f'readl {address:#x}')[0], 0)
                    def put(self, dsp, value): self.write(ADDRESS, dsp); self.write(FIXED, value)
                    def get(self, dsp): self.write(ADDRESS, dsp); return self.read(FIXED)
                    def hint(self): return not int(self.command('readw 0xfff10040')[0], 0) & 0x10
                    def phase(self, value): self.command(f'writew 0xfff1005c {value:#x}')
                    def dspint(self): self.write(CONTROL, 0x014b014b)
                    def step(self, ns): self.command(f'clock_step {ns}')

                board = Board()
                board.command('writew 0xfff10054 0x40')        # release DSP reset
                board.write(CONTROL, 0x01050105)                # HWOB, ack ROM HINT
                assert not board.hint()
                board.put(0x11800000, 0x11802000)               # boot entry word
                board.dspint()                                  # start
                assert not board.read(CONTROL) & 2, 'stage 1 acks DSPINT'
                assert board.get(0x1183fff4) == 1
                board.phase(2)
                assert board.hint()
                board.write(CONTROL, 0x014d014d)
                assert not board.hint()
                board.phase(0)
                board.put(0x11837bc0, 0xdeadbeef)               # second-record image
                board.phase(3)
                assert board.hint()
                board.write(CONTROL, 0x014d014d)
                board.phase(0)
                yield board
                stream.close()
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            process.stderr.close()


def position(board):
    """MAIN's position in half frames (1/150 s): 2 x frame + low half / 294."""
    return 2 * board.get(0x11837c10) + (board.get(0x11837bf4) & 0xffff) // 294


def stream(board, record, frames, first=True):
    """A track stream's command and first header, or a later header."""
    if first:
        board.put(0x11838120, record)
        board.put(0x1183811c, 0x7e93)
        board.put(0x11838100, 3)
        board.dspint()
    board.put(0x11838144, frames)
    board.put(0x11838140, 0x01010100 if first else 0x01020100)
    board.dspint()


def test_model_boot_handshake_and_mailboxes():
    with model_board() as board:
        put, get = board.put, board.get
        assert get(0x11837bc0) == 0
        assert get(0x11837bf8) == 1 and get(0x11837cc8) == 0x1a24
        for dsp, value in ((0x11837c9c, 0x1c), (0x11837ba0, 1), (0x11837cb0, 1),
                           (0x11838140, 0x01010100), (0x11838100, 3)):
            put(dsp, value)
        put(0x11837bd0, 0x3c)                          # MAIN-owned rate block
        board.dspint()
        assert not board.read(CONTROL) & 2
        for dsp in (0x11837c9c, 0x11837ba0, 0x11837cb0, 0x11838140, 0x11838100):
            assert get(dsp) == 0, hex(dsp)
        assert get(0x11837bd0) == 0x3c
        put(0x11838144, 0x28)                          # 40 frames
        put(0x11838140, 0x01010100)
        put(0x11838100, 7)                             # unaccepted command
        put(0x118381c4, 1)
        board.dspint()
        assert get(0x11838100) == 7 and get(0x118381c4) == 0
        assert get(0x11838140) == 0
        assert get(0x11837cd0) == 40 and get(0x11837cc8) == 0x1a24 - 40
        # Play at 1.0: the position follows virtual time, ahead
        # frames become behind ones, and it stops at the buffer end.
        put(0x11837bc0, 0x100000)
        put(0x11837ba0, 2)
        board.dspint()
        assert get(0x11837ba0) == 0 and get(0x11837bf8) == 2
        board.step(200000000)                          # 0.2 s = 15 frames
        board.dspint()
        assert get(0x11837c10) == 15 and get(0x11837c50) == 15
        assert get(0x11837cd0) == 25 and get(0x11837ccc) == 15
        assert position(board) == 30                   # 0.2 s in MAIN's 1/150 s
        sub = get(0x11837bf4)
        assert sub >> 16 == sub & 0xffff < 588
        board.step(1000000000)                         # past 40 frames
        board.dspint()
        assert get(0x11837c10) == 40 and get(0x11837cd0) == 0
        put(0x11837ba0, 4)                             # cue: stops
        board.dspint()
        board.step(200000000)
        board.dspint()
        assert get(0x11837bf8) == 4 and get(0x11837c10) == 40


def test_model_transport_commands():
    """Buffer pool, stream continuation, step, search, re-sync, command words."""
    with model_board() as board:
        put, get = board.put, board.get
        put(0x11837bc0, 0x100000)
        stream(board, record=1, frames=0x28)
        assert get(0x118381a4) == 1 and get(0x11837cd0) == 0x28
        # The same record re-issued continues; the position keeps running.
        put(0x11837ba0, 2)
        board.dspint()
        board.step(100000000)                          # 7.5 frames
        board.dspint()
        assert get(0x11837c10) == 7
        stream(board, record=1, frames=0x28)
        assert get(0x11837c10) == 7 and get(0x11837cd0) == 0x28 - 7 + 0x28
        # ahead + behind + free stays 0x1a24; a full pool takes behind frames.
        level = lambda: get(0x11837cd0) + get(0x11837ccc) + get(0x11837cc8)
        assert level() == 0x1a24
        put(0x11837ba0, 3)                             # pause: holds
        board.dspint()
        for _ in range(0x1a24 // 0x28):
            stream(board, record=1, frames=0x28, first=False)
        assert get(0x11837cc8) == 0 and level() == 0x1a24 and get(0x11837ccc) < 7
        # Step: 0x11837ba4 = 1, +4 signed half frames, cleared when taken.
        before = position(board)
        put(0x11837ba8, 0x10)
        put(0x11837ba4, 1)
        board.dspint()
        assert get(0x11837ba4) == 0 and position(board) == before + 0x10
        put(0x11837ba8, (-6) & 0xffffffff)
        put(0x11837ba4, 1)
        board.dspint()
        assert position(board) == before + 0x10 - 6
        # Search: REQ 5 after command 2 (direction) and 3 (multiple, 8).
        for code, value in ((2, 0), (3, 8)):
            put(0x11837ba8, value)
            put(0x11837ba4, code)
            board.dspint()
            assert get(0x11837ba4) == 0
        put(0x11837ba0, 5)
        board.dspint()
        start = position(board)
        board.step(100000000)
        board.dspint()
        assert position(board) - start == 8 * 15       # 0.1 s at 8x
        put(0x11837ba8, 1)                             # back
        put(0x11837ba4, 2)
        board.dspint()
        board.step(100000000)
        board.dspint()
        assert abs(position(board) - start) <= 1
        # The third command word (0x21 cue play) is taken within a step.
        put(0x11837c80, 0x21)
        board.dspint()
        assert get(0x11837c80) == 0
        # 0x11837cb0 = 2 (re-sync): position, buffers and status words restart.
        put(0x11837cb0, 2)
        board.dspint()
        assert get(0x11837cb0) == 0
        assert get(0x11837c10) == 0 and position(board) == 0
        assert get(0x11837cd0) == get(0x11837ccc) == 0 and get(0x11837cc8) == 0x1a24
        assert get(0x118381a4) == get(0x11838184) == 0
        stream(board, record=1, frames=0x28)           # binds afresh
        assert get(0x118381a4) == 1 and get(0x11837cd0) == 0x28


def test_model_continuous_play_switches_record_at_the_boundary():
    """The next track's stream is appended without a reset: the receive block
    names it at once, the position blocks when its frames start playing."""
    with model_board() as board:
        put, get = board.put, board.get
        put(0x11837bc0, 0x100000)
        stream(board, record=1, frames=0x28)
        put(0x11837ba0, 2)
        board.dspint()
        board.step(200000000)                          # 15 of 40 frames
        board.dspint()
        stream(board, record=2, frames=0x28)
        assert get(0x118381a4) == 2 and get(0x11837c14) == 1
        assert get(0x11837c10) == 15
        board.step(400000000)                          # 30 more: 5 into record 2
        board.dspint()
        assert get(0x11837c14) == get(0x11837c34) == 2
        assert get(0x11837c10) == 5


def test_model_track_plays_to_its_length():
    """The deck's stream keeps no frame past its length (+0x1c): the last
    0x28 unit is cut so the received word reads length - 1 (runs/el-real-4,
    Obey: 0x7e75 + 0x28 -> 0x7e92 for 0x7e93), and playback stops there."""
    with model_board() as board:
        put, get = board.put, board.get
        put(0x11837bc0, 0x100000)
        put(0x11838120, 1)
        put(0x1183811c, 0x30)                          # 48 frames long
        put(0x11838100, 3)
        board.dspint()
        put(0x11838144, 0x28)
        put(0x11838140, 0x01010100)
        board.dspint()
        for _ in range(2):                             # the second unit is cut,
            put(0x11838140, 0x01020100)                # the third adds nothing
            board.dspint()
        assert get(0x118381a0) == 0x30 - 1 and get(0x11837cd0) == 0x30
        assert get(0x11837cd0) + get(0x11837ccc) + get(0x11837cc8) == 0x1a24
        put(0x11837ba0, 2)
        board.dspint()
        board.step(1000000000)                         # 75 frames of time
        board.dspint()
        assert get(0x11837c10) == 0x30 and get(0x11837cd0) == 0
        assert get(0x11837cd0) + get(0x11837ccc) + get(0x11837cc8) == 0x1a24


def test_model_end_of_file_flag_is_a_status_write_not_frames():
    """Header 0x3000100 (MAIN's sub_041c8b90 at reader EOF) copies
    0x11838144..0x11838150 into the deck's status block when 0x11838168
    names its stream; it adds no frames. Another stream's write is dropped."""
    with model_board() as board:
        put, get = board.put, board.get
        stream(board, record=1, frames=0x28)
        ahead, received = get(0x11837cd0), get(0x118381a0)
        for record, flagged in ((2, False), (1, True)):
            put(0x11838168, record)
            put(0x11838144, received)
            put(0x11838148, 1)
            put(0x1183814c, get(0x118381a8))
            put(0x11838150, get(0x118381ac) | 0x1000000)
            put(0x11838140, 0x03000100)
            board.dspint()
            assert get(0x11838140) == 0
            assert get(0x11837cd0) == ahead and get(0x118381a0) == received
            assert bool(get(0x118381ac) & 0x1000000) == flagged


def test_model_coded_header_waits_for_its_bytes():
    """A coded stream's header stays pending until MAIN has delivered the
    file chunks its frames take at the stream's mean rate (+0x18 bytes over
    +0x1c frames; 2.77 chunks per 0x28 frames for Obey, runs/el-real-8).
    Each chunk is 0x118381c4 = 1, cleared when taken; 2 is the file's last
    chunk and completes the header at once."""
    with model_board() as board:
        put, get = board.put, board.get
        put(0x11838120, 1)
        put(0x1183811c, 0x50)                          # 80 frames
        put(0x11838118, 0x50 * 0x300)                  # 0x300 bytes a frame
        put(0x11838100, 3)
        board.dspint()
        put(0x11838144, 0x28)
        put(0x11838140, 0x01010100)                    # binds: taken at once
        board.dspint()
        assert get(0x11838140) == 0 and get(0x118381a0) == 0x27
        put(0x11838140, 0x01020100)                    # 0x50 frames: 0xf000 bytes
        for chunk in range(8):
            assert get(0x11838140) == 0x01020100, chunk
            put(0x118381c4, 1)
            board.dspint()
            assert get(0x118381c4) == 0
        assert get(0x11838140) == 0 and get(0x118381a0) == 0x4f
        put(0x11838120, 1)                             # re-issued: counts afresh
        put(0x1183811c, 0xa0)
        put(0x11838118, 0xa0 * 0x300)
        put(0x11838100, 3)
        board.dspint()
        put(0x11838140, 0x01010100)
        board.dspint()
        put(0x11838140, 0x01020100)
        board.dspint()
        assert get(0x11838140) == 0x01020100
        put(0x118381c4, 2)                             # the last chunk
        board.dspint()
        assert get(0x11838140) == 0
