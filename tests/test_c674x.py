"""Compile and execute architecture-level tests without proprietary firmware."""
from pathlib import Path
import shutil
import subprocess
import pytest
ROOT = Path(__file__).resolve().parents[1]

def test_c6747_spi_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'spi-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-spi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_cache_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'cache-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-cache.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_cache.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_edma_registers_and_transfers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'edma-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-edma.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_timer64p_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'timer-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_interrupt_controller(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'intc-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
        '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_emifb_configuration(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'emifb-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-emifb.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_emifb.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_hpi_control(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'hpi-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-hpi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_hpi.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_dsp_checkpoint_round_trip(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'dsp-checkpoint-test'
    checkpoint = tmp_path / 'state.cdjdsp'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/dsp-checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_cache.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_scheduler.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary), str(checkpoint)], check=True, timeout=5)

def test_c6747_pll_cycle_clock(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'pll-clock-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-pll-clock.c'),
        *[str(ROOT / 'emulator/qemu' / name) for name in
          ('cdj_c6747_pll.c', 'cdj_c6747_timer.c', 'cdj_c6747_mcasp.c',
           'cdj_c674x.c', 'cdj_c674x_uncond.c', 'cdj_c674x_mpy.c', 'cdj_c674x_dotp.c', 'cdj_c674x_packed8.c', 'cdj_c674x_packed16.c', 'cdj_c674x_packbits.c', 'cdj_c674x_mpy32.c', 'cdj_c674x_dp.c', 'cdj_c674x_approx.c',
           'cdj_c674x_sp.c', 'cdj_c674x_control.c', 'cdj_c674x_loop.c')],
        '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_pll_configuration(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'pll-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-pll.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_pll.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_i2c_gpio_mode(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'i2c-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-i2c.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_i2c.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_gpio_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'gpio-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-gpio.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_gpio.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_mcasp_pin_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'mcasp-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-mcasp.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_psc_transitions(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'psc-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-psc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_psc.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c674x_packets_and_branch_delays(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)


def test_c674x_nonconditional_encodings(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-uncond-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x-uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_c674x_loop_schedule(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-loop-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x-loop.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)


def test_c6747_syscfg_unlock_and_pipeline(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c6747-syscfg-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_pll.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)


def test_c674x_dispatch_table_has_no_shadowed_rows(tmp_path):
    """No row of cdj_c674x_arms[] may be unreachable behind an earlier row.

    Arm selection is first-match-wins, so a new row whose mask/match overlaps an
    earlier row's never runs and nothing in the build complains - the
    instruction it was added for keeps reporting whatever the earlier row does.
    The table comment asks for an instruction sweep before adding a row; overlap
    has a closed form instead, checked here over every pair:

        ((match_i ^ match_j) & mask_i & mask_j) == 0  =>  some word matches both

    An overlapping pair is legitimate only when one row carries an `also`
    predicate, which is how the ladder's remaining condition is expressed.  A
    pair where NEITHER row is predicated is a hard shadow and fails.

    This is the gate for adding instructions to the table: it turns a silent
    mis-ordering into a red test.  The predicated-overlap count is asserted too,
    so a new unpredicated row cannot hide by being miscategorised.
    """
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'arm-table-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/c674x-arm-table.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
        '-o', str(binary), '-lm'], check=True)
    out = subprocess.run([str(binary)], check=True, timeout=30,
                         capture_output=True, text=True).stdout
    lines = out.splitlines()
    shadows = [line for line in lines if line.startswith('shadow ')]
    assert not shadows, 'shadowed dispatch rows:\n' + '\n'.join(shadows)
    assert 'hard-shadows 0' in lines, out
    # Some pairs overlap on mask/match alone and are separated only by an `also`
    # predicate.  That is consistent with the table comment's sweep, which
    # evaluated `also` and found no word claimed twice; this check deliberately
    # does not evaluate `also`, so it over-reports rather than under-reports.
    # The count is pinned as a ratchet: a new row that overlaps and is NOT
    # predicated pushes hard-shadows above 0 and fails outright, while a new
    # predicated row moves this number and must be changed here deliberately,
    # together with the `also` predicate that justifies the overlap.
    predicated = [line for line in lines if line.startswith('predicated-overlaps ')]
    # The GMPY/XORMPY row also overlaps one generic .M mask but its opfield
    # predicate keeps the claims disjoint.
    assert predicated == ['predicated-overlaps 254'], out


