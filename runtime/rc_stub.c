/* Stand-in for lane R's recompiled modules (recomp/rc_port.h) when recomp/ is not part of the build: no module is
 * switched in (-1), so runtime/overlay.c leaves the overlay area alone and module calls are skipped. */
#include <stddef.h>
#include "rc_port.h"

void rc_thread_init(uint32_t ps2_stack_top) { (void)ps2_stack_top; }
int rc_overlay_switch(void) { return -1; }
const char *rc_current_module(void) { return NULL; }
int rc_check_modules(int (*find)(const char *, uint32_t *, uint32_t *), int (*read)(uint64_t, uint32_t, void *), char *msg, int len)
{
    (void)find; (void)read;
    if (len > 0) { const char *t = "no recompiler in this build"; int i; for (i = 0; t[i] && i < len - 1; i++) msg[i] = t[i]; msg[i] = 0; }
    return -1;
}
int rc_oracle_init(int (*find)(const char *, uint32_t *, uint32_t *), int (*read)(uint64_t, uint32_t, void *),
                   char *msg, int len)
{
    (void)find; (void)read;
    if (len > 0) msg[0] = 0;
    return 1;
}
int rc_oracle_run(uint32_t addr, const uint64_t *iargs, int ni, const float *fargs, int nf, int deep,
                  RcOracleResult *r)
{
    (void)addr; (void)iargs; (void)ni; (void)fargs; (void)nf; (void)deep;
    r->status = -1;
    r->msg[0] = 0;
    return -1;
}
const char *rc_oracle_name(uint32_t addr) { (void)addr; return NULL; }
void rc_oracle_hook(uint32_t addr, int ni, const uint64_t *iargs, int nf, const float *fargs)
{
    (void)addr; (void)ni; (void)iargs; (void)nf; (void)fargs;
}
void rc_oracle_totals(unsigned *same, unsigned *differ, unsigned *skipped) { *same = *differ = *skipped = 0; }
int rc_oracle_busy;
void rc_oracle_abort(void) {}
unsigned rc_oracle_checks;
