"""Benchmark the Blackfin SPORT transmit capture path before and after patch 11."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time


BASELINE = r'''
static void
bfin_sport_tx_dump_before (const char *path, bu32 base, const void *source,
                           unsigned nr_bytes)
{
  if (path && *path)
    {
      FILE *stream = fopen (path, "ab");
      if (stream)
        {
          unsigned char header[12] =
          {
            'S', 'P', 'T', 'X',
            base, base >> 8, base >> 16, base >> 24,
            nr_bytes, nr_bytes >> 8, nr_bytes >> 16, nr_bytes >> 24,
          };
          fwrite (header, 1, sizeof (header), stream);
          fwrite (source, 1, nr_bytes, stream);
          fclose (stream);
        }
    }
}
'''


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def after_source(path: Path) -> str:
    text = path.read_text()
    begin = text.index("static unsigned\nbfin_sport_tx_dump_stream_close")
    end = text.index("\nstatic unsigned\nbfin_sport_dma_write_buffer", begin)
    return text[begin:end]


def build_harness(directory: Path, label: str, source: str) -> Path:
    call = (
        "bfin_sport_tx_dump_before(bfin_sport_tx_dump_path(), 0x12345678, "
        "bytes, sizeof(bytes));"
        if label == "after"
        else "bfin_sport_tx_dump_before(path, 0x12345678, bytes, sizeof(bytes));"
    )
    if label == "after":
        source = source.replace("bfin_sport_tx_dump (const char *path,", "bfin_sport_tx_dump_before (const char *path,")
    harness = directory / f"{label}.c"
    harness.write_text(
        "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
        "typedef uint32_t bu32;\n"
        + source
        + r'''
int main(int argc, char **argv) {
    unsigned char bytes[64] = {0};
    if (argc != 3) return 2;
    unsigned count = strtoul(argv[2], NULL, 10);
    const char *path = argv[1];
    (void)path;
    for (unsigned i = 0; i < count; ++i) {
        bytes[0] = i;
        bytes[1] = i >> 8;
        CALL(path, bytes);
    }
    return 0;
}
'''.replace("CALL(path, bytes);", call)
    )
    output = directory / label
    subprocess.run(["cc", "-std=c11", "-O3", "-Wall", "-Wextra", "-Werror",
                    str(harness), "-o", str(output)], check=True)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, default=Path("build/gdb-17.2/sim/bfin/dv-bfin_ppi.c"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--records", type=int, default=100_000)
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    before = build_harness(args.output, "before", BASELINE)
    after = build_harness(args.output, "after", after_source(args.after))
    rows = []
    for trial in range(args.trials):
        labels = ("before", "after") if trial % 2 == 0 else ("after", "before")
        for label in labels:
            output = args.output / f"{label}-{trial}.capture"
            started = time.perf_counter()
            env = {"BFIN_SPORT_TX_OUTPUT": str(output)}
            subprocess.run([str(before if label == "before" else after), str(output),
                            str(args.records)], env=env, check=True)
            elapsed = time.perf_counter() - started
            expected = args.records * (12 + 64)
            if output.stat().st_size != expected:
                raise RuntimeError(f"{label} wrote {output.stat().st_size}, expected {expected}")
            rows.append({"label": label, "trial": trial, "seconds": elapsed,
                         "bytes": expected, "sha256": digest(output)})
    medians = {label: statistics.median(r["seconds"] for r in rows if r["label"] == label)
               for label in ("before", "after")}
    report = {
        "records": args.records,
        "trials": rows,
        "before_source_sha256": digest(args.before),
        "after_source_sha256": digest(args.after),
        "median_seconds": medians,
        "speedup": medians["before"] / medians["after"],
        "time_reduction_percent": 100 * (1 - medians["after"] / medians["before"]),
    }
    (args.output / "measurements.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
