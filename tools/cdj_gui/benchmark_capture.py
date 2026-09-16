"""Benchmark actual before/after SPORT dump functions with synthetic records."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def capture_source(text):
    marker = '/* The run-local capture path'
    begin = text.index(marker) if marker in text else text.index('static void\nbfin_sport_link_dump (')
    return text[begin:text.index('/* BFIN_LINK_RX_CENSUS', begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, default=Path('build/gdb-17.2/sim/bfin/dv-bfin_ppi.c'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--records', type=int, default=100000)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    for label, source in [('before', args.before), ('after', args.after)]:
        c = args.output / f'{label}.c'
        c.write_text('#include <stdio.h>\n#include <stdlib.h>\n' +
                     capture_source(source.read_text()) + '''
int main(int argc, char **argv) {
    unsigned char bytes[64] = {0};
    if (argc != 2) return 2;
    unsigned count = strtoul(argv[1], NULL, 10);
    for (unsigned i=0; i<count; i++) {
        bytes[0] = i; bytes[1] = i >> 8;
        bfin_sport_link_dump(bytes, sizeof(bytes));
    }
    return 0;
}
''')
        subprocess.run(['cc', '-O3', str(c), '-o', str(args.output / label)], check=True)
    rows = []
    for trial in range(3):
        for label in (('before', 'after') if trial % 2 == 0 else ('after', 'before')):
            output = args.output / f'{label}-{trial}.capture'
            env = os.environ.copy()
            env['BFIN_MAIN_LINK_DUMP'] = str(output)
            start = time.perf_counter()
            subprocess.run([str(args.output / label), str(args.records)], env=env, check=True)
            elapsed = time.perf_counter() - start
            data = output.read_bytes()
            assert len(data) == args.records * 72
            rows.append(dict(label=label, trial=trial, seconds=elapsed, bytes=len(data),
                             sha256=hashlib.sha256(data).hexdigest()))
    report = dict(records=args.records, trials=rows,
                  before_sha256=hashlib.sha256(args.before.read_bytes()).hexdigest(),
                  after_sha256=hashlib.sha256(args.after.read_bytes()).hexdigest())
    (args.output / 'measurements.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
