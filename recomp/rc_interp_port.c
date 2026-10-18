/* The reference interpreter in the port build, for the oracle (rc_oracle.c): code comes from the disc copy of
 * SLUS_205.15, since tramp.c has written x86 jumps over the start of every function in PS2 memory. */
#include <stdint.h>
uint32_t rc_oracle_fetch(uint32_t pc);
#define RC_FETCH(pc) rc_oracle_fetch(pc)
int rc_oracle_external(uint32_t pc);
#define RC_INTERP_EXTERNAL(pc) rc_oracle_external(pc)
#define RC_INTERP_EE_FLOAT 1
#include "rc_interp.c"
