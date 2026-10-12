# SPDX-License-Identifier: GPL-2.0-or-later
"""Differential test of the fast Blackfin core (emulator/bfin) against GNU sim
(bin/cdj-run, BFIN_PARALLEL_WRITEBACK=1, the reference).

Every instruction encoding objdump finds in the given GUI ELFs (16-bit, 32-bit
and whole parallel bundles) runs from random states -- pointers inside a
per-case memory region ("a"), and fully random registers for encodings that
touch no memory ("w") -- plus random runs of 2-8 instructions that keep their
addresses in the region ("s"). One ELF holds the cases; each sets every
register, executes the instruction(s) and stores R, P, SP, FP, I, M, B, L, A0,
A1, ASTAT, RETS, LC0 and LC1 next to its 256-byte region. GNU sim writes each
case out with a syscall and restarts after a fault; the fast core
(tests/cstub/bfin-diff-runner.c) stops at the same point. Any field or region
byte that differs is a divergence; so is a case one rejects and the other runs.

    python -m tools.cdj_gui.bfin_diff OUT --elf firmware/nxs/gui-boot-memory.elf \\
        [--elf MODS.elf] [--states 12] [--wild 12] [--seq 100000] [--random 40000]

Needs bfin-elf-as/-ld/-objdump (BFIN_TOOLCHAIN, default
~/.local/share/cdj-toolchain/binutils-bfin/bin) and bin/cdj-run. Writes
OUT/report.txt; exit status 1 on any divergence.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from tools.paths import REPO_ROOT

NW = 40
BLK = NW * 4 + 256
IN_BASE, BLK_BASE = 0x1000000, 0x1800000
ASTAT_DEF = 0x030F316F
NAMES = ([f'R{i}' for i in range(8)] + [f'P{i}' for i in range(6)] + ['SP', 'FP'] +
         [f'{g}{n}' for g in 'IMBL' for n in range(4)] +
         ['A0.W', 'A0.X', 'A1.W', 'A1.X', 'ASTAT', 'RETS', 'LC0', 'LC1'])
TOOLS = Path(os.environ.get('BFIN_TOOLCHAIN', Path.home() / '.local/share/cdj-toolchain/binutils-bfin/bin'))
SOURCES = ['bf531.c', 'bfin_core.c', 'bfin_exec.c', 'bfin_dsp.c']


def words(h):
    b = bytes.fromhex(h)
    return [b[i] | b[i + 1] << 8 for i in range(0, len(b), 2)]


def sext(v, n):
    v &= (1 << n) - 1
    return v - (1 << n) if v >> (n - 1) else v


def state(h, mode, seed, region):
    """The case's 40 starting register words and 256 region bytes."""
    rng = random.Random(seed)
    specials = [0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x8000, 0x7FFF,
                0xFFFF8000, 0x7FFF7FFF, 0x80008000, 0xFFFF0001, 0x00010000, 0x40000000]

    def val():
        r = rng.random()
        if r < 0.2: return rng.choice(specials)
        if r < 0.3: return rng.getrandbits(16) * 0x10001 & 0xFFFFFFFF
        if r < 0.4: return sext(rng.getrandbits(9), 9) & 0xFFFFFFFF
        return rng.getrandbits(32)

    def addr():
        return region + 64 + 4 * rng.randrange(32)
    s = [val() for _ in range(8)]
    wild = mode == 'w'
    s += [val() if wild else addr() for _ in range(6)]
    s += [region + 64 + 4 * rng.randrange(16) if not wild else val(),
          region + 128 + 4 * rng.randrange(28) if not wild else val()]
    s += [val() if wild else addr() for _ in range(4)]
    if mode == 's':
        s += [rng.choice([0, 4, 0xFFFFFFFC, 8, 2, 0xFFFFFFFE]) for _ in range(4)]
    else:
        s += [rng.choice([0, 4, 0xFFFFFFFC, 8, 2, 0xFFFFFFFE, val(), val()]) for _ in range(4)]
    s += [rng.choice([region + 64, region + 96, val()]) if wild else region + 64 + 4 * rng.randrange(8) for _ in range(4)]
    s += [rng.choice([0, 0, 0, 16, 32, 64, 6, val()]) if wild else rng.choice([0, 0, 0, 16, 32, 64]) for _ in range(4)]
    s += [val(), sext(rng.getrandbits(8), 8) & 0xFFFFFFFF, val(), sext(rng.getrandbits(8), 8) & 0xFFFFFFFF]
    s += [rng.getrandbits(32) & ASTAT_DEF, 0, 0, 0]
    w = words(h)
    if len(w) >= 2 and (w[0] & 0xFC00) == 0xE400 and (w[0] >> 6) & 3 < 3:
        # The one address a large offset touches, inside the region.
        s[8 + ((w[0] >> 3) & 7)] = (region + 128 - sext(w[1], 16) * (4 >> ((w[0] >> 6) & 3))) & 0xFFFFFFFF
    mem = bytes(rng.getrandbits(8) for _ in range(256))
    return s, mem


