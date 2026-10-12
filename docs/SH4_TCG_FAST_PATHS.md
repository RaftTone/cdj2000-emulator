# SH-4 MAIN translation fast paths (2026-10-05)

`patches/qemu-sh4-tcg-fast-paths.patch`, adapted from Stijn Jacobs'
cdj-nxs2-qemu (see `patches/README.md` for the knobs). Profiled first, on
stock NXS with `--dsp-model` and a real rekordbox USB export, boot -> Not Loaded, USB mount, browse,
Enter -> loaded, with macOS `sample` on the QEMU process and the ported
`CDJ_JCPROF`.

What the profile said (TCG thread, unpatched):

* The vCPU never idles: MAIN's idle state is a busy loop, ~50 % of sampled PCs
  in `strlen` (`0x04388e48`) and `strncmp` (`0x04388500`) called from
  `0x04311438..0x0431144e`. TB lookups ran at ~35 M/s, split evenly between
  `jsr` and `rts`, with 99.6 % jump-cache hits: the cost is the
  `helper_lookup_tb_ptr` call itself (12-15 % self, 18-21 % inclusive), not
  qht misses.
* Stores into pages holding code: `notdirty_write` ->
  `tb_invalidate_phys_range_fast` was 11 % of the boot-phase thread
  (`page_collection_lock`, QTree, malloc), and 8.5 M such stores in a run
  overlapped no TB at all.
* MMIO dispatch (~4 %), `cpu_io_recompile` (<1 %), `helper_ld_fpscr` (1 %) and
  lock waits (3 %) were minor.

Throughput, MAIN alone under `-icount shift=2,sleep=off` (4 ns per
instruction, so guest seconds per host second is instruction throughput),
same firmware, card and `CDJ_NXS_DSP_MODEL=1`, 30 host seconds:

| build | guest s / host s (30 s) | host s to guest 1 s | to guest 32 s |
|---|---:|---:|---:|
| before (two runs) | 4.38, 4.62 | 1.66, 1.66 | 7.98, 8.23 |
| after (three runs) | 5.51, 5.31, 5.38* | 0.85, 0.86, 0.86 | 6.40, 6.43, 6.68 |
| after, all knobs =0 | 4.67 | 1.66 | 8.24 |
| after, `CDJ_SMC_FASTREJECT=0` | 4.56 | 1.66 | 7.97 |
| after, `CDJ_TB_XPAGE=0` / `CALLPRED=0` / `FPSCR=0` | 5.34 / 5.14 / 5.23 | 0.85-0.87 | 6.67-6.68 |

(*the third "after" run included a since-removed return-prediction
experiment.) The store fast-reject is the large term: the first guest second
of boot halves (1.66 -> 0.86 s) and the run gains ~15-20 %; call prediction,
cross-page chaining and FPSCR chaining are a few percent each, at the noise
floor of this busy host. With `CDJ_JCPROF`, `jsr` lookups fall from 428 M to
17 M per run.

Milestones, full two-board scenario (wall seconds; boot from launch to the
`Not Loaded.` screen, mount from the USB key to the USB browser, Enter ->
loaded to the command-5 duration reply), `--gui-head-start 2.0`, alternating
runs:

| build | boot | mount | browse | Enter -> loaded | QEMU CPU s (whole run) |
|---|---|---|---|---|---|
| before (4) | 7.31-7.65 (7.44) | 0.27-0.29 | 0.27-0.28 | 0.86-0.92 | 24.6-25.0 |
| after (4) | 6.81-7.86 (7.23) | 0.27-0.29 | 0.27-0.28 | 0.87-1.08 | 22.8-24.4 |

With `--dsp-model` none of these is bound by SH-4 speed: mount and load are
sub-second either way and boot is set by the simulated GUI and the link
handshake, so the gain shows as throughput, not latency. The Not Loaded and
USB-browser screens are byte-identical between builds; the loaded screen
matches outside the blinking NOW LOADING text in every run where it was not
mid-redraw (one in four for both builds).
