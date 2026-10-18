/*
 * EE threads, semaphores, interrupt handlers and the 60 Hz vblank, for the HLE kernel (hle/kernel.c).
 *
 * The EE kernel is cooperative between threads: the highest-priority ready thread (lowest number) runs until it
 * sleeps, waits or rotates; equal priorities keep FIFO order. Here each EE thread is a Windows fiber whose code
 * runs on a stack inside the fixed area 0x02000000..0x023FFFFF (some game code ORs mirror bits into stack
 * addresses it hands to DMA, so stacks stay in low memory). When no thread is ready, the scheduler waits for the
 * next vblank, runs the registered vblank/DMA handlers and the graphics vblank, and looks again.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "sif.h"
#include "hle_rt.h"
#include "rc_port.h"

#define MAX_THREADS 32
#define MAX_SEMAS   64
#define MAX_HANDLERS 16

enum { T_FREE, T_DORMANT, T_READY, T_RUN, T_SLEEP, T_WAITSEMA, T_WAITVBLANK };

typedef struct {
    int state, prio, wakeups, sema, seq;
    void (*entry)(void *);
    void *arg;
    LPVOID fiber;
    uint8_t *stack_lo, *stack_hi;
    uint32_t rc_stack;      /* top of this thread's PS2 stack for module code (recomp/rc_port.h) */
} thread_t;

static thread_t th[MAX_THREADS];
static int cur = -1;
static int seq_counter;
static LPVOID sched_fiber;
static uint8_t *stack_bump = (uint8_t *)DOTR_STACKS_BASE;

static struct { int used, count, max; } sem[MAX_SEMAS];

typedef struct { int used, cause, enabled; int (*fn)(int); void *arg; } handler_t;
static handler_t intc[MAX_HANDLERS], dmac[MAX_HANDLERS];
static int intc_mask = 0xFFFF, dmac_mask = 0xFFFF;

static volatile hw_u32 g_vblanks_rt;
static double next_vblank, t0;
static LARGE_INTEGER freq;
int rt_fast;            /* --fast: no real-time pacing (headless runs) */
int rt_fast_forward;    /* held fast-forward key (runtime/pad.c): no real-time pacing while it is down */
unsigned int rt_frame_limit;   /* --frames N: stop cleanly after N vblanks */

double rt_seconds(void)
{
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

hw_u32 rt_vblanks(void) { return g_vblanks_rt; }
unsigned int rt_vblank_count(void) { return g_vblanks_rt; }

/* How far into the current field the game is, 0..1 (the EE timers, runtime/mmio.c): real time since the last vblank
 * in the window; 0 in --fast runs and fast-forward, where only whole fields count (deterministic bot duels) */
static double last_vblank_at;
static int ff_on(void);
double rt_field_phase(void)
{
    double p;
    if (rt_fast || ff_on() || !last_vblank_at) return 0.0;
    p = (rt_seconds() - last_vblank_at) * 59.94;
    return p < 0.0 ? 0.0 : p > 0.999 ? 0.999 : p;
}

/* ---- stacks on fixed addresses ---------------------------------------------------------------- */

static uint8_t *alloc_stack(uint32_t size, uint8_t **lo)
{
    uint8_t *base;
    size = (size + 0xFFFF) & ~0xFFFFu;
    if (size < 0x10000) size = 0x10000;
    if (stack_bump + size > (uint8_t *)(DOTR_STACKS_BASE + DOTR_STACKS_SIZE)) return NULL;
    base = stack_bump;
    stack_bump += size;
    *lo = base;
    return base + size;
}

static void on_stack(uint8_t *top, thread_t *t)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void (*fn)(void *) = t->entry;
    void *arg = t->arg;
    tib->StackBase = top;
    tib->StackLimit = t->stack_lo;
    __asm__ volatile("movl %0, %%esp\n\t"
                     "pushl %2\n\t"
                     "calll *%1\n\t"
                     "calll *%3\n\t"
                     "ud2"
                     :: "r"(top - 16), "r"(fn), "r"(arg), "r"(rt_thread_exit) : "memory");
}

