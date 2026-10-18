/* rc_interp.h - the reference EE interpreter used to test the module recompiler (see rc_interp.c). */
#ifndef DOTR_RC_INTERP_H
#define DOTR_RC_INTERP_H
#include "rc.h"

#define RC_INTERP_SENTINEL 0xFFFF0000u
enum { RC_INTERP_DONE = 0, RC_INTERP_BUDGET = 1, RC_INTERP_BAD = 2 };

/* Run from entry until the function returns to RC_INTERP_SENTINEL (set in $ra). Code outside [mod_lo, mod_hi)
 * is the main program and goes to rc_call_ext. *used = instructions run. */
int rc_interp(RcCpu *c, uint32_t entry, uint32_t mod_lo, uint32_t mod_hi, uint64_t budget, uint64_t *used);

#endif
