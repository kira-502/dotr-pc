/*
 * rc_test.h - force-included into the recompiler tests (translated code and interpreter alike).
 * Memory is a 1 MB arena: every PS2 address is masked into it, so random pointers in random states are safe.
 * Code is fetched from the test's own copy, so a random store cannot change what the interpreter runs.
 */
#ifndef DOTR_RC_TEST_H
#define DOTR_RC_TEST_H
#include <stdint.h>
#include <setjmp.h>

#define RC_GENERIC_EXT 1
#define RC_TEST_MASK 0xFFFFFu
/* the arena has 16 spare bytes past the mask: an access at its last bytes runs into them (no wrap) */
#define RC_TEST_SPAN (RC_TEST_MASK + 1 + 16)
extern uint8_t *rc_test_arena;
#define RC_ADDR(a) (rc_test_arena + ((uint32_t)(a) & RC_TEST_MASK))
uint32_t rc_test_fetch(uint32_t pc);
#define RC_FETCH(pc) rc_test_fetch(pc)

extern uint64_t rc_test_ticks, rc_test_budget;
extern jmp_buf rc_test_jmp;
#define RC_ENTER() RC_TICK()
#define RC_TICK() do { if (++rc_test_ticks > rc_test_budget) longjmp(rc_test_jmp, 1); } while (0)

static inline float rc_test_fnan(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    if ((v.u & 0x7F800000u) == 0x7F800000u && (v.u & 0x007FFFFFu)) v.u = 0xFFC00000u;
    return v.f;
}
#define RC_FNAN(x) rc_test_fnan(x)

#ifdef RC_DIFF_TRACE
void rc_tr(int which, uint32_t pc);
struct RcCpu; void rc_trc(int w, struct RcCpu *c, unsigned pc);
#endif

#include "rc.h"

typedef struct RcUnitTest {
    const char *name;
    const uint32_t *words;
    uint32_t nwords;
    const RcFunc *funcs;
    uint32_t nfuncs;
} RcUnitTest;

#endif