static void CALLBACK fiber_start(void *p)
{
    thread_t *t = (thread_t *)p;
    rc_thread_init(t->rc_stack);       /* this fiber's module CPU state (fiber-local on lane R's side) */
    on_stack(t->stack_hi, t);
}

/* ---- scheduler ---------------------------------------------------------------------------------- */

static void run_handlers(handler_t *hs, int cause, int mask)
{
    int i;
    if (!(mask & (1 << cause))) return;
    for (i = 0; i < MAX_HANDLERS; i++)
        if (hs[i].used && hs[i].enabled && hs[i].cause == cause) hs[i].fn(cause);
}

static void (*vblank_hooks[8])(void);
void rt_on_vblank(void (*fn)(void))
{
    int i;
    for (i = 0; i < 8; i++) if (!vblank_hooks[i]) { vblank_hooks[i] = fn; return; }
}

static void vblank_now(void);
static int ff_on(void);
static void threads_log(void);
static void reschedule(void);

static void vblank_tick(void)
{
    int i;
    g_vblanks_rt++;
    for (i = 0; i < 8 && vblank_hooks[i]; i++) vblank_hooks[i]();
    run_handlers(intc, 2, intc_mask);   /* INTC_VBLANK_S */
    gfx_vblank(g_vblanks_rt & 1);
    run_handlers(intc, 3, intc_mask);   /* INTC_VBLANK_E */
    snd_vblank();
    for (i = 0; i < MAX_THREADS; i++)
        if (th[i].state == T_WAITVBLANK) { th[i].state = T_READY; th[i].seq = ++seq_counter; }
    if (gfx_quit_requested()) rt_stop_ok("window closed");
    if (rt_frame_limit && g_vblanks_rt >= rt_frame_limit) { crash_log_here("frame limit reached"); threads_log(); rt_stop_ok("frame limit reached"); }
}

static int pick(void)
{
    int i, best = -1;
    for (i = 0; i < MAX_THREADS; i++) {
        if (th[i].state != T_READY && th[i].state != T_RUN) continue;
        if (best < 0 || th[i].prio < th[best].prio || (th[i].prio == th[best].prio && th[i].seq < th[best].seq))
            best = i;
    }
    return best;
}

/* runs on the scheduler fiber */
static void scheduler_loop(void)
{
    for (;;) {
        int n = pick();
        double now;
        if (n >= 0) {
            cur = n;
            th[n].state = T_RUN;
            SwitchToFiber(th[n].fiber);
            continue;
        }
        /* nobody ready: next vblank */
        now = rt_seconds();
        if (!rt_fast && !ff_on() && now < next_vblank) {
            double wait = next_vblank - now;
            if (wait > 0.002) Sleep((DWORD)((wait - 0.001) * 1000));
            while (rt_seconds() < next_vblank) YieldProcessor();
        }
        vblank_now();
    }
}

/* DOTR_HITCH=<ms>: log every field that took longer, with its time in disc reads and module switches */
double rt_hitch_switch_s;
static void hitch_check(void)
{
    extern double rt_hitch_disc_s;
    extern unsigned rt_hitch_disc_bytes;
    static double limit = -1, last;
    double now = rt_seconds();
    if (limit < 0) { const char *e = getenv("DOTR_HITCH"); limit = e ? atof(e) / 1000.0 : 0; last = now; }
    if (limit && now - last > limit)
        hw_log(2, "hitch: field %u took %.1f ms: disc %.1f ms (%u KB), module switch %.1f ms", g_vblanks_rt,
               (now - last) * 1000, rt_hitch_disc_s * 1000, rt_hitch_disc_bytes / 1024, rt_hitch_switch_s * 1000);
    last = now;
    rt_hitch_disc_s = rt_hitch_switch_s = 0;
    rt_hitch_disc_bytes = 0;
}

