import json
import mmap
from pathlib import Path
import struct
import wave

import pytest

from tools.cdj_main import test_media


@pytest.mark.parametrize('extra', [
    ['--sd-insert-seconds', '10'],
    ['--sd', 'unused.img', '--sd-insert-seconds', '-1'],
    ['--sd', 'unused.img', '--sd-insert-seconds', '86401'],
])
def test_invalid_insert_schedule_fails_before_launch(monkeypatch, extra):
    from tools.cdj_main import nxs_vm
    monkeypatch.setattr(nxs_vm.sys, 'argv', ['nxs_vm', 'unused-run', *extra])
    def unexpected_launch(*args, **kwargs):
        pytest.fail('invalid arguments must not launch an emulator')
    monkeypatch.setattr(nxs_vm.subprocess, 'Popen', unexpected_launch)
    with pytest.raises(SystemExit) as error:
        nxs_vm.main()
    assert error.value.code == 2


def test_generated_wav_is_stereo_pcm_and_low_level(tmp_path):
    track = tmp_path / 'test.wav'
    test_media.write_track(track)
    with wave.open(str(track)) as wav:
        assert wav.getparams()[:4] == (2, 2, 44100, 441000)
        frames = list(struct.iter_unpack('<hh', wav.readframes(wav.getnframes())))
    assert all(left == right for left, right in frames)
    assert max(abs(left) for left, _ in frames) == test_media.AMPLITUDE
    assert sum(left for left, _ in frames) == 0
    with pytest.raises(FileExistsError):
        test_media.write_track(track)


def test_fat32_fixture_contains_the_exact_wav(tmp_path):
    directory = tmp_path / 'media'
    manifest = test_media.create(directory)
    with (directory / manifest['image']).open('rb') as file:
        with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as image:
            assert len(image) == test_media.IMAGE_BYTES
            assert image[510:512] == b'\x55\xaa'
            assert image[450] == 0x0c
            base = struct.unpack_from('<I', image, 454)[0] * 512
            assert base == 2048 * 512
            assert image[base + 82:base + 90] == b'FAT32   '
            reserved = struct.unpack_from('<H', image, base + 14)[0]
            fats, cluster_sectors = image[base + 16], image[base + 13]
            fat_sectors = struct.unpack_from('<I', image, base + 36)[0]
            data = base + (reserved + fats * fat_sectors) * 512
            assert (len(image) - data) // (cluster_sectors * 512) >= 65525
            assert image[data:data + 11] == b'TESTTONEWAV'
            cluster = struct.unpack_from('<H', image, data + 26)[0]
            size = struct.unpack_from('<I', image, data + 28)[0]
            offset = data + (cluster - 2) * cluster_sectors * 512
            assert image[offset:offset + size] == (directory / manifest['track']).read_bytes()
    assert json.loads((directory / 'manifest.json').read_text()) == manifest
    with pytest.raises(FileExistsError):
        test_media.create(directory)


def test_mounts_use_temporary_overlays_and_escape_commas(tmp_path):
    image = tmp_path / 'my, test.img'
    original = b'\0' * 1024
    image.write_bytes(original)
    command, inputs = test_media.media_drives(image, image)
    drives = [command[i + 1] for i, value in enumerate(command) if value == '-drive']
    assert len(drives) == 2
    assert all('snapshot=on' in value and 'format=raw' in value for value in drives)
    assert all('my,, test.img' in value for value in drives)
    assert 'if=sd' in drives[0]
    assert command[-1] == 'usb-storage,drive=cdj-usb-media,removable=on'
    assert inputs == {'sd_image': image, 'usb_image': image}
    assert image.read_bytes() == original
    assert test_media.media_drives(None, None) == ([], {})


@pytest.mark.parametrize('size', [0, 513, 1536])
def test_invalid_sd_image_sizes_are_rejected(tmp_path, size):
    image = tmp_path / 'bad.img'
    image.write_bytes(bytes(size))
    with pytest.raises(ValueError):
        test_media.media_drives(image, None)


def test_missing_or_directory_images_are_rejected(tmp_path):
    with pytest.raises(FileNotFoundError):
        test_media.media_drives(None, tmp_path / 'absent.img')
    with pytest.raises(ValueError):
        test_media.media_drives(None, tmp_path)


def test_fsinfo_records_the_real_free_count(tmp_path):
    """An unknown free count makes a driver read every FAT sector to count it,
    which on these images is 1,023 sectors before any file is touched."""
    from tools.cdj_main import make_sd_image

    source = tmp_path / 'tracks'
    source.mkdir()
    (source / 'TRACK.WAV').write_bytes(b'\0' * 100000)
    image = tmp_path / 'card.img'
    monkeypatch = None
    import sys
    argv = sys.argv
    sys.argv = ['make_sd_image', str(source), str(image), '--size', '64M']
    try:
        assert make_sd_image.main() == 0
    finally:
        sys.argv = argv

    data = image.read_bytes()
    lba = struct.unpack_from('<I', data, 446 + 8)[0]
    boot = lba * 512
    sectors_per_cluster = data[boot + 13]
    reserved = struct.unpack_from('<H', data, boot + 14)[0]
    fat_sectors = struct.unpack_from('<I', data, boot + 36)[0]
    total = struct.unpack_from('<I', data, boot + 32)[0]
    clusters = (total - reserved - 2 * fat_sectors) // sectors_per_cluster

    fsinfo = boot + 512
    assert data[fsinfo:fsinfo + 4] == b'RRaA'
    assert data[fsinfo + 484:fsinfo + 488] == b'rrAa'
    free, next_free = struct.unpack_from('<II', data, fsinfo + 488)
    assert free != 0xFFFFFFFF

    fat = boot + reserved * 512
    used = sum(1 for c in range(2, clusters + 2)
               if struct.unpack_from('<I', data, fat + 4 * c)[0])
    assert free == clusters - used
    assert next_free == used + 2