# ---- which encodings ------------------------------------------------------------

def testable(h):
    """Runs straight through and leaves no state the dump cannot see."""
    n, w = len(h) // 2, words(h)[0]
    if n == 2:
        if w < 0x0100:
            return w in (0, 0x23, 0x24) or 0xB0 <= w <= 0xB5     # NOP CSYNC SSYNC TESTSET
        if w < 0x0180:                                           # push/pop one
            grp = (w >> 3) & 7
            if grp == 7: return False
            if grp == 6: return bool(w & 0x40) and (w & 7) < 6
            return True
        if 0x1000 <= w < 0x3000: return False                    # branches
        if (w & 0xF000) == 0x3000:
            gd, gs = (w >> 9) & 7, (w >> 6) & 7
            return not (gd >= 6 or gs == 7 or (gs == 6 and (w & 7) >= 6))
        return w < 0xC000
    if n == 4:
        return not ((w & 0xFF80) == 0xE080 or (w & 0xFE00) == 0xE200 or w >= 0xF000)
    return n == 8


def nomem(h):
    n, w = len(h) // 2, words(h)[0]
    if n == 2:
        return ((0x0200 <= w < 0x0400 or 0x0600 <= w < 0x1000 or 0x3000 <= w < 0x7000 or
                 (w & 0xFF60) == 0x9E60 or (w & 0xFFF0) == 0x9F60) and not 0x0240 <= w < 0x0280)
    if n == 4:
        return (w & 0xF000) == 0xC000 and not w & 0x0800 or (w & 0xFF00) == 0xE100
    return False


def _seq_safe16(x, slot=False):
    """Keeps every later address of a run inside the region."""
    if x == 0 or 0x9C00 <= x < 0x9E00: return True
    if 0x9000 <= x < 0x9C00: return not (x & 0x40 and not (x >> 10) & 3 and not x & 0x200)
    if 0xA000 <= x < 0xB800: return (x & 0x1C00) != 0x0C00
    if 0xB800 <= x < 0xBC00: return bool(x & 0x200) or (x & 0xF) < 8
    if slot: return False
    if (x & 0xFF60) == 0x9E60: return not x & 0x80
    if (x & 0xFFF0) == 0x9F60: return True
    if not nomem(bytes([x & 0xFF, x >> 8]).hex()): return False
    if 0x4400 <= x < 0x4600 or 0x5A00 <= x < 0x6000 or 0x6800 <= x < 0x7000: return False
    if (x & 0xF000) == 0x3000: return (x >> 9) & 7 not in (1, 2, 3)
    if (x & 0xFE00) == 0x0600: return not x & 0x80
    return True


def seq_safe(h):
    w = words(h)
    if len(w) == 1: return _seq_safe16(w[0])
    if len(w) == 2: return ((w[0] & 0xF000) == 0xC000 and not w[0] & 0x800) or (w[0] & 0xFF18) == 0xE100
    return _seq_safe16(w[2], True) and _seq_safe16(w[3], True)


