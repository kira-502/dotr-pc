/* gs_par.h - row-parallel drawing of one primitive (gs_par.c). */
#ifndef DOTR_GS_PAR_H
#define DOTR_GS_PAR_H

typedef void (*gs_rows_fn)(void *ctx, int y0, int y1);

void gs_par_init(int threads);      /* 0 = choose from the CPU; 1 = no worker threads */
void gs_par_shutdown(void);
/* Run fn over rows [y0, y1): split across the workers when the area is large, else in the calling thread. */
void gs_par_rows(int y0, int y1, int width, gs_rows_fn fn, void *ctx);

/* Run fn(ctx, k, n) once on each of the n threads (k = 0 is the caller) and wait for all of them. */
typedef void (*gs_all_fn)(void *ctx, int k, int n);
void gs_par_all(gs_all_fn fn, void *ctx);
/* The same on the worker threads only (k = 0..n-1 over the n workers), returning at once; gs_par_wait waits for
 * it. Every other gs_par call waits for a running asynchronous job first. Needs at least one worker. */
void gs_par_all_async(gs_all_fn fn, void *ctx);
void gs_par_wait(void);
int  gs_par_threads(void);
int  gs_par_busy(void);     /* an asynchronous job is still running */

#endif
