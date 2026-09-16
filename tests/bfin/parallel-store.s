/* SPDX-License-Identifier: GPL-2.0-or-later
 * Every parallel slot must read the pre-instruction data registers.
 * This sequence reproduces the firmware's bitmap-address relocation idiom.
 */
.global _start
.text
_start:
    P1 = 0x2010 (Z);
    P0 = 4 (Z);
    I0 = P1;
    M0 = P0;
    R0 = 1;
    R1 = 10;
    R2 = 7;
    R3 = 99;
    [P1] = R3;
    P1 = 0x2000 (Z);
    R2 = R1 + R0 (NS) || [P1 ++ P0] = R2 || R0 = [I0 ++ M0];
    P1 = 0x2000 (Z);
    R3 = [P1];
    DBGAL (R3, 7);
    DBGAL (R2, 11);
    DBGAL (R0, 99);
    /* Packed ALU case fixed by patch 01 must also defer its store. */
    P5 = 0x2020 (Z);
    R0 = 7;
    R0 = R0 -|- R0 || [P5] = R0 || NOP;
    R3 = [P5];
    DBGAL (R3, 7);
    DBGAL (R0, 0);
    R0 = 0;
    P0 = 1;
    EXCPT 0;
