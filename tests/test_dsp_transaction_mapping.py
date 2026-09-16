"""Map prefilters must never reject an accepted device write."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_transaction_prefilters_cover_registers_and_exclude_ram(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    source = tmp_path / 'mapping.c'
    source.write_text('''
#include <assert.h>
#include "cdj_c6747_edma.h"
#include "cdj_c6747_mcasp.h"
int main(void) {
    CdjC6747Edma edma;
    CdjC6747McaspControl mcasp = {0};
    cdj_c6747_edma_reset(&edma);
    for (uint32_t a=0x01bffff8; a<0x01c08008; a++)
        for (unsigned size=0; size<=8; size++) {
            bool mapped = cdj_c6747_edma_write_mapped(a,size);
            assert(mapped == ((size==4 || size==8) && !(a&3) &&
                             a>=0x01c00000 && a<0x01c08000));
            if (cdj_c6747_edma_write(&edma,a,0,size,false,0)) assert(mapped);
        }
    for (uint32_t a=0x01cffff8; a<0x01d0c008; a++)
        for (unsigned size=0; size<=8; size++) {
            bool mapped = cdj_c6747_mcasp_control_write_mapped(a,size);
            assert(mapped == (size==4 && !(a&3) && a>=0x01d00000 && a<0x01d0c000));
            if (cdj_c6747_mcasp_control_write(&mcasp,a,0,size,false)) assert(mapped);
        }
    uint32_t ram[] = {0x00800000,0x11800000,0x80000000,0xc0000000,0xffffffff};
    for (unsigned i=0;i<sizeof(ram)/sizeof(ram[0]);i++)
        for (unsigned size=0;size<=8;size++) {
            assert(!cdj_c6747_edma_write_mapped(ram[i],size));
            assert(!cdj_c6747_mcasp_control_write_mapped(ram[i],size));
        }
    return 0;
}
''')
    binary = tmp_path / 'mapping'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(source),
                    str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


# Every address below is one the captured NXS firmware actually touched and the
# bus model once refused with "unaligned or unmapped scalar memory access".  All
# six are word aligned, so the alignment arm of the fault never applied: each was
# a hole in the peripheral map, and each hole is now backed by a register the
# device really has.  Offsets and reset values come from SPRUH91D, never from
# this emulator's output.  Faults and their recorded addresses:
#
#   pc 0xc004dbe8  store 0x01c0451c  EDMA3CC PaRAM set 40 CCNT
#   pc 0xc004e28e  store 0x01c01028  EDMA3CC EECR
#   pc 0xc004e33c  load  0x01c14110  SYSCFG MSTPRI0
#   pc 0xc004e52c  load  0x01c00314  EDMA3CC QEMCR
#   pc 0xc004e618  store 0x01d04044  McASP1 GBLCTL
#   pc 0xc004f42c  store 0x01e1203c  SPI1 SPIDAT1
def test_recorded_unmapped_fault_addresses_are_real_c6747_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    source = tmp_path / 'recorded.c'
    source.write_text('''
#include <assert.h>
#include "cdj_c6747_edma.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_spi.h"
#include "cdj_c6747_syscfg.h"

/* SPRUH91D Table 16-20 (printed page 507, PDF page 507) lists the EDMA3CC
 * registers by offset; the C6747 places EDMA3CC0 at 0x01c00000. */
#define EDMA 0x01c00000u

static void edma_registers(void)
{
    CdjC6747Edma edma;
    uint32_t v;
    cdj_c6747_edma_reset(&edma);

    /* QEMCR, offset 314h (Table 16-20).  The recorded fault was a load, and
     * the manual marks QEMCR write-only, so only acceptance is asserted here:
     * the readback value is cdj_c6747_edma.c's documented software-compat
     * choice, not a manual-derived expectation. */
    assert(cdj_c6747_edma_read(&edma, EDMA + 0x314u, &v));
    /* Table 16-30 (printed page 518, PDF page 518): QEMCR bits 31-8 are
     * Reserved, so the guard must stay closed above bit 7. */
    assert(cdj_c6747_edma_write(&edma, EDMA + 0x314u, 0xffu, 4, false, 0));
    assert(!cdj_c6747_edma_write(&edma, EDMA + 0x314u, 0x100u, 4, false, 0));

    /* EECR, offset 1028h (Table 16-20, continued on PDF page 508).  Table
     * 16-45 (printed page 534, PDF page 534): "Writes of 1 to the bits in
     * EECR clear the corresponding event bits in EER; writes of 0 have no
     * effect."  EESR is 1030h and EER is 1020h in the same table. */
    assert(cdj_c6747_edma_write(&edma, EDMA + 0x1030u, 0x5u, 4, true, 0));
    assert(cdj_c6747_edma_read(&edma, EDMA + 0x1020u, &v) && v == 0x5u);
    assert(cdj_c6747_edma_write(&edma, EDMA + 0x1028u, 0x4u, 4, true, 0));
    assert(cdj_c6747_edma_read(&edma, EDMA + 0x1020u, &v) && v == 0x1u);
    /* The recorded store wrote 0xffffffff, which must clear every event. */
    assert(cdj_c6747_edma_write(&edma, EDMA + 0x1028u, 0xffffffffu, 4, true, 0));
    assert(cdj_c6747_edma_read(&edma, EDMA + 0x1020u, &v) && v == 0u);

    /* PaRAM occupies 4000h-4FFFh (Table 16-20, printed page 509, PDF page
     * 509); Table 16-11 (printed page 500, PDF page 500) gives CCNT at offset
     * 1Ch of a 32-byte set.  Set 40 CCNT is 0x4000 + 40*32 + 0x1c = 0x451c,
     * and the recorded store wrote 1 there. */
    assert(EDMA + 0x4000u + 40u * 32u + 0x1cu == 0x01c0451cu);
    assert(cdj_c6747_edma_write(&edma, 0x01c0451cu, 1u, 4, true, 0));
    assert(cdj_c6747_edma_read(&edma, 0x01c0451cu, &v) && v == 1u);
}

static void mcasp1_gblctl(void)
{
    /* Table 24-7 (printed page 1037, PDF pages 1036-1038) gives GBLCTL at
     * McASP offset 44h.  The C6747 spaces the three McASP configuration
     * ports 0x4000 apart from 0x01d00000, so McASP1 GBLCTL is 0x01d04044;
     * the recorded store wrote 0. */
    CdjC6747McaspControl mcasp;
    cdj_c6747_mcasp_control_reset(&mcasp);
    assert(cdj_c6747_mcasp_control_write_mapped(0x01d04044u, 4));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d04044u, 0u, 4, true));
}

static void syscfg_mstpri(void)
{
    /* Table 10-1 (printed page 172, PDF page 172) places MSTPRI0-MSTPRI2 at
     * SYSCFG offsets 110h-118h; the C6747 SYSCFG base is 0x01c14000, which is
     * the same base cdj_c6747_syscfg.h already uses for KICK0R (38h),
     * PINMUX0 (120h) and CFGCHIP0 (17Ch).  Reset values and writable fields
     * are read off Figures 10-15, 10-16 and 10-17 (printed pages 186, 187 and
     * 188): every "Reserved" field carries R/W-0, R/W-4h, R/W-5h or R/W-6h,
     * and only the named master fields are software-settable. */
    CdjC6747Syscfg syscfg;
    CdjC6747SyscfgPriority priority;
    uint32_t v;
    cdj_c6747_syscfg_reset(&syscfg);
    cdj_c6747_syscfg_priority_reset(&priority);
    assert(cdj_c6747_syscfg_priority_read(&priority, 0x01c14110u, &v) &&
           v == 0x44442222u);
    assert(cdj_c6747_syscfg_priority_read(&priority, 0x01c14114u, &v) &&
           v == 0x44440000u);
    assert(cdj_c6747_syscfg_priority_read(&priority, 0x01c14118u, &v) &&
           v == 0x54604404u);
    /* 11Ch is past MSTPRI2 and is not in Table 10-1: it must stay unmapped. */
    assert(!cdj_c6747_syscfg_priority_read(&priority, 0x01c1411cu, &v));
    /* MSTPRI0 bits 14-12 are DSP_CFG and 10-8 are DSP_MDMA (Table 10-19),
     * so lowering DSP_CFG to 7h is legal while disturbing the reserved 6-4
     * field is not. */
    assert(cdj_c6747_syscfg_priority_write(&priority, &syscfg, 0x01c14110u,
                                           0x44447222u, 4, false));
    assert(!cdj_c6747_syscfg_priority_write(&priority, &syscfg, 0x01c14110u,
                                            0x44442202u, 4, false));
}

static void spi1_spidat1(void)
{
    /* Table 27-2 (printed page 1175, PDF page 1175) gives SPIDAT1 at SPI
     * offset 3Ch; SPI1's base is 0x01e12000, so the recorded store address
     * 0x01e1203c is SPI1 SPIDAT1.  Only the register's presence is asserted:
     * whether a given SPIDAT1 write completes depends on the enable and the
     * attached endpoint, and cdj_c6747_spi.c fails that closed on purpose. */
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT];
    uint32_t v;
    cdj_c6747_spis_reset(spis);
    assert(CDJ_C6747_SPI1_BASE + 0x3cu == 0x01e1203cu);
    assert(cdj_c6747_spi_wm8740_timed_mapped(0x01e1203cu));
    assert(cdj_c6747_spis_read(spis, 0x01e1203cu, &v));
    /* 30h is not in Table 27-2 and stays unmapped, so the window above is a
     * register list and not a blanket hole. */
    assert(!cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x30u, &v));
}

int main(void)
{
    edma_registers();
    mcasp1_gblctl();
    syscfg_mstpri();
    spi1_spidat1();
    return 0;
}
''')
    binary = tmp_path / 'recorded'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'emulator/qemu'), str(source),
                    str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