/* fast-forward held (or DOTR_FF_FORCE=1, for measuring it windowless) */
static int ff_on(void)
{
    static int force = -1;
    if (force < 0) force = getenv("DOTR_FF_FORCE") != NULL;
    return rt_fast_forward || force;
}

/* The x87 register stack must be empty here (a call boundary: compiled code keeps no x87 value across a call). A
 * float/double result left unpopped by a caller that took it for an int (Ta_ConvBitLengthToNumericLength's pow,
 * lane G's oracle) fills it, and after 8 every float result is NaN. Free it and say where it first happened. */
static void x87_check(void)
{
    static int told;
    unsigned short env[14];
    __asm__ volatile("fnstenv %0" : "=m"(env));
    __asm__ volatile("fldenv %0" : : "m"(env));    /* fnstenv masks the exceptions: put the control word back */
    if (env[4] == 0xFFFF) return;                   /* tag word: all empty */
    __asm__ volatile("emms");                       /* all registers empty again, control word unchanged */
    if (!told++) {
        hw_log(0, "x87 register stack not empty at field %u (tags %04X): a float result was not taken; freed",
               g_vblanks_rt, env[4]);
        crash_log_here("x87 stack");
    }
}

void report_vblank(void);
static int in_tick;
static void vblank_now(void)
{
    extern int gfx_no_draw;                     /* gfx/gfx.c */
    static int base_no_draw = -1;
    static double last_drawn;
    x87_check();
    report_vblank();                            /* the flight recorder's field line (runtime/report.c) */
    if (base_no_draw < 0) base_no_draw = gfx_no_draw;
    hitch_check();
    next_vblank = (rt_fast || ff_on() ? rt_seconds() : next_vblank) + 1.0 / 59.94;
    if (next_vblank < rt_seconds() - 0.25) next_vblank = rt_seconds();   /* fell far behind: catch up */
    ee_float_check_armed();
    last_vblank_at = rt_seconds();
    in_tick = 1;
    vblank_tick();                              /* shows the field that just ended (if it was drawn) */
    in_tick = 0;
    /* the next field: while fast-forwarding one field in every 100 ms is drawn and shown (10 pictures a second);
     * the others keep the GS state and skip the drawing, the picture read-out and Present */
    if (base_no_draw) gfx_no_draw = 1;
    else if (!ff_on() || rt_seconds() - last_drawn >= 0.1) { gfx_no_draw = 0; last_drawn = rt_seconds(); }
    else gfx_no_draw = 1;
}

/* Busy waits. gen/loops.py puts RT_POLL() into every game loop; every 4096 turns it lands here. A thread that
 * spins on a flag an interrupt sets gets its interrupt: the vblank runs right here when it is due (real time),
 * or after 25 * 4096 turns with --fast (deterministic). A higher-priority thread it readied runs first, as on
 * the EE when an interrupt handler wakes one. */
unsigned int rt_poll_n;
static unsigned int poll_slow_n;
void rt_poll_slow(void)
{
    int n;
    if (cur < 0 || in_tick || rc_oracle_busy) return;     /* no vblank or switch inside an oracle check */
    /* the EE float traps, here too and not only at the vblank: a Windows component called on the game thread
     * (Media Foundation's MFStartup as the opening movie starts) masks them, and game code ran untrapped until the
     * next field (lane I's sweep: "traps found masked at frame 600") */
    ee_float_check_armed();
    if (rt_fast ? ++poll_slow_n < 25 : rt_seconds() < next_vblank) return;
    poll_slow_n = 0;
    vblank_now();
    n = pick();
    if (n >= 0 && n != cur && th[n].prio < th[cur].prio) {
        th[cur].state = T_READY;
        th[cur].seq = ++seq_counter;
        reschedule();
    }
}

