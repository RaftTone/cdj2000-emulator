"""Fixed-tick Blackfin firmware benchmark using an isolated packet replay.

This uses the simulator's existing diagnostic packet/housekeeping mode, not a
connected boot. Supply a recorded SPRX link dump and local firmware. Outputs
stay outside source control; record hashes, counters and timings for comparison.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--link-dump', type=Path, required=True)
    parser.add_argument('--board', type=Path, default=Path('emulator/cdj2000-gui-nxs.hw'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ticks', type=int, default=300000000)
    parser.add_argument('--trials', type=int, default=3)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    data = args.link_dump.read_bytes()
    packets = bytearray()
    offset = 0
    while offset < len(data):
        if data[offset:offset+4] != b'SPRX' or offset + 8 > len(data):
            raise ValueError('invalid SPRX capture')
        size = int.from_bytes(data[offset+4:offset+8], 'little')
        if offset + 8 + size > len(data):
            raise ValueError('truncated SPRX capture')
        packets.extend(data[offset+4:offset+8+size])
        offset += 8 + size
    packet_path = output / 'packets.bin'
    packet_path.write_bytes(packets)
    rows = []
    for trial in range(args.trials):
        frame, tx = output / f'{trial}.ppm', output / f'{trial}.tx'
        env = {k: v for k, v in os.environ.items() if not k.startswith('BFIN_')}
        env.update(BFIN_TIME_BASE='insn', BFIN_EXIT_AFTER_TICKS=str(args.ticks),
                   BFIN_STATS='1000', BFIN_PARALLEL_WRITEBACK='1',
                   BFIN_GUI_COLOR='rgb555le', BFIN_GUI_OUTPUT=str(frame),
                   BFIN_GPIO5_READY_TOGGLE='1', BFIN_SPORT_RX_RECORDS='1',
                   BFIN_SPORT_RX_ZERO_200='1', BFIN_SPORT_RX_INPUT=str(packet_path),
                   BFIN_SPORT_TX_OUTPUT=str(tx))
        command = [str(args.binary.resolve()), '--model', 'bf531',
                   '--environment', 'operating', '--memory-region', '0,64M',
                   '--hw-board-file', str(args.board.resolve()), str(args.elf.resolve())]
        started = time.perf_counter()
        result = subprocess.run(command, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        elapsed = time.perf_counter() - started
        log = result.stdout.decode(errors='replace')
        (output / f'{trial}.log').write_text(log)
        match = re.search(r'STATS exit at (\d+) ticks after (\d+) insns', log)
        if result.returncode or not match:
            raise RuntimeError(log[-3000:])
        rows.append(dict(trial=trial, seconds=elapsed, ticks=int(match[1]),
                         instructions=int(match[2]), frame_sha256=digest(frame),
                         tx_sha256=digest(tx)))
    report = dict(binary_sha256=digest(args.binary), elf_sha256=digest(args.elf),
                  packet_sha256=digest(packet_path), board_sha256=digest(args.board),
                  time_base='insn', requested_ticks=args.ticks, trials=rows)
    (output / 'measurements.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