def encodings(elf):
    """Unique encodings at objdump's instruction starts, as little-endian hex."""
    data = Path(elf).read_bytes()
    phoff, = struct.unpack_from('<I', data, 28)
    phnum, = struct.unpack_from('<H', data, 44)
    segs = []
    for i in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from('<6I', data, phoff + 32 * i)
        if t == 1:
            segs.append((pa, data[off:off + fs]))
    out = subprocess.run([str(TOOLS / 'bfin-elf-objdump'), '-d', str(elf)],
                         capture_output=True, text=True, check=True).stdout
    seen = set()
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)', line)
        if not m or 'ILLEGAL' in m.group(3) or m.group(3).startswith('.'):
            continue
        addr = int(m.group(1), 16)
        n = 8 if '||' in m.group(3) else len(m.group(2).split())
        for base, b in segs:
            if base <= addr and addr + n <= base + len(b) and n >= 2:
                seen.add(b[addr - base:addr - base + n].hex())
    return seen


def cases(encs, states, wild, seq, rnd, seed):
    rng = random.Random(seed)
    out = []
    pool = sorted(h for h in encs if len(h) in (4, 8, 16) and testable(h))
    for h in pool:
        out += [(h, 'a', rng.getrandbits(48)) for _ in range(states)]
        if nomem(h):
            out += [(h, 'w', rng.getrandbits(48)) for _ in range(wild)]
    safe = [h for h in pool if seq_safe(h)]
    big = [h for h in safe if len(h) >= 8]
    for _ in range(seq if safe else 0):
        k = rng.randint(2, 8)
        out.append((''.join(rng.choice(big if big and rng.random() < 0.5 else safe) for _ in range(k)),
                    's', rng.getrandbits(48)))
    for _ in range(rnd):                 # encodings beyond the firmware's
        w = rng.getrandbits(16)
        h = bytes([w & 0xFF, w >> 8]).hex()
        if testable(h):
            out.append((h, 'w' if nomem(h) else 'a', rng.getrandbits(48)))
        iw0 = (0xC000 | rng.getrandbits(11)) & ~0x0800
        if (iw0 & 0xF600) == 0xC400: iw0 &= ~0x01C0
        if (iw0 & 0xF780) == 0xC600: iw0 &= ~0x0060
        iw1 = rng.getrandbits(16)
        out.append((bytes([iw0 & 0xFF, iw0 >> 8, iw1 & 0xFF, iw1 >> 8]).hex(), 'w', rng.getrandbits(48)))
    return out


# ---- one chunk --------------------------------------------------------------------

