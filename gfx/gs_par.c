/* gs_par.c - run the rows of one large primitive on several cores. Each primitive still finishes before the
 * next one starts, and every pixel is drawn by exactly one thread, so the picture is identical to a single-
 * threaded draw. */
#include <windows.h>
#include <stdlib.h>
#include "gfx_internal.h"
#include "gs_par.h"
#include <xmmintrin.h>

/* Drawing runs under the default SSE mode (round to nearest, no flush-to-zero) on every thread: the caller may
 * arrive with the VU's round-toward-zero mode still set (an XGKICK draws from inside a VU1 program), and its rows
 * must come out as the workers' would. */
#define CALLER_BEGIN unsigned int csr_ = _mm_getcsr(); _mm_setcsr(0x1F80u | (csr_ & 0x3Fu))
#define CALLER_END   _mm_setcsr(csr_)

#define MAX_WORKERS 15
#define CHUNK_ROWS  4

static HANDLE workers[MAX_WORKERS];
static int nworkers, inited;
static volatile LONG job_gen;       /* bumped per job; workers wait on it */
static volatile LONG next_row, rows_done;
static volatile LONG quitting;
static gs_rows_fn job_fn;
static void *job_ctx;
static int job_y0, job_y1;
static gs_all_fn all_fn;           /* set: every thread runs all_fn(ctx, k, n) once */
static volatile LONG all_next, all_done;
static int all_async;               /* the running all_fn job leaves the caller out */

static void run_chunks(void)
{
    if (all_fn) {
        LONG k = InterlockedIncrement(&all_next);   /* 1..nworkers; the caller is 0 */
        if (k <= nworkers) {
            if (all_async) all_fn(job_ctx, (int)k - 1, nworkers); else all_fn(job_ctx, (int)k, nworkers + 1);
            InterlockedIncrement(&all_done);
        }
        return;
    }
    for (;;) {
        LONG r = InterlockedExchangeAdd(&next_row, CHUNK_ROWS);
        int ya = job_y0 + (int)r, yb;
        if (ya >= job_y1) break;
        yb = ya + CHUNK_ROWS;
        if (yb > job_y1) yb = job_y1;
        job_fn(job_ctx, ya, yb);
        InterlockedExchangeAdd(&rows_done, yb - ya);
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    LONG seen = 0;
    (void)arg;
    _mm_setcsr(0x1F80u);
    for (;;) {
        LONG g = job_gen;
        while (g == seen) {
            WaitOnAddress(&job_gen, &seen, sizeof seen, INFINITE);
            g = job_gen;
        }
        seen = g;
        if (quitting) return 0;
        run_chunks();
    }
}

void gs_par_init(int threads)
{
    int i;
    SYSTEM_INFO si;
    if (inited) return;
    inited = 1;
    {
        char buf[16];
        DWORD n = GetEnvironmentVariableA("DOTR_GFX_THREADS", buf, sizeof buf);
        if (n && n < sizeof buf && atoi(buf) > 0) threads = atoi(buf);
    }
    if (threads <= 0) {
        GetSystemInfo(&si);
        /* leave four logical CPUs to the game thread, sound and Windows; measured best at 12 on 16 */
        threads = (int)si.dwNumberOfProcessors - 4;
        if (threads > 12) threads = 12;
        if (threads < 1) threads = 1;
    }
    for (i = 0; i < threads - 1 && i < MAX_WORKERS; i++) {
        workers[nworkers] = CreateThread(NULL, 64 * 1024, worker, NULL, 0, NULL);
        if (workers[nworkers]) nworkers++;
    }
}

void gs_par_shutdown(void)
{
    int i;
    gs_par_wait();
    inited = 0;
    if (!nworkers) return;
    quitting = 1;
    InterlockedIncrement(&job_gen);
    WakeByAddressAll((PVOID)&job_gen);
    WaitForMultipleObjects((DWORD)nworkers, workers, TRUE, 2000);
    for (i = 0; i < nworkers; i++) CloseHandle(workers[i]);
    nworkers = 0;
    quitting = 0;
}

int gs_par_threads(void) { return nworkers + 1; }
int gs_par_busy(void) { return all_async && all_done < nworkers; }

void gs_par_wait(void)
{
    if (!all_async) return;
    while (all_done < nworkers) YieldProcessor();
    all_fn = NULL; all_async = 0;
    MemoryBarrier();
}

void gs_par_all_async(gs_all_fn fn, void *ctx)
{
    gs_par_wait();
    if (!nworkers) { CALLER_BEGIN; fn(ctx, 0, 1); CALLER_END; return; }
    job_ctx = ctx; all_next = 0; all_done = 0;
    all_async = 1;
    all_fn = fn;
    MemoryBarrier();
    InterlockedIncrement(&job_gen);
    WakeByAddressAll((PVOID)&job_gen);
}

void gs_par_all(gs_all_fn fn, void *ctx)
{
    gs_par_wait();
    CALLER_BEGIN;
    if (!nworkers) { fn(ctx, 0, 1); CALLER_END; return; }
    job_ctx = ctx; all_next = 0; all_done = 0;
    all_fn = fn;
    MemoryBarrier();
    InterlockedIncrement(&job_gen);
    WakeByAddressAll((PVOID)&job_gen);
    fn(ctx, 0, nworkers + 1);
    CALLER_END;
    while (all_done < nworkers) YieldProcessor();
    all_fn = NULL;
    MemoryBarrier();
}

void gs_par_rows(int y0, int y1, int width, gs_rows_fn fn, void *ctx)
{
    if (y1 <= y0) return;
    gs_par_wait();
    if (!nworkers || (y1 - y0) * width < 8192 || y1 - y0 < 2 * CHUNK_ROWS) {
        CALLER_BEGIN;
        fn(ctx, y0, y1);
        CALLER_END;
        return;
    }
    job_fn = fn; job_ctx = ctx; job_y0 = y0; job_y1 = y1;
    next_row = 0; rows_done = 0;
    MemoryBarrier();
    InterlockedIncrement(&job_gen);
    WakeByAddressAll((PVOID)&job_gen);
    {
        CALLER_BEGIN;
        run_chunks();
        CALLER_END;
    }
    while (rows_done < y1 - y0) YieldProcessor();
    MemoryBarrier();
}
