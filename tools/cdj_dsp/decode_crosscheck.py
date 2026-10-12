"""Cross-check the C674x core's decoder against GNU objdump over a DSP image.

Idea from Stijn Jacobs' cdj-nxs2-qemu ("make m1", decoder vs tic6x objdump
over the whole image; github.com/Stijn-Jacobs/cdj-nxs2-qemu, 08d5cb1).  Our
core has no disassembler, so the check is made at the level the interpreter
decides things:

* structure: every instruction our fetch produces sits at an address objdump
  also decodes, with the same size (16/32-bit) and the same parallel bit;
* routing: the semantic family the issue loop picks (cdj_c674x_describe) is
  one that implements objdump's mnemonic (FAMILIES below);
* operands of rewritten compact forms: a compact instruction the issue loop
  executes as a 32-bit equivalent (compact .D memory, M3 multiplies, the
  saturating forms) is disassembled again from that 32-bit word and must
  read exactly as objdump reads the compact original.

Rejections of words objdump decodes are reported as coverage gaps, and words
objdump calls undefined but we would execute as "accepted undefined".  The
range comes from a checkpoint (which holds L2 and SDRAM).  Exit 1 on any
disagreement.  Needs a built tools/cdj_dsp/decode_crosscheck.c (--tool) and
an objdump with the tic6x target (Homebrew binutils: gobjdump).

Usage:
  python -m tools.cdj_dsp.decode_crosscheck CHECKPOINT OUT.json --tool BIN \
      [--objdump gobjdump] [--range START:END ...]
"""
import argparse
import collections
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# NXS DSP code: stage1 in L2 and stage2 in SDRAM.
DEFAULT_RANGES = ['0x11801da0:0x11804d80', '0xc0000000:0xc0058320']

