"""Inspect the known NXS GuiCom/DbCli layout in a stopped MAIN RAM dump.

This is a firmware-specific diagnostic, not a boot-success oracle. Addresses
come from the research NXS MAIN image (see NXS_GUI_STALL.md).
Capture with QMP stop, then HMP `pmemsave 0x04000000 0x08000000 ram.bin`.
No guest memory is modified. A 64 MiB dump can inspect control structures but
cannot inspect the DbCli response buffers above 0x08000000.
"""
from __future__ import annotations

import argparse
import json
import mmap
from pathlib import Path
import struct

RAM_BASE = 0x04000000
TASK_TABLE = 0x04D12B48
POOL_TABLE = 0x04D133DC
MAILBOX_TABLE = 0x04D1326C
RECEIVER_READY = 0x04984A68
SENDER_SPECIAL = 0x04985404
POOL_LAYOUT = {52: (8, 4096), 60: (35, 2160)}
TASK_NAMES = {85: 'DbCli_TASK', 102: 'GuiCom_RcvTASK', 103: 'GuiCom_SndTASK'}


class LayoutError(ValueError):
    pass


class Memory:
    def __init__(self, data, base=RAM_BASE):
        self.data, self.base = data, base

    def read(self, address, size):
        # SH-4 P1/P2 aliases of physical SDRAM. Do not mask arbitrary addresses.
        physical = address & 0x1FFFFFFF if address >> 29 in (4, 5) else address
        offset = physical - self.base
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise LayoutError(f'address {address:#010x}, size {size}, outside dump')
        return self.data[offset:offset + size]

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]

    def u16(self, address):
        return struct.unpack('<H', self.read(address, 2))[0]


def inspect(memory: Memory) -> dict:
    """Validate known descriptors before interpreting live pool/queue state."""
    pools = {}
    for number, (expected_count, expected_size) in POOL_LAYOUT.items():
        control = memory.u32(POOL_TABLE + number * 4)
        descriptor = memory.u32(control)
        count = memory.u32(descriptor + 4)
        size = memory.u32(descriptor + 8)
        total = memory.u32(control + 12)
        free = memory.u16(control + 16)
        if (count, size, total) != (expected_count, expected_size, count * size):
            raise LayoutError(f'pool {number}: dump does not match known NXS layout')
        if free > count:
            raise LayoutError(f'pool {number}: free count exceeds capacity')
        pools[number] = dict(control=control, base=memory.u32(control + 8),
                             block_size=size, capacity=count, free=free,
                             free_head=memory.u32(control + 4))

    tasks = {}
    for number, name in TASK_NAMES.items():
        control = memory.u32(TASK_TABLE + number * 4)
        stack = memory.u32(control)
        tasks[number] = dict(name=name, control=control, saved_sp=stack,
                             state=memory.read(control + 5, 1)[0],
                             saved_pc=memory.u32(stack + 56))

    queues = {}
    for number in (44, 50):
        control = memory.u32(MAILBOX_TABLE + number * 4)
        # This known mailbox layout stores head/tail at +12/+16.
        head, tail = memory.u32(control + 12), memory.u32(control + 16)
        queue = dict(head=head, tail=tail, records=[], complete=False)
        seen = set()
        pointer = head
        while pointer:
            if pointer in seen:
                raise LayoutError(f'mailbox {number}: cycle in message chain')
            if len(seen) >= 256:
                raise LayoutError(f'mailbox {number}: diagnostic chain limit exceeded')
            seen.add(pointer)
            try:
                words = struct.unpack('<9I', memory.read(pointer, 36))
            except LayoutError as error:
                queue['unavailable'] = str(error)
                break
            pool = pools.get(words[6])
            if pool is not None:
                relative = pointer - pool['base']
                if (relative < 0 or relative % pool['block_size'] or
                        relative // pool['block_size'] >= pool['capacity']):
                    raise LayoutError(f'mailbox {number}: pointer outside its declared pool')
            # Other task replies, including heap-owned pool=0 messages, share
            # mailbox 50. Report them without pretending to validate ownership.
            queue['records'].append(dict(address=pointer, command=words[2],
                                         sequence=words[3], sender=words[4], pool=words[6],
                                         known_pool_validated=pool is not None))
            pointer = words[0]
        else:
            queue['complete'] = True
            # The kernel leaves a stale tail after dequeuing the last message;
            # head alone determines emptiness. Tail matters only when nonempty.
            if head and tail != queue['records'][-1]['address']:
                raise LayoutError(f'mailbox {number}: inconsistent tail')
        queues[number] = queue

    ready = memory.u32(RECEIVER_READY)
    special = memory.u32(SENDER_SPECIAL)
    blocked = (pools[52]['free'] == pools[60]['free'] == 0 and ready == special == 0
               and tasks[85]['state'] & 0xF0 == 0xD0
               and tasks[102]['state'] & 0xF0 == 0xD0
               and tasks[85]['saved_pc'] == 0x0414D5FA
               and tasks[102]['saved_pc'] == 0x0425998C)
    return dict(profile='research NXS MAIN fixed-address layout', pools=pools,
                tasks=tasks, mailboxes=queues, receiver_ready=ready,
                sender_special=special, known_pool_stall_signature=blocked,
                meaning='Snapshot only: absence of this signature does not prove boot or UI liveness.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ram', type=Path)
    args = parser.parse_args()
    try:
        with args.ram.open('rb') as stream:
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                result = inspect(Memory(data))
    except (OSError, ValueError) as error:
        parser.exit(2, f'nxs-gui-state: {error}\n')
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
