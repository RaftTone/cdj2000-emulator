; SPDX-License-Identifier: GPL-2.0-or-later
;
; SPRUH91D 28.1.5.4.2.2.4 "32-Bit Timer Unchained Mode Configuration
; Procedure", printed page 1238, for timer 1:2 of Timer64P0, then an endless
; loop so the timer has something to run underneath.  Assembled by TI's cl6x so
; that no instruction encoding in this program comes from our own core.
;
; Timer64P0 registers are at 01C2 0000h (SPRUH91D Table 28-16 register map, and
; the C6747 datasheet SPRS377F peripheral memory map):
;   TGCR        +24h   TIMMODE = 1h dual 32-bit unchained, TIM12RS = 1  -> 05h
;   PRD12       +18h   period, programmed with "the desired timer period value
;                      - 1" per the procedure's step 3                 -> 20h
;   INTCTLSTAT  +44h   PRDINTEN12 (bit 0) enables interrupt generation  -> 01h
;   TCR         +20h   ENAMODE12 = 2h continuous, TCR bits 7-6         -> 80h
;
; --no_compress keeps every instruction 32 bits wide so the words can be laid
; straight into an L2 image.  The NOP 4s cover the store's delay slots rather
; than relying on the assembler to pack anything.
        .sect ".text"
        .global _start
_start:
        MVKL    0x01c20024, B4
        MVKH    0x01c20024, B4
        MVK     0x05, B5
        STW     B5, *B4
        NOP     4
        MVKL    0x01c20018, B4
        MVKH    0x01c20018, B4
        MVK     0x20, B5
        STW     B5, *B4
        NOP     4
        MVKL    0x01c20044, B4
        MVKH    0x01c20044, B4
        MVK     0x01, B5
        STW     B5, *B4
        NOP     4
        MVKL    0x01c20020, B4
        MVKH    0x01c20020, B4
        MVK     0x80, B5
        STW     B5, *B4
        NOP     4
spin:   B       spin
        NOP     5
