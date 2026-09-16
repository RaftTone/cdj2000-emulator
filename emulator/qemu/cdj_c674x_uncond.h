/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_UNCOND_H
#define CDJ_C674X_UNCOND_H
#include <stdint.h>
/* The C64x+ nonconditional (unconditional) 32-bit encodings.
 *
 * SPRUFE8B Table 3-9 (printed page 77) reserves creg = 000 with z = 1 - but
 * only for instructions that have a creg field.  TI places the nonconditional
 * extensions in exactly that hole: bits 31-28 are the literal 0001 opcode
 * field of Figure C-3 (printed page 724), Figure D-3 (735), Figure E-3 (743),
 * Figure F-12/F-14 (749) and Figure H-1 (765).  A decoder that applies the
 * reserved-predicate test before classifying those bits makes every one of
 * them unreachable, and for the ADDAB/ADDAH/ADDAW long-immediate form it also
 * aliases onto Figure C-5's 15-bit-offset store.
 *
 * This classifier is pure: word in, kind out.  It decides only reachability,
 * so the decoder can reject an unimplemented extension by name instead of
 * calling a valid opcode a reserved predicate.
 */
typedef enum {
    /* Not a nonconditional encoding: creg/z apply as usual. */
    CDJ_C674X_UNCOND_NONE = 0,
    /* ADDAB/ADDAH/ADDAW (.unit) B14/B15, ucst15, dst (printed pages 115,
     * 120, 123); decode with cdj_c674x_adda_long(). */
    CDJ_C674X_UNCOND_ADDAB,
    CDJ_C674X_UNCOND_ADDAH,
    CDJ_C674X_UNCOND_ADDAW,
    /* A documented nonconditional instruction that is not implemented. */
    CDJ_C674X_UNCOND_UNIMPLEMENTED,
    /* A nonconditional instruction the caller's own dispatch table implements.
     * Classifying it is still necessary: without it the word falls into
     * Table 3-9's reserved-predicate hole and never reaches that table. */
    CDJ_C674X_UNCOND_ARM_TABLE,
} CdjC674xUncondKind;
/* CALLP (Figure F-12) and DINT/RINT (Figure H-1, op 0010/0011) share the same
 * 0001 opcode field and are dispatched by the caller before this point; they
 * are deliberately not classified here. */
CdjC674xUncondKind cdj_c674x_uncond_classify(uint32_t word);
typedef struct {
    unsigned dst;     /* destination register number, 0-31 */
    unsigned side;    /* destination register file: 0 = A (.D1), 1 = B (.D2) */
    uint32_t result;  /* B14/B15 + (ucst15 << scale) */
} CdjC674xAddaLong;
/* Operand decode and execution of one ADDAB/ADDAH/ADDAW long-immediate word.
 * Linear arithmetic only (printed pages 115/120/123): AMR never applies, and
 * no memory is touched.  kind must be one of the three ADDA kinds. */
CdjC674xAddaLong cdj_c674x_adda_long(CdjC674xUncondKind kind, uint32_t word,
                                    uint32_t b14, uint32_t b15);
#endif