# Family -> objdump mnemonics it implements.  A pair outside this table is a
# routing disagreement.  Loop families are recognised by step/loop_step.
FAMILIES = {
    'abs': {'abs'}, 'abssp': {'abssp'},
    'add_imm': {'add'}, 'add_reg': {'add'},
    'addk': {'addk'}, 'addkpc': {'addkpc'},
    'adda-long-b': {'addab'}, 'adda-long-h': {'addah'}, 'adda-long-w': {'addaw'},
    'addsubdp': {'adddp', 'subdp'}, 'addsubsp': {'addsp', 'subsp'},
    'and_imm': {'and'}, 'and_reg': {'and'}, 'andn': {'andn'},
    'approx': {'rcpsp', 'rsqrsp', 'rcpdp', 'rsqrdp'},
    'b_disp': {'b'}, 'b_irp': {'b'}, 'b_nrp': {'b'}, 'b_reg': {'b'},
    'bdec': {'bdec'}, 'bnop_disp': {'bnop'}, 'bnop_reg': {'bnop'},
    'bpos': {'bpos'},
    'bitfield': {'clr', 'set', 'ext', 'extu'},
    'callp': {'callp'},
    'cmp': {'cmpeq', 'cmpgt', 'cmpgtu', 'cmplt', 'cmpltu'},
    'cmp_long': {'cmpeq', 'cmpgt', 'cmpgtu', 'cmplt', 'cmpltu'},
    'cmpdp': {'cmpeqdp', 'cmpgtdp', 'cmpltdp'},
    'cmpsp': {'cmpeqsp', 'cmpgtsp', 'cmpltsp'},
    'd_adda': {'addab', 'addah', 'addaw', 'addad', 'subab', 'subah', 'subaw'},
    'd_addsub': {'add', 'sub'}, 'd_addsub_cross': {'add', 'sub'},
    'dint': {'dint'}, 'rint': {'rint'},
    'dp_convert': {'dpsp', 'dpint', 'dptrunc'},
    'intdp': {'intdp', 'intdpu'}, 'intsp': {'intsp', 'intspu'},
    'l_long_addsub': {'add', 'addu', 'sub', 'subu'},
    'mpy16': {'mpy', 'mpyu', 'mpyus', 'mpysu', 'mpyh', 'mpyhu', 'mpyhus',
              'mpyhsu', 'mpyhl', 'mpyhlu', 'mpyhuls', 'mpyhslu', 'mpylh',
              'mpylhu', 'mpyluhs', 'mpylshu'},
    'mpy32': {'mpy32', 'mpy32u', 'mpy32su', 'mpy32us'},
    'mpy_half32': {'mpyli', 'mpyhi', 'mpylir', 'mpyhir'},
    'mpydp': {'mpydp', 'mpyspdp', 'mpysp2dp'},
    'mpyi': {'mpyi', 'mpyid'}, 'mpysp': {'mpysp'},
    'mvc_read': {'mvc'}, 'mvc_write': {'mvc'}, 'mvd': {'mvd'},
    'mvk_d': {'mvk'}, 'mvk_l': {'mvk'}, 'mvk_s': {'mvk'}, 'mvkh': {'mvkh', 'mvklh'},
    'nop': {'nop'}, 'idle': {'idle'},
    'or_imm': {'or'}, 'or_reg': {'or'},
    'pack': {'pack2', 'packh2', 'packhl2', 'packlh2', 'packl4', 'packh4'},
    'packbits_lmbd': {'lmbd'}, 'packbits_norm': {'norm'},
    'packbits_m': {'rotl', 'bitr', 'bitc4', 'shfl', 'deal', 'xormpy', 'gmpy4'},
    'sat_scalar': {'sadd', 'ssub', 'sshl'},
    'scalar_memory': {'ldb', 'ldbu', 'ldh', 'ldhu', 'ldw', 'lddw', 'ldndw',
                      'ldnw', 'stb', 'sth', 'stw', 'stdw', 'stndw', 'stnw'},
    'shift_long': {'shl', 'shr', 'shru'}, 'shift_s': {'shl', 'shr', 'shru'},
    'spdp': {'spdp'}, 'two_cycle_dp': {'spdp', 'absdp', 'rcpdp', 'rsqrdp'},
    'spint': {'spint', 'sptrunc'},
    'sub_imm': {'sub'}, 'sub_reg': {'sub'}, 'sub_reverse': {'sub'},
    'subc': {'subc'},
    'xor_imm': {'xor'}, 'xor_reg': {'xor'},
    'spmask': {'spmask', 'spmaskr'},
    'sploop': {'sploop'}, 'sploopd': {'sploopd'}, 'sploopw': {'sploopw'},
    'spkernel': {'spkernel'}, 'spkernelr': {'spkernelr'},
    # Compact forms executed in place by the issue loop.
    'compact-mvk01': {'mvk'}, 'compact-mvk_l': {'mvk'}, 'compact-mvk_s': {'mvk'},
    'compact-lsdx1': {'mvk', 'sub', 'add', 'xor'},
    'compact-s_addsub': {'add', 'sub'}, 'compact-sx2op': {'add', 'sub'},
    'compact-d_addsub': {'add', 'sub'}, 'compact-l_addsub': {'add', 'sub'},
    'compact-l_addk': {'add'}, 'compact-addk': {'addk'},
    'compact-dx5': {'addaw'}, 'compact-dx5p': {'addaw', 'subaw'},
    'compact-s_shift': {'shl', 'shr'}, 'compact-ssh5': {'shl', 'shr', 'shru', 'sshl'},
    'compact-s2sh': {'shl', 'shr', 'shru', 'sshl'},
    'compact-bits': {'clr', 'set', 'ext', 'extu'},
    'compact-cmpeq': {'cmpeq'}, 'compact-cmp_order': {'cmpgt', 'cmplt', 'cmpgtu', 'cmpltu'},
    'compact-l2c': {'and', 'or', 'xor', 'cmpeq', 'cmpgt', 'cmplt', 'cmpgtu', 'cmpltu'},
    'compact-bnop': {'bnop'}, 'compact-bnop_reg': {'bnop'},
    'compact-mvc_ilc': {'mvc'}, 'compact-move': {'mv'},
    'compact-b15_word': {'ldw', 'stw'}, 'compact-dpp': {'ldw', 'stw', 'lddw', 'stdw'},
}
REJECTIONS = {'unimplemented', 'reserved-predicate', 'reserved-nop',
              'compact-none', 'sploopd-reload', 'unnamed-arm'}
LINE = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f]+)\s*\t(.*)$')


def objdump_lines(objdump, image, base):
    text = subprocess.run([objdump, '-D', '-z', '-EL', '-b', 'binary', '-m', 'tic6x',
                           f'--adjust-vma={base:#x}', str(image)],
                          check=True, capture_output=True, text=True).stdout
    out = {}
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        body = m.group(3).strip()
        if body.startswith('<fetch packet header'):
            continue
        parallel = body.startswith('||')
        body = body.lstrip('|').strip()
        # binutils appends " || nop 5" to every instruction of a PROT fetch
        # packet (tic6x-dis.c); it is display, not decode.
        body = body.removesuffix(' || nop 5')
        bare = re.sub(r'^\[!?[ab]\d+\]\s*', '', body)
        mnemonic = 'UNDEFINED' if bare.startswith('<undefined') else bare.split()[0]
        out[int(m.group(1), 16)] = dict(size=len(m.group(2)) // 2, mnemonic=mnemonic,
                                         parallel=parallel, text=body)
    return out