/* the current thread gives up the CPU (its state already set) */
static void reschedule(void)
{
    if (rc_oracle_busy) { th[cur].state = T_RUN; rc_oracle_abort(); }   /* an oracle check never blocks: abandon it */
    SwitchToFiber(sched_fiber);
}

void rt_threads_run(void (*main_entry)(void *), void *arg)
{
    int id;
    timeBeginPeriod(1);     /* 1 ms Sleep: the default 15.6 ms timer would oversleep a whole field */
    sched_fiber = ConvertThreadToFiber(NULL);
    {
        /* interrupt handlers run on the scheduler fiber too: give it module state and a module stack */
        uint8_t *lo;
        uint8_t *top = alloc_stack(0x10000, &lo);
        if (top) rc_thread_init((uint32_t)(uintptr_t)top - 16);
    }
    t0 = rt_seconds();
    next_vblank = t0 + 1.0 / 59.94;
    id = rt_thread_create(main_entry, arg, 1, 0x100000);
    rt_thread_start(id, arg);
    scheduler_loop();
}

int rt_thread_create(void (*entry)(void *), void *arg, int priority, unsigned int stack_size)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++) if (th[i].state == T_FREE) break;
    if (i == MAX_THREADS) return -1;
    memset(&th[i], 0, sizeof th[i]);
    th[i].stack_hi = alloc_stack(stack_size ? stack_size : 0x40000, &th[i].stack_lo);
    if (!th[i].stack_hi) { rt_log(0, "thread stack area full"); return -1; }
    {
        uint8_t *lo, *top = alloc_stack(0x10000, &lo);   /* 64 KB: the deepest module frame is 0xA40 bytes */
        if (!top) { rt_log(0, "thread stack area full"); return -1; }
        th[i].rc_stack = (uint32_t)(uintptr_t)top - 16;
    }
    th[i].entry = entry;
    th[i].arg = arg;
    th[i].prio = priority;
    th[i].state = T_DORMANT;
    th[i].fiber = CreateFiber(0x8000, fiber_start, &th[i]);
    return i + 1;
}

int rt_thread_start(int id, void *arg)
{
    thread_t *t = &th[id - 1];
    if (id < 1 || id > MAX_THREADS || t->state != T_DORMANT) return -1;
    t->arg = arg;
    t->state = T_READY;
    t->seq = ++seq_counter;
    /* a higher-priority thread starts at once */
    if (cur >= 0 && t->prio < th[cur].prio) { th[cur].state = T_READY; th[cur].seq = ++seq_counter; reschedule(); }
    return 0;
}

int rt_thread_self(void) { return cur + 1; }

void rt_thread_exit(void)
{
    th[cur].state = T_DORMANT;
    reschedule();
}

void rt_thread_delete(int id)
{
    if (id >= 1 && id <= MAX_THREADS && id - 1 != cur && th[id - 1].state != T_FREE) {
        if (th[id - 1].fiber) DeleteFiber(th[id - 1].fiber);
        th[id - 1].state = T_FREE;
    }
}

int rt_thread_sleep(void)
{
    if (th[cur].wakeups > 0) { th[cur].wakeups--; return cur + 1; }
    th[cur].state = T_SLEEP;
    reschedule();
    return cur + 1;
}

int rt_thread_wakeup(int id)
{
    thread_t *t;
    if (id < 1 || id > MAX_THREADS) return -1;
    t = &th[id - 1];
    if (t->state == T_SLEEP) { t->state = T_READY; t->seq = ++seq_counter; }
    else t->wakeups++;
    return id;
}

int rt_thread_rotate(int priority)
{
    th[cur].state = T_READY;
    th[cur].seq = ++seq_counter;
    (void)priority;
    reschedule();
    return 0;
}

int rt_thread_change_priority(int id, int priority)
{
    int old;
    if (id == 0) id = cur + 1;
    if (id < 1 || id > MAX_THREADS) return -1;
    old = th[id - 1].prio;
    th[id - 1].prio = priority;
    if (id - 1 == cur) { th[cur].state = T_READY; th[cur].seq = ++seq_counter; reschedule(); }
    return old;
}

