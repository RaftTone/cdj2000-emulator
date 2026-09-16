/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_SP_H
#define CDJ_C674X_SP_H
#include <stdbool.h>
#include <stdint.h>
/* Pure binary32 semantics for the C674x .L/.M/.S floating-point instructions,
 * SPRUFE8B July 2010.  No CdjC674x access: see cdj_c674x_sp.c.
 *
 * status is the warning mask the caller ORs into FADCR/FAUCR/FMCR, already in
 * the low-half (.1 unit) bit positions; the caller shifts it by 16 for a .2
 * unit.  rmode is the two-bit FADCR/FMCR rounding mode: 0 nearest-even,
 * 1 toward zero, 2 toward +infinity, 3 toward -infinity (Table 2-25). */
typedef struct { uint32_t value, status; } CdjC674xSpResult;
/* INTSP/INTSPU: 32-bit integer to binary32.  *inexact reports a lost bit. */
uint32_t cdj_c674x_integer_to_sp(uint32_t source, bool signed_source,
                                 unsigned rmode, bool *inexact);
/* CMPEQSP/CMPGTSP/CMPLTSP: relation 0 equal, 1 greater-or-equal, 2 less. */
CdjC674xSpResult cdj_c674x_compare_sp(uint32_t left, uint32_t right,
                                      unsigned relation);
/* ADDSP/SUBSP: operation 0 add, 1 src1-src2, 2 src2-src1.  The warning bits
 * always describe the encoded src1/src2 fields, including reversed .S SUBSP. */
CdjC674xSpResult cdj_c674x_add_sub_sp(uint32_t source1, uint32_t source2,
                                      unsigned operation, unsigned rmode);
/* MPYSP. */
CdjC674xSpResult cdj_c674x_multiply_sp(uint32_t left, uint32_t right,
                                       unsigned rmode);
/* SPINT/SPTRUNC: binary32 to 32-bit integer. */
CdjC674xSpResult cdj_c674x_sp_to_integer(uint32_t source, unsigned rmode);
#endif
