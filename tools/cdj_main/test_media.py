"""Create a deterministic, low-level test tone on a FAT32 USB/SD image.

    python -m tools.cdj_main.test_media runs/test-media

Creates a new directory; never overwrites an existing image. This supplies
ordinary WAV/FAT32 bytes, not a rekordbox database or fabricated load response.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import wave

from tools.cdj_main.make_sd_image import Builder

IMAGE_BYTES = 512 * 1024 * 1024
RATE = 44100
SECONDS = 10
FREQUENCY = 440
AMPLITUDE = 2048


def write_track(path: Path) -> None:
    with path.open('xb') as output, wave.open(output, 'wb') as wav:
        wav.setparams((2, 2, RATE, RATE * SECONDS, 'NONE', 'not compressed'))
        # One second contains an integral number of periods; repeat identically.
        second = b''.join(struct.pack('<hh', sample, sample)
                         for i in range(RATE)
                         for sample in [round(AMPLITUDE * math.sin(
                             2 * math.pi * FREQUENCY * i / RATE))])
        for _ in range(SECONDS):
            wav.writeframesraw(second)


def create(directory: Path) -> dict:
    directory.mkdir(parents=True, exist_ok=False)
    tracks = directory / 'tracks'
    tracks.mkdir()
    track = tracks / 'TESTTONE.WAV'
    write_track(track)
    builder = Builder(IMAGE_BYTES)
    root = builder.alloc(1)[0]
    builder.add_dir(tracks, root, 0, is_root=True)
    builder.finish('CDJTEST')
    image = directory / 'test-track.img'
    with image.open('xb') as output:
        output.write(builder.image)
    manifest = dict(image=image.name, bytes=IMAGE_BYTES,
                    sha256=hashlib.sha256(builder.image).hexdigest(),
                    track='tracks/TESTTONE.WAV',
                    track_sha256=hashlib.sha256(track.read_bytes()).hexdigest(),
                    format='PCM WAV, stereo, signed 16-bit little-endian',
                    sample_rate=RATE, seconds=SECONDS, frequency_hz=FREQUENCY,
                    peak_amplitude=AMPLITUDE,
                    purpose='Synthetic test signal; no copyrighted recording',
                    validation='Fixture only; firmware load and audio unverified')
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def media_drives(sd: Path | None, usb: Path | None,
                 disc: Path | None = None) -> tuple[list[str], dict[str, Path]]:
    """Attach writable media through overlays and an ISO through the read-only CD backend."""
    command: list[str] = []
    inputs: dict[str, Path] = {}
    for kind, path in [('sd', sd), ('usb', usb)]:
        if path is None:
            continue
        path = path.resolve(strict=True)
        if not path.is_file():
            raise ValueError(f'{kind} image must be a regular file: {path}')
        size = path.stat().st_size
        if not size or size % 512:
            raise ValueError(f'{kind} image must contain complete 512-byte sectors')
        if kind == 'sd' and size & (size - 1):
            raise ValueError('SD image size must be a power of two')
        inputs[f'{kind}_image'] = path
        # QEMU's keyval parser escapes a literal comma by doubling it.
        filename = str(path).replace(',', ',,')
        bus = 'if=sd' if kind == 'sd' else 'if=none,id=cdj-usb-media'
        command += ['-drive', f'{bus},format=raw,snapshot=on,file={filename}']
        if kind == 'usb':
            command += ['-device', 'usb-storage,drive=cdj-usb-media,removable=on']
    if disc is not None:
        disc = disc.resolve(strict=True)
        if not disc.is_file():
            raise ValueError(f'disc image must be a regular file: {disc}')
        if not disc.stat().st_size or disc.stat().st_size % 2048:
            raise ValueError('disc image must contain complete 2048-byte ISO sectors')
        inputs['disc_image'] = disc
        filename = str(disc).replace(',', ',,')
        command += [
            '-drive',
            f'if=ide,media=cdrom,bus=0,unit=0,format=raw,file={filename}',
        ]
    return command, inputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='new output directory')
    args = parser.parse_args()
    try:
        manifest = create(args.directory)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