void rt_thread_yield(void)
{
    th[cur].state = T_READY;
    th[cur].seq = ++seq_counter;
    reschedule();
}

void rt_wait_vblank(void)
{
    th[cur].state = T_WAITVBLANK;
    reschedule();
}

static void threads_log(void)
{
    static const char *const st[] = { "free", "dormant", "ready", "run", "sleep", "wait-sema", "wait-vblank" };
    int i;
    for (i = 0; i < MAX_THREADS; i++)
        if (th[i].state != T_FREE)
            hw_log(2, "thread %d: %s, priority %d, entry %p%s%s", i + 1, st[th[i].state], th[i].prio,
                   (void *)th[i].entry, i == cur ? " (current)" : "", th[i].state == T_WAITSEMA ? " sema" : "");
}

/* ---- semaphores ----------------------------------------------------------------------------------- */

int rt_sema_create(int init, int max)
{
    int i;
    for (i = 0; i < MAX_SEMAS; i++) if (!sem[i].used) {
        sem[i].used = 1; sem[i].count = init; sem[i].max = max;
        return i + 1;
    }
    return -1;
}

int rt_sema_delete(int id) { if (id >= 1 && id <= MAX_SEMAS) sem[id - 1].used = 0; return id; }

int rt_sema_wait(int id)
{
    if (id < 1 || id > MAX_SEMAS || !sem[id - 1].used) return -1;
    while (sem[id - 1].count <= 0) {
        th[cur].state = T_WAITSEMA;
        th[cur].sema = id;
        reschedule();
    }
    sem[id - 1].count--;
    return id;
}

int rt_sema_poll(int id)
{
    if (id < 1 || id > MAX_SEMAS || sem[id - 1].count <= 0) return -1;
    sem[id - 1].count--;
    return id;
}

int rt_sema_signal(int id)
{
    int i, best = -1;
    if (id < 1 || id > MAX_SEMAS || !sem[id - 1].used) return -1;
    sem[id - 1].count++;
    for (i = 0; i < MAX_THREADS; i++)
        if (th[i].state == T_WAITSEMA && th[i].sema == id && (best < 0 || th[i].seq < th[best].seq)) best = i;
    if (best >= 0) { th[best].state = T_READY; th[best].seq = ++seq_counter; }
    return id;
}

/* ---- interrupt handlers --------------------------------------------------------------------------- */

static int add_handler(handler_t *hs, int cause, int (*fn)(int), void *arg)
{
    int i;
    for (i = 0; i < MAX_HANDLERS; i++) if (!hs[i].used) {
        hs[i].used = 1; hs[i].cause = cause; hs[i].fn = fn; hs[i].arg = arg; hs[i].enabled = 1;
        return i + 1;
    }
    return -1;
}

int rt_intc_add(int cause, int (*fn)(int), int next, void *arg) { (void)next; return add_handler(intc, cause, fn, arg); }
int rt_intc_remove(int cause, int id) { (void)cause; if (id >= 1 && id <= MAX_HANDLERS) intc[id - 1].used = 0; return 0; }
int rt_intc_enable(int cause, int on) { if (on) intc_mask |= 1 << cause; else intc_mask &= ~(1 << cause); return 0; }
int rt_dmac_add(int ch, int (*fn)(int), int next, void *arg) { (void)next; return add_handler(dmac, ch, fn, arg); }
int rt_dmac_remove(int ch, int id) { (void)ch; if (id >= 1 && id <= MAX_HANDLERS) dmac[id - 1].used = 0; return 0; }
int rt_dmac_enable(int ch, int on) { if (on) dmac_mask |= 1 << ch; else dmac_mask &= ~(1 << ch); return 0; }

void hw_raise_dmac(int channel) { run_handlers(dmac, channel, dmac_mask); }
void hw_raise_intc(int source) { run_handlers(intc, source, intc_mask); }
