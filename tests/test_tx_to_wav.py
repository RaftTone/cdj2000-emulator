import json
import struct
import wave

import pytest

from tools.cdj_dsp.tx_capture import tx_capture_metadata
from tools.cdj_dsp.tx_to_wav import export_wav


def capture(path, words):
    with path.open('w') as stream:
        for number, (sequence, slot, word) in enumerate(words, 1):
            event = {'sequence': number, 'instance': 1, 'slot': slot,
                     'serializer': 0, 'word': word, 'xbuf_sequence': sequence,
                     'packets': number * 1024, 'cycles': number * 2048,
                     'source': 'genuine_xbuf',
                     'clock': 'functional-coarse-packet-slot'}
            stream.write(json.dumps(event) + '\n')


def test_export_reconstructs_missing_zero_words_and_stereo_order(tmp_path):
    source = tmp_path / 'capture.jsonl'
    output = tmp_path / 'audio.wav'
    capture(source, [(10, 0, 0x00800000), (11, 1, 0xff800000),
                     (14, 0, 0x01000000), (15, 1, 0x02000000)])
    result = export_wav(source, output, 44100)
    assert result['frames'] == 3
    assert result['zero_words_reconstructed'] == 2
    with wave.open(str(output), 'rb') as wav:
        assert wav.getparams()[:4] == (2, 2, 44100, 3)
        assert wav.readframes(3) == struct.pack('<hhhhhh', 128, -128, 0, 0, 256, 512)


@pytest.mark.parametrize('clock, timing', [
    ('functional-coarse-packet-slot', 'coarse packet slots'),
    ('virtual-clock-batch', 'virtual-time slot batches'),
    ('dsp-thread-virtual', "DSP thread's virtual clock"),      # --audio-clock virtual
    ('dsp-sysclk1-cycle', 'SYSCLK1 cycles'),                   # --dsp-cycle-mcasp-clock
])
def test_metadata_accepts_every_clock_the_board_names(tmp_path, clock, timing):
    source = tmp_path / 'capture.jsonl'
    capture(source, [(10, 0, 0x00800000), (11, 1, 0xff800000)])
    source.write_text(source.read_text().replace('functional-coarse-packet-slot', clock))
    metadata = tx_capture_metadata(source)
    assert metadata['records'] == 2 and timing in metadata['timing']
    source.write_text(source.read_text().replace(clock, 'some-other-clock'))
    with pytest.raises(ValueError, match='invalid DSP transmit capture record 1'):
        tx_capture_metadata(source)


def test_export_rejects_slot_discontinuity_without_leaving_output(tmp_path):
    source = tmp_path / 'capture.jsonl'
    output = tmp_path / 'audio.wav'
    capture(source, [(10, 0, 0x00800000), (11, 0, 0x00800000)])
    with pytest.raises(ValueError, match='slot and XBUF order'):
        export_wav(source, output, 44100)
    assert not output.exists()