def program(pre, chunk):
    asm = ['.text', '.global _start', '_start:', 'R0 = 0; LC0 = R0; LC1 = R0;',
           'P0.L = _first; P0.H = _first; R0 = [P0];',
           'P1.L = _cases; P1.H = _cases; R0 <<= 2; P2 = R0; P1 = P1 + P2; P1 = [P1]; JUMP (P1);',
           '_setup:']
    for g, n0 in (('B', 96), ('L', 112), ('M', 80), ('I', 64)):
        asm += [f'R0 = [P5 + {n0 + 4 * n}]; {g}{n} = R0;' for n in range(4)]
    asm += ['R0 = [P5 + 128]; A0.W = R0; R0 = [P5 + 132]; A0.X = R0.L;',
            'R0 = [P5 + 136]; A1.W = R0; R0 = [P5 + 140]; A1.X = R0.L;',
            'R0 = [P5 + 144]; ASTAT = R0;']
    asm += [f'R{n} = [P5 + {4 * n}];' for n in range(8)]
    asm += ['SP = [P5 + 56]; FP = [P5 + 60];'] + [f'P{n} = [P5 + {32 + 4 * n}];' for n in range(5)]
    asm += ['P5 = [P5 + 52];', 'RTS;', '_dump:', 'R0 = ASTAT; [P5 + 144] = R0;']
    asm += [f'[P5 + {4 * n}] = R{n};' for n in range(1, 8)]
    asm += [f'[P5 + {32 + 4 * n}] = P{n};' for n in range(5)]
    asm += ['R0 = RETN; [P5 + 52] = R0; [P5 + 56] = SP; [P5 + 60] = FP;']
    o = 64
    for g in 'IMBL':
        for n in range(4):
            asm.append(f'R0 = {g}{n}; [P5 + {o}] = R0;')
            o += 4
    asm += ['R0 = A0.W; [P5 + 128] = R0; R0 = A0.X; [P5 + 132] = R0;',
            'R0 = A1.W; [P5 + 136] = R0; R0 = A1.X; [P5 + 140] = R0;',
            'R0 = LC0; [P5 + 152] = R0; R0 = LC1; [P5 + 156] = R0;',
            '.global _emit', '_emit:',
            'P0.L = _is_gdb; P0.H = _is_gdb; R0 = [P0]; CC = R0 == 0; IF CC JUMP _emit_ret;',
            'P0.L = _args; P0.H = _args; R1 = 1; [P0] = R1; [P0 + 4] = P5;',
            f'R1 = {BLK}; [P0 + 8] = R1; R0 = P0; P0 = 5; EXCPT 0;', '_emit_ret: RTS;']
    ins, blks = [], []
    for k, (h, mode, seed) in enumerate(chunk):
        blk, i_k = BLK_BASE + k * BLK, IN_BASE + k * NW * 4
        s, mem = state(h, mode, seed, blk + NW * 4)
        ins.append(struct.pack(f'<{NW}I', *s))
        blks.append(bytes(NW * 4) + mem)
        asm += [f'_c{k}:',
                f'P5.L = {blk & 0xFFFF:#x}; P5.H = {blk >> 16:#x}; RETE = P5;',
                f'P5.L = {i_k & 0xFFFF:#x}; P5.H = {i_k >> 16:#x}; CALL _setup;',
                '.short ' + ', '.join(f'{x:#06x}' for x in words(h)) + ';',
                'RETN = P5; P5 = RETE; [P5] = R0; R0 = RETS; [P5 + 148] = R0; CALL _dump;']
    asm += ['.global _done', '_done:',
            'P0.L = _args; P0.H = _args; R0 = P0; R2 = 0; [P0] = R2; P0 = 1; EXCPT 0;',
            '_spin: JUMP.S _spin;', '.data', '_args: .long 0, 0, 0, 0',
            '.global _first', '_first: .long 0', '.global _is_gdb', '_is_gdb: .long 0',
            '.global _cases', '_cases: .long ' + ', '.join(f'_c{k}' for k in range(len(chunk))),
            '.section .tdata_in, "aw"', f'.incbin "{pre}.in.bin"',
            '.section .tdata_blk, "aw"', f'.incbin "{pre}.blk.bin"']
    Path(pre + '.s').write_text('\n'.join(asm) + '\n')
    Path(pre + '.in.bin').write_bytes(b''.join(ins))
    Path(pre + '.blk.bin').write_bytes(b''.join(blks))


def elf_patch(path, addr, val):
    d = bytearray(Path(path).read_bytes())
    phoff, = struct.unpack_from('<I', d, 28)
    phnum, = struct.unpack_from('<H', d, 44)
    for i in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from('<6I', d, phoff + 32 * i)
        if t == 1 and pa <= addr < pa + fs:
            struct.pack_into('<I', d, off + addr - pa, val)
            Path(path).write_bytes(d)
            return
    raise SystemExit(f'no segment for {addr:#x}')


