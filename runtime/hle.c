/* Fallback for functions the PC build does not provide yet (gen/out/src/zz_stubs.c). */
#include <windows.h>
#include "runtime.h"

void hle_unimplemented(const char *name)
{
    static const char *last;
    if (name == last) return;
    last = name;
    hw_log(1, "unimplemented: %s", name);
}