def ours_lines(tool, checkpoint, start, end, image):
    text = subprocess.run([str(tool), str(checkpoint), hex(start), hex(end), str(image)],
                          check=True, capture_output=True, text=True).stdout
    out, faults = {}, {}
    for line in text.splitlines():
        f = line.split()
        if f[1] == 'fetch-fault':
            faults[int(f[0], 16)] = ' '.join(f[2:])
            continue
        out[int(f[0], 16)] = dict(size=int(f[1]), word=f[2], compact=f[3] == 'compact',
                                  parallel=f[4] == 'parallel', family=f[5],
                                  lowered=None if f[6] == '-' else int(f[6], 16))
    return out, faults


def lowered_texts(objdump, words, workdir):
    """Disassemble each 32-bit word alone in its own header-free fetch block."""
    image = workdir / 'lowered.bin'
    image.write_bytes(b''.join(struct.pack('<I', w & ~1) + bytes(28) for w in words))
    lines = objdump_lines(objdump, image, 0)
    return [lines[32 * i]['text'] for i in range(len(words))]


def check(tool, objdump, checkpoint, start, end, workdir):
    image = workdir / f'{start:08x}.bin'
    ours, faults = ours_lines(tool, checkpoint, start, end, image)
    theirs = objdump_lines(objdump, image, start)
    addresses = sorted(theirs)
    following = {a: b for a, b in zip(addresses, addresses[1:])}
    found = collections.defaultdict(list)
    pairs = collections.Counter()
    lowered = []
    for address, insn in sorted(ours.items()):
        other = theirs.get(address)
        where = f'{address:08x}'
        if other is None or other['size'] != insn['size']:
            found['structure'].append(f'{where} size {insn["size"]} vs objdump '
                                      f'{other and other["size"]}')
            continue
        nxt = theirs.get(following.get(address))
        their_parallel = bool(nxt and nxt['parallel'])
        if insn['parallel'] != their_parallel:
            found['parallel'].append(f'{where} ours {insn["parallel"]} objdump {their_parallel}')
        family, mnemonic = insn['family'], other['mnemonic']
        pairs[(mnemonic, family)] += 1
        if mnemonic == 'UNDEFINED':
            if family not in REJECTIONS:
                found['accepted_undefined'].append(f'{where} {insn["word"]} as {family}')
        elif family in REJECTIONS:
            found['coverage_gap'].append(f'{where} {other["text"]} rejected as {family}')
        elif mnemonic not in FAMILIES.get(family, ()):
            found['routing'].append(f'{where} {other["text"]} routed to {family}')
        if insn['lowered'] is not None and insn['lowered'] != int(insn['word'], 16):
            lowered.append((where, other['text'], insn['lowered']))
    for (where, text, word), again in zip(
            lowered, lowered_texts(objdump, [w for _, _, w in lowered], workdir)):
        if again != text:
            found['lowering'].append(f'{where} {text!r} executes as {again!r}')
    covered = sum(1 for a in theirs if a in ours)
    return dict(start=f'{start:#x}', end=f'{end:#x}', objdump_instructions=len(theirs),
                our_instructions=len(ours), compared=covered,
                fetch_faults={f'{a:08x}': r for a, r in sorted(faults.items())},
                lowered_compact_checked=len(lowered),
                pairs={f'{m} -> {f}': n for (m, f), n in sorted(pairs.items())},
                findings={k: v for k, v in sorted(found.items())})


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('checkpoint', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--tool', type=Path, required=True,
                    help='built tools/cdj_dsp/decode_crosscheck.c')
    ap.add_argument('--objdump', default='gobjdump')
    ap.add_argument('--range', action='append', dest='ranges')
    args = ap.parse_args(argv)
    reports = []
    with tempfile.TemporaryDirectory() as tmp:
        for spec in args.ranges or DEFAULT_RANGES:
            start, end = (int(x, 0) for x in spec.split(':'))
            reports.append(check(args.tool, args.objdump, args.checkpoint, start, end,
                                 Path(tmp)))
    args.output.write_text(json.dumps(dict(checkpoint=str(args.checkpoint),
                                           ranges=reports), indent=2) + '\n')
    bad = 0
    for r in reports:
        counts = {k: len(v) for k, v in r['findings'].items()}
        print(f"{r['start']}-{r['end']}: {r['compared']} compared, "
              f"{r['lowered_compact_checked']} rewritten compact, "
              f"{len(r['fetch_faults'])} fetch faults, findings {counts or 'none'}")
        for kind, items in r['findings'].items():
            if kind != 'coverage_gap':
                bad += len(items)
            for item in items[:20]:
                print(f'  {kind}: {item}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
