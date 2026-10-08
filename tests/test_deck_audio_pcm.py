# SPDX-License-Identifier: GPL-2.0-or-later
"""A WAV record in the DSP stream dump: blocks of 2352 frames, each followed by a 64-byte tail whose first word is
4 * the block's number in the file; the deck streams the first blocks at the load and again from block 0 at PLAY."""
import numpy as np

from tools.cdj_main import deck_audio as da

BLOCK = da.PCM_FRAMES * 4      # 16 bit stereo


def chunk(offset, body, record=1):
    head = b"CDJS" + bytes(8) + offset.to_bytes(4, "little") + len(body).to_bytes(4, "little") \
        + record.to_bytes(4, "little") + bytes(8)
    return head + body


def dump(blocks, numbers):
    out = b""
    for n, (number, body) in enumerate(zip(numbers, blocks)):
        out += chunk(da.DATA_OFFSETS[n % 2], body)
        out += chunk(0xBDE0, (4 * number).to_bytes(4, "little") + bytes(60))
    return out


def test_blocks_are_placed_by_their_number_and_a_restart_does_not_shift_them(tmp_path):
    file_blocks = [bytes([n + 1]) * BLOCK for n in range(6)]
    # blocks 0..3 at the load, then everything again from 0 at PLAY
    order = [0, 1, 2, 3, 0, 1, 2, 3, 4, 5]
    path = tmp_path / "stream.bin"
    path.write_bytes(dump([file_blocks[n] for n in order], order))
    assert da.records(path, BLOCK)[1] == b"".join(file_blocks)


def test_a_missing_block_is_silence(tmp_path):
    path = tmp_path / "stream.bin"
    path.write_bytes(dump([b"\x07" * BLOCK, b"\x09" * BLOCK], [0, 2]))
    assert da.records(path, BLOCK)[1] == b"\x07" * BLOCK + bytes(BLOCK) + b"\x09" * BLOCK


def test_pcm_decoding_16_and_24_bit():
    sixteen = np.array([[0, 16384], [-32768, 32767]], dtype="<i2").tobytes()
    assert da.decode_pcm(sixteen, 16)[0, 1] == 0.5
    left, right = (-(1 << 23)) & 0xFFFFFF, (1 << 22)
    raw = b"".join(v.to_bytes(3, "little") for v in (left, right))
    assert list(da.decode_pcm(raw, 24)[0]) == [-1.0, 0.5]