def run_chunk(pre, chunk, runner, gdb):
    program(pre, chunk)
    subprocess.run([str(TOOLS / 'bfin-elf-as'), pre + '.s', '-o', pre + '.o'], check=True)
    subprocess.run([str(TOOLS / 'bfin-elf-ld'), '-Ttext=0x2000000', '-Tdata=0x800000',
                    f'--section-start=.tdata_in={IN_BASE:#x}', f'--section-start=.tdata_blk={BLK_BASE:#x}',
                    '-e', '_start', pre + '.o', '-o', pre + '.elf'], check=True)
    nm = {}
    for line in subprocess.run([str(TOOLS / 'bfin-elf-nm'), pre + '.elf'], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            nm[p[2]] = int(p[0], 16)
    n = len(chunk)
    out = subprocess.run([str(runner), pre + '.elf', hex(nm['_emit']), hex(nm['_cases']), hex(nm['_first']), str(n)],
                         capture_output=True, timeout=1200).stdout
    fast, o = {}, 0
    while o < len(out):
        k, st = struct.unpack_from('<II', out, o)
        o += 8
        if st == 0:
            fast[k] = out[o:o + BLK]
            o += BLK
        else:
            fast[k] = 'undefined' if st == 1 else 'runaway'
    g_elf = pre + '.g.elf'
    shutil.copy(pre + '.elf', g_elf)
    elf_patch(g_elf, nm['_is_gdb'], 1)
    ref, first = {}, 0
    env = {'PATH': '/usr/bin:/bin', 'BFIN_TIME_BASE': 'insn', 'BFIN_PARALLEL_WRITEBACK': '1'}
    while first < n:
        elf_patch(g_elf, nm['_first'], first)
        r = subprocess.run([str(gdb), '--model', 'bf531', '--environment', 'virtual',
                            '--memory-region', '0,64M', g_elf], env=env, capture_output=True, timeout=1200)
        k = len(r.stdout) // BLK
        for j in range(k):
            ref[first + j] = r.stdout[j * BLK:(j + 1) * BLK]
        if first + k >= n:
            break
        ref[first + k] = 'fault'
        first += k + 1
    bad = []
    for k, (h, mode, seed) in enumerate(chunk):
        g, f = ref.get(k), fast.get(k, 'missing')
        rec = {'enc': h, 'mode': mode, 'seed': seed, 'region': BLK_BASE + k * BLK + NW * 4}
        if isinstance(g, bytes) and isinstance(f, bytes):
            a, c = struct.unpack_from(f'<{NW}I', g), struct.unpack_from(f'<{NW}I', f)
            d = [NAMES[i] for i in range(NW) if a[i] != c[i]] + (['MEM'] if g[NW * 4:] != f[NW * 4:] else [])
            if d:
                bad.append(dict(rec, diff=d, gdb=[f'{x:x}' for x in a], fast=[f'{x:x}' for x in c],
                                gdb_mem=g[NW * 4:].hex(), fast_mem=f[NW * 4:].hex()))
        elif g == 'fault' and isinstance(f, bytes):
            bad.append(dict(rec, gdb_fault=True))            # GNU sim rejects, fast core runs
        elif isinstance(g, bytes):
            bad.append(dict(rec, fast=f))                    # fast core rejects or runs away
    for suffix in ('.s', '.o', '.in.bin', '.blk.bin', '.g.elf'):
        Path(pre + suffix).unlink(missing_ok=True)
    return n, bad


def build_runner(out):
    exe = out / 'bfin-diff-runner'
    bfin = REPO_ROOT / 'emulator/bfin'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-I', str(bfin),
                    str(REPO_ROOT / 'tests/cstub/bfin-diff-runner.c'), *(str(bfin / s) for s in SOURCES),
                    '-o', str(exe)], check=True)
    return exe


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--elf', action='append', required=True)
    ap.add_argument('--states', type=int, default=12)
    ap.add_argument('--wild', type=int, default=12)
    ap.add_argument('--seq', type=int, default=100000)
    ap.add_argument('--random', type=int, default=40000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--chunk', type=int, default=8000)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--gdb', type=Path, default=REPO_ROOT / 'bin/cdj-run')
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    encs = set()
    for e in a.elf:
        encs |= encodings(e)
    todo = cases(encs, a.states, a.wild, a.seq, a.random, a.seed)
    runner = build_runner(a.out)
    chunks = [todo[i:i + a.chunk] for i in range(0, len(todo), a.chunk)]
    with ThreadPoolExecutor(a.jobs) as pool:
        results = list(pool.map(lambda ic: run_chunk(str(a.out / f'c{ic[0]:04d}'), ic[1], runner, a.gdb),
                                enumerate(chunks)))
    total = sum(n for n, _ in results)
    bad = [b for _, bs in results for b in bs]
    div = [b for b in bad if 'diff' in b or 'fast' in b]
    by = collections.Counter(b['enc'] for b in div)
    lines = [f'encodings {len(encs)} cases {total} divergent cases {len(div)} '
             f'GNU-sim-only faults {sum(1 for b in bad if b.get("gdb_fault"))}']
    lines += [f'{h} {by[h]}' for h in sorted(by)]
    (a.out / 'report.txt').write_text('\n'.join(lines) + '\n')
    (a.out / 'divergences.json').write_text(json.dumps(div))
    print(lines[0])
    return 1 if div else 0


if __name__ == '__main__':
    sys.exit(main())
