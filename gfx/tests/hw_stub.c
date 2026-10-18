/* hw_stub.c - minimal hw_* runtime for gfx's own tests and tools (the real ones live in runtime/). */
#include <stdarg.h>
#include <stdio.h>
#include "hw.h"

int hw_stub_intc[16];
int hw_stub_dmac[16];
int hw_stub_quiet;
static hw_u32 vblanks;

void hw_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (hw_stub_quiet && level > 1) return;
    va_start(ap, fmt);
    fprintf(stderr, "[%d] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void hw_raise_intc(int source) { if (source >= 0 && source < 16) hw_stub_intc[source]++; }
void hw_raise_dmac(int channel) { if (channel >= 0 && channel < 16) hw_stub_dmac[channel]++; }
hw_u32 hw_vblank_count(void) { return vblanks++; }
