"""Benchmark a fixed local firmware continuation; never claims boot correctness.

Run from the repository root. Supply --root for a preserved baseline checkout.
All generated traces/checkpoints remain in --output; only JSON measurements
need to be retained in version control. Run serially on an otherwise idle host.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--formats', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--steps', type=int, default=300000)
    parser.add_argument('replay_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop('PYTHONPATH', None)
    # A per-measurement cache makes trial zero explicitly cold.
    env['CDJ_REPLAY_CACHE'] = str(output / 'build-cache')
    rows = []
    extra = args.replay_args
    if extra[:1] == ['--']:
        extra = extra[1:]
    for trial in range(args.trials):
        run = output / str(trial)
        command = [sys.executable, '-m', 'tools.cdj_dsp.replay',
                   str(args.checkpoint.resolve()), str(run), '--formats',
                   str(args.formats.resolve()), '--steps', str(args.steps),
                   '--verify-repeat', *extra]
        started = time.perf_counter()
        result = subprocess.run(command, cwd=args.root.resolve(), env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        elapsed = time.perf_counter() - started
        (output / f'{trial}.stderr').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors='replace'))
        gate = json.loads((run / 'gate.json').read_text())
        if not gate['passed']:
            raise RuntimeError('repeat gate failed')
        coverage = json.loads((run / 'coverage.json').read_text())
        coverage.pop('sha256', None)
        manifest = json.loads((run / 'manifest.json').read_text())
        rows.append(dict(trial=trial, seconds=elapsed, gate_passed=True,
                         trace_bytes=(run / 'trace.jsonl').stat().st_size,
                         trace_sha256=digest(run / 'trace.jsonl'),
                         checkpoint_sha256=digest(run / 'final.cdjdsp'),
                         coverage_semantic_sha256=hashlib.sha256(json.dumps(
                             coverage, sort_keys=True).encode()).hexdigest(),
                         build=manifest.get('build')))
    report = dict(root=str(args.root.resolve()), steps=args.steps,
                  checkpoint_sha256=digest(args.checkpoint),
                  formats_sha256=digest(args.formats), args=extra, trials=rows)
    (output / 'measurements.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