def test_c674x_packed_dot_products(tmp_path):
    """DOTP2/DOTPN2/DOTP(N)R(SU|US)2/DOTP(SU|US|U)4, SPRUFE8B pp235-253.

    Expected values are transcribed from the manual's own worked examples,
    including the "4 cycles after instruction" latency; the two rounded forms
    halt where the manual prints "result undefined".
    """
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-dotp-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x-dotp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)


def test_c674x_packed_8bit(tmp_path):
    """Packed 8-bit (4x8) .L/.S/.M semantics, dispatch and pipeline latency.
    Every expected register value in the cstub is transcribed from the worked
    example of the instruction's own SPRUFE8B entry, with the printed page
    quoted beside it; the cstub's header states the two exceptions.
    """
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-packed8-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/c674x-packed8.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_c674x_packed16_arithmetic(tmp_path):
    """Packed 16-bit arithmetic, compares and shifts against SPRUFE8B examples.
    Every expected value in the cstub is transcribed from a printed "N cycles
    after instruction" block, and every instruction word from assembling the
    manual's own example line with asm6x -mv6740.
    """
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-packed16-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/c674x-packed16.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_c674x_pack_unpack_shuffle_and_bit_manipulation(tmp_path):
    """UNPKHU4/UNPKLU4/SWAP4/BITR/BITC4/DEAL/SHFL/SHFL3/XPND2/XPND4/ROTL/LMBD/
    NORM/SHLMB/SHRMB/DPACK2/DPACKX2 against SPRUFE8B's own worked examples."""
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-packbits-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/c674x-packbits.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_c674x_double_precision(tmp_path):
    """Double-precision semantics, delay slots and FADCR/FAUCR/FMCR effects.

    Expected values are transcribed from the SPRUFE8B per-instruction Example
    blocks and special-case tables, or derived from the notes on those pages
    where no example exists; the cstub says which, per case.  Nothing here
    comes from running this emulator.  RCPDP/RCPSP/RSQRDP/RSQRSP are asserted
    to stay unimplemented: the manual fixes only that their mantissa is
    "accurate to the eighth binary position".
    """
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-dp-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x-dp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)


def test_c674x_32bit_multiply_galois_and_long_forms(tmp_path):
    """MPYI/MPYID/MPY2/GMPY4/GMPY/XORMPY, DMV, SAT, SUBC, ABS, the 40-bit CMP*/SH* forms,
    B NRP and BPOS, against SPRUFE8B's own per-instruction examples."""
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-mpy32-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x-mpy32.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_c674x_no_word_reaches_two_dispatch_rows(tmp_path):
    """No instruction word may be claimed by two rows of cdj_c674x_arms[].

    test_c674x_dispatch_table_has_no_shadowed_rows answers a weaker question: it
    compares mask/match only and so over-reports, flagging 251 pairs that are
    separated solely by an `also` predicate.  That check cannot tell a genuine
    double claim from a pair the predicates keep apart, and the table comment's
    original assurance came from a sweep that was never re-run as the table grew
    from 120 rows to 211.

    This is that sweep, done exhaustively rather than by sampling and without
    needing 2^32 words: for each pair whose mask/match already agree, every bit
    in mask_i | mask_j is fixed by the two matches, so only the remaining bits
    are free, and enumerating those covers the pair's whole overlap region.
    Every `also` predicate is a pure function of the instruction word, which is
    checked rather than assumed before the sweep runs.

    Zero skipped pairs is asserted alongside zero double claims: a pair skipped
    for having too many free bits is a hole in the coverage, and a run that
    skipped everything would otherwise look identical to a clean one.
    """
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'arm-claims-test'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/c674x-arm-claims.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
        '-o', str(binary), '-lm'], check=True)
    out = subprocess.run([str(binary)], check=True, timeout=300,
                         capture_output=True, text=True).stdout
    lines = out.splitlines()
    claims = [line for line in lines if line.startswith('double-claim ')]
    assert not claims, 'words reaching two dispatch rows:\n' + '\n'.join(claims)
    assert 'double-claims 0' in lines, out
    assert 'skipped 0' in lines, out
    assert 'word-only-probes 4096' in lines, out
    # Pin the pair count so this stays tied to the mask/match check above.
    # The unconditional GMPY/XORMPY and RPACK2 rows contribute to the count.
    assert 'pairs-examined 308' in lines, out
