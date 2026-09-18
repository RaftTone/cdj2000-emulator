"""Read-only, exact-image-gated DHCP/AutoIP snapshot through the QEMU monitor.

Temporarily pauses MAIN, saves RAM and resumes in finally. This observer
perturbs host timing; it neither changes guest RAM nor establishes wire timing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

from tools.cdj_main.qmp import connect_chardev, parse_endpoint

MAIN_SHA = 'd88369e4b1986a9d3dcd58b68b784968ff1a756b7e59637c71dc13e4f9d891fe'
REGIONS = {'autoip': (0x045a6360, 0x20), 'dhcp': (0x046313b4, 0x70),
           'kernel': (0x04d12b48, 0xc00), 'dhcp_task': (0x04d16128, 0x100),
           'dhcp_stack': (0x04631708, 0x400), 'interface': (0x046329ec, 0x4c)}


def capture(run, tag):
    run = Path(run).resolve()
    if not tag.isalnum():
        raise ValueError('capture tag must be alphanumeric')
    manifest = json.loads((run / 'run.json').read_text())
    if manifest['input_artifacts']['main_firmware']['sha256'] != MAIN_SHA:
        raise ValueError('addresses require the exact Dante stereo MAIN image')
    paths = {name: run / f'network-{tag}-{name}.bin' for name in REGIONS}
    report = run / f'network-{tag}.json'
    if report.exists() or any(p.exists() for p in paths.values()):
        raise FileExistsError('capture already exists')
    if any('"' in str(p) or '\n' in str(p) for p in paths.values()):
        raise ValueError('unsafe monitor path')
    monitor = manifest.get('qemu_sync_profile', {}).get('monitor') or 'qemu-monitor.sock'
    with connect_chardev(parse_endpoint(monitor, relative_to=run), timeout=10) as sock:

        def receive():
            data = b''
            while b'(qemu)' not in data:
                chunk = sock.recv(65536)
                if not chunk:
                    raise ConnectionError('monitor closed')
                data += chunk
            return data.decode(errors='replace')

        def command(line):
            sock.sendall((line + '\n').encode())
            return receive()

        receive()
        started = time.time()
        pause_start = time.monotonic()
        try:
            command('stop')
            for name, (address, length) in REGIONS.items():
                command(f'pmemsave {address:#x} {length:#x} "{paths[name]}"')
        finally:
            command('cont')
        pause = time.monotonic() - pause_start
    regions = {}
    buffers = {}
    for name, (address, length) in REGIONS.items():
        data = paths[name].read_bytes()
        if len(data) != length:
            raise ValueError('incomplete RAM capture')
        buffers[name] = data
        regions[name] = dict(address=hex(address), hex=data.hex(),
                             sha256=hashlib.sha256(data).hexdigest())
    kernel = buffers['kernel']
    dhcp = buffers['dhcp']
    task = buffers['dhcp_task']
    u32 = lambda data, offset: int.from_bytes(data[offset:offset + 4], 'little')
    task_id = u32(dhcp, 0x5c)
    pointer_matches = (0 < task_id < 256 and
                       u32(kernel, task_id * 4) == REGIONS['dhcp_task'][0])
    decoded = dict(dhcp_task_id=task_id, dhcp_task_live=dhcp[0x58],
                   interface_ipv4='.'.join(str(b) for b in buffers['interface'][0x20:0x24]),
                   dhcp_allocated=dhcp[0x14], dhcp_request=dhcp[0x15],
                   dhcp_state=dhcp[0x24], dhcp_timer_last_ms=u32(dhcp, 0),
                   kernel_tick=u32(kernel, 0xb48),
                   task_pointer_matches=pointer_matches,
                   task_saved_sp=hex(u32(task, 0)) if pointer_matches else None,
                   task_deadline=u32(task, 12) if pointer_matches else None)
    result = dict(capture_unix=started, observer_pause_seconds=pause,
                  main_sha256=MAIN_SHA, regions=regions,
                  decoded=decoded,
                  scope='read-only RAM; not network timing or audio evidence')
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('tag')
    args = parser.parse_args()
    capture(args.run, args.tag)
