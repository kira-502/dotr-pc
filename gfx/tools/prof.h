/* prof.h - timing and a poor man's profiler for the windowless gfx tools (gfxbench, gfxreplay). */
#ifndef DOTR_GFX_PROF_H
#define DOTR_GFX_PROF_H

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static double now_ms(void)
{
    LARGE_INTEGER c, fq;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&fq);
    return (double)c.QuadPart * 1000.0 / (double)fq.QuadPart;
}

/* Poor man's profiler (DOTR_GFX_PROFILE=1, run with DOTR_GFX_THREADS=1): sample the main thread's EIP every
 * millisecond; print the hottest addresses, to resolve with gfxbench.map. */
static HANDLE prof_main;
static volatile int prof_on;
static unsigned prof_hits[1 << 16], prof_base;
static DWORD WINAPI prof_thread(LPVOID a)
{
    (void)a;
    while (prof_on) {
        static __declspec(align(16)) CONTEXT c;
        double until = now_ms() + 0.5;
        while (now_ms() < until) YieldProcessor();
        SuspendThread(prof_main);
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(prof_main, &c)) {
            unsigned o = ((unsigned)c.Eip - prof_base) >> 4;
            if (o < (1u << 16)) prof_hits[o]++;
        }
        ResumeThread(prof_main);
    }
    return 0;
}
static void prof_start(void)
{
    if (!getenv("DOTR_GFX_PROFILE")) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
    prof_base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    prof_on = 1;
    timeBeginPeriod(1);
    CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
}
static void prof_stop(void)
{
    unsigned i, k, total = 0;
    if (!prof_on) return;
    prof_on = 0;
    Sleep(5);
    for (i = 0; i < (1u << 16); i++) total += prof_hits[i];
    printf("profile: %u samples; hottest (module offset, 16-byte bucket):\n", total);
    for (k = 0; k < 400; k++) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < (1u << 16); i++) if (prof_hits[i] > best) { best = prof_hits[i]; bi = i; }
        if (!best) break;
        printf("  %08x %5.1f%%\n", 0x400000u + bi * 16, 100.0 * best / total);   /* as in the .map (base 0x400000) */
        prof_hits[bi] = 0;
    }
}

#endif
