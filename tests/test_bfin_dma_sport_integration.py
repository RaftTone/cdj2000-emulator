"""Guest MMIO -> DMA -> SPORT regressions in the actual BF531 simulator."""
import os
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('mode', ['stop', 'chain', 'unaligned', 'bad_descriptor', 'no_irq', 'receive'])
def test_guest_dma_sport(tmp_path, mode):
    assembler = os.environ.get('BFIN_AS') or shutil.which('bfin-elf-as') or str(ROOT / 'build/bfin-binutils/gas/as-new')
    linker = os.environ.get('BFIN_LD') or shutil.which('bfin-elf-ld') or str(ROOT / 'build/bfin-binutils/ld/ld-new')
    sim = ROOT / 'bin' / ('cdj-run.exe' if os.name == 'nt' else 'cdj-run')
    if not all(Path(p).is_file() for p in (assembler, linker, sim)):
        pytest.skip('run sh scripts/build-bfin-tools.sh and build the simulator')
    # DMA1 is SPORT0 TX; use 16-bit transfers and completion interrupts.
    setup = 'R0 = 0x85 (Z);'
    if mode == 'chain':
        setup = 'R0.L = desc1; R0.H = desc1; [P0] = R0; R0 = 0x7985 (Z);'
    if mode == 'bad_descriptor':
        setup = 'R0.H = 0x0400; R0.L = 0; [P0] = R0; R0 = 0x7985 (Z);'
    if mode == 'no_irq':
        setup = 'R0 = 5;'
    if mode == 'receive':
        setup = 'R0 = 0x87 (Z);'
    irq_mask = 0x400 if mode == 'receive' else 0x200
    dma_base = 0x0c80 if mode == 'receive' else 0x0c40
    irq = 0 if mode == 'no_irq' else irq_mask
    received = ''
    if mode == 'receive':
        received = 'P3.L = payload; P3.H = payload;'
        for offset in range(0, 8, 2):
            expected_word = (34 + offset) * 256 + 33 + offset
            received += f' R3 = W[P3+{offset}] (Z); DBGAL(R3, {expected_word});'
        # DMA must leave the adjacent memory untouched.
        received += ' R3 = W[P3+8] (Z); DBGAL(R3, 0x0a09);'

    address = 'payload+1' if mode == 'unaligned' else 'payload'
    status = 2 if mode in ('unaligned', 'bad_descriptor') else 1
    source = f'''
.global _start
.text
_start:
    P0.H = 0xffc0; P0.L = {dma_base};
    R0.L = {address}; R0.H = {address}; [P0+4] = R0;
    R0 = 4; W[P0+16] = R0;
    R0 = 2; W[P0+20] = R0;
    {setup}
    W[P0+8] = R0;
    P1 = 1000 (Z);
wait:
    R0 = W[P0+40] (Z);
    CC = BITTST(R0, 3);
    IF !CC JUMP done;
    P1 += -1;
    R1 = P1;
    CC = R1 == 0;
    IF !CC JUMP wait;
    R0 = 0; DBGAL(R0, 0xffff);
done:
    DBGAL(R0, {status});
    P2.H = 0xffc0; P2.L = 0x0120;
    R1 = [P2];
    R2 = {irq_mask} (Z); R1 = R1 & R2;
    DBGAL(R1, {irq});
    R0 = {status}; W[P0+40] = R0;
    R0 = W[P0+40] (Z); DBGAL(R0, 0);
    R1 = [P2]; R1 = R1 & R2; DBGAL(R1, 0);
    {received}
    OUTC 'p'; OUTC 'a'; OUTC 's'; OUTC 's'; HLT;
.data
.align 4
payload: .byte 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16
.align 4
desc1: .long desc2, payload
.short 0x7985,4,2,0,0
.align 4
desc2: .long 0, payload+8
.short 0x85,4,2,0,0
'''
    asm, obj, elf = (tmp_path / name for name in ('dma.s', 'dma.o', 'dma.elf'))
    asm.write_text(source)
    subprocess.run([assembler, str(asm), '-o', str(obj)], check=True, capture_output=True)
    subprocess.run([linker, '-Ttext=0x1000', '-Tdata=0x4000', '-e', '_start', str(obj), '-o', str(elf)], check=True, capture_output=True)
    capture = tmp_path / 'tx.bin'
    env = {k: v for k, v in os.environ.items() if not k.startswith('BFIN_')}
    env.update(BFIN_TIME_BASE='insn', BFIN_SPORT_TX_OUTPUT=str(capture))
    if mode == 'receive':
        rx = tmp_path / 'rx.bin'
        rx.write_bytes(struct.pack('<I', 8) + bytes(range(33, 41)))
        env.update(BFIN_SPORT_RX_INPUT=str(rx), BFIN_SPORT_RX_RECORDS='1')
    result = subprocess.run([str(sim), '--model', 'bf531', '--environment', 'operating', '--memory-region', '0,64M', str(elf)], env=env, capture_output=True, text=True, timeout=10)
    output = result.stdout + result.stderr
    assert result.returncode == 0 and 'pass' in output and 'FAIL' not in output, output
    expected = b''
    for start in ([] if mode in ('unaligned', 'bad_descriptor', 'receive') else [1, 9] if mode == 'chain' else [1]):
        expected += b'SPTX' + struct.pack('<II', 0xffc00800, 8) + bytes(range(start, start + 8))
    assert (capture.read_bytes() if capture.exists() else b'') == expected
