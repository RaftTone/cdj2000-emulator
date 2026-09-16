"""The media-mode flag byte must be read little-endian, as the SH4 here is.

Reading it big-endian reported 0x00 for a whole run while bit 1 was set, which
cost NXS_SD_READINESS.md a retraction that was itself wrong.
"""
from tools.cdj_main.sd_readiness import byte_of


def test_byte_of_is_little_endian():
    word = 0x44332211                      # bytes 11 22 33 44 in memory order
    assert [byte_of(word, 0x1000 + i) for i in range(4)] == [0x11, 0x22, 0x33, 0x44]


def test_measured_media_mode_flag():
    # Measured with tools.cdj_main.gdbprobe: the word at 0x051e21d0 is 0x00000002,
    # so the flag byte is 0x02 and bit 1 - the media-mode bit - is set.
    assert byte_of(0x00000002, 0x051E21D0) == 0x02
