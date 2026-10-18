/*
 * The EE kernel (syscalls) and Sony's libkernl wrappers, as the game uses them: threads, semaphores, interrupt
 * handlers, cache control, heap. Written from the public PS2 kernel interface (PS2SDK documents the same
 * syscalls); no Sony SDK code.
 */
#include "hle.h"

typedef struct {
    s32 status;
    void *func;
    void *stack;
    s32 stack_size;
    void *gp_reg;
    s32 initial_priority;
    s32 current_priority;
    u32 attr;
    u32 option;
} ee_thread_param;

typedef struct {
    s32 count;
    s32 max_count;
    s32 init_count;
    s32 wait_threads;
    u32 attr;
    u32 option;
} ee_sema_param;

/* ---- threads ------------------------------------------------------------------------------------- */

HLE_FUNC s32 CreateThread(void *arg0)
{
    ee_thread_param *p = (ee_thread_param *)arg0;
    s32 id = rt_thread_create((void (*)(void *))p->func, 0, p->initial_priority, (u32)p->stack_size);
    rt_log(2, "CreateThread func=%p prio=%d stack=%d -> %d", p->func, p->initial_priority, p->stack_size, id);
    return id;
}

HLE_FUNC s32 StartThread(s32 id, void *arg) { return rt_thread_start(id, arg) == 0 ? id : -1; }
HLE_FUNC s32 DeleteThread(s32 id) { rt_thread_delete(id); return id; }
HLE_FUNC s32 TerminateThread(s32 id) { rt_thread_delete(id); return id; }
HLE_FUNC s32 iTerminateThread(s32 id) { rt_thread_delete(id); return id; }
HLE_FUNC s32 ExitThread(void) { rt_thread_exit(); return 0; }
HLE_FUNC void ExitDeleteThread(void) { rt_thread_exit(); }
HLE_FUNC s32 SleepThread(void) { return rt_thread_sleep(); }
HLE_FUNC s32 WakeupThread(s32 id) { return rt_thread_wakeup(id); }
HLE_FUNC s32 iWakeupThread(s32 id) { return rt_thread_wakeup(id); }
HLE_FUNC s32 _iWakeupThread(s32 id) { return rt_thread_wakeup(id); }
HLE_FUNC s32 RotateThreadReadyQueue(s32 prio) { return rt_thread_rotate(prio); }
HLE_FUNC s32 _iRotateThreadReadyQueue(s32 prio) { (void)prio; return 0; }
HLE_FUNC s32 ChangeThreadPriority(s32 id, s32 prio) { return rt_thread_change_priority(id, prio); }
HLE_FUNC s32 iChangeThreadPriority(s32 id, s32 prio) { return rt_thread_change_priority(id, prio); }
HLE_FUNC s32 GetThreadId(void) { return rt_thread_self(); }
HLE_FUNC s32 DisableDispatchThread(void) { return 0; }
HLE_FUNC s32 EnableDispatchThread(void) { return 0; }
HLE_FUNC s32 SuspendThread(s32 id) { (void)id; HLE_TODO("SuspendThread"); return 0; }
HLE_FUNC s32 ResumeThread(s32 id) { (void)id; HLE_TODO("ResumeThread"); return 0; }

/* ---- semaphores ---------------------------------------------------------------------------------- */

HLE_FUNC s32 CreateSema(void *arg0)
{
    ee_sema_param *s = (ee_sema_param *)arg0;
    return rt_sema_create(s->init_count, s->max_count);
}
HLE_FUNC s32 DeleteSema(s32 id) { return rt_sema_delete(id); }
HLE_FUNC s32 SignalSema(s32 id) { return rt_sema_signal(id); }
HLE_FUNC s32 iSignalSema(s32 id) { return rt_sema_signal(id); }
HLE_FUNC s32 WaitSema(s32 id) { return rt_sema_wait(id); }
HLE_FUNC s32 PollSema(s32 id) { return rt_sema_poll(id); }
HLE_FUNC s32 iPollSema(s32 id) { return rt_sema_poll(id); }

/* ---- interrupt handlers -------------------------------------------------------------------------- */

HLE_FUNC s32 AddIntcHandler(s32 cause, s32 (*handler)(s32), s32 next)
{
    rt_log(2, "AddIntcHandler cause=%d", cause);
    return rt_intc_add(cause, (int (*)(int))handler, next, 0);
}
HLE_FUNC s32 AddIntcHandler2(s32 cause, s32 (*handler)(s32), s32 next, void *arg)
{
    return rt_intc_add(cause, (int (*)(int))handler, next, arg);
}
HLE_FUNC s32 RemoveIntcHandler(s32 cause, s32 id) { return rt_intc_remove(cause, id); }
HLE_FUNC s32 AddDmacHandler(s32 ch, s32 (*handler)(s32), s32 next)
{
    rt_log(2, "AddDmacHandler channel=%d", ch);
    return rt_dmac_add(ch, (int (*)(int))handler, next, 0);
}
HLE_FUNC s32 AddDmacHandler2(s32 ch, s32 (*handler)(s32), s32 next, void *arg)
{
    return rt_dmac_add(ch, (int (*)(int))handler, next, arg);
}
HLE_FUNC s32 RemoveDmacHandler(s32 ch, s32 id) { return rt_dmac_remove(ch, id); }
HLE_FUNC s32 _EnableIntc(s32 cause) { return rt_intc_enable(cause, 1); }
HLE_FUNC s32 _DisableIntc(s32 cause) { return rt_intc_enable(cause, 0); }
HLE_FUNC s32 EnableIntc(s32 cause) { return rt_intc_enable(cause, 1); }
HLE_FUNC s32 DisableIntc(s32 cause) { return rt_intc_enable(cause, 0); }
HLE_FUNC s32 iEnableIntc(s32 cause) { return rt_intc_enable(cause, 1); }
HLE_FUNC s32 iDisableIntc(s32 cause) { return rt_intc_enable(cause, 0); }
HLE_FUNC s32 _EnableDmac(s32 ch) { return rt_dmac_enable(ch, 1); }
HLE_FUNC s32 _DisableDmac(s32 ch) { return rt_dmac_enable(ch, 0); }
HLE_FUNC s32 EnableDmac(s32 ch) { return rt_dmac_enable(ch, 1); }
HLE_FUNC s32 DisableDmac(s32 ch) { return rt_dmac_enable(ch, 0); }
HLE_FUNC s32 iEnableDmac(s32 ch) { return rt_dmac_enable(ch, 1); }
HLE_FUNC s32 iDisableDmac(s32 ch) { return rt_dmac_enable(ch, 0); }
HLE_FUNC s32 EnableIntcHandler(s32 cause, s32 id) { (void)cause; (void)id; return 0; }
HLE_FUNC s32 DisableIntcHandler(s32 cause, s32 id) { (void)cause; (void)id; return 0; }
HLE_FUNC s32 EnableDmacHandler(s32 ch, s32 id) { (void)ch; (void)id; return 0; }
HLE_FUNC s32 DisableDmacHandler(s32 ch, s32 id) { (void)ch; (void)id; return 0; }

/* ---- caches, GS IMR, display mode, heap ------------------------------------------------------------ */

HLE_FUNC void FlushCache(s32 mode) { (void)mode; }
HLE_FUNC void iFlushCache(s32 mode) { (void)mode; }
HLE_FUNC void InvalidDCache(u32 start, u32 end) { (void)start; (void)end; }
HLE_FUNC void SyncDCache(u32 start, u32 end) { (void)start; (void)end; }
HLE_FUNC void iInvalidDCache(u32 start, u32 end) { (void)start; (void)end; }
HLE_FUNC void iSyncDCache(u32 start, u32 end) { (void)start; (void)end; }

static u64 hle_gs_imr = 0x7F00;
HLE_FUNC u64 GsGetIMR(void) { return hle_gs_imr; }
HLE_FUNC u64 GsPutIMR(u64 imr) { u64 old = hle_gs_imr; hle_gs_imr = imr; return old; }
HLE_FUNC u64 iGsGetIMR(void) { return hle_gs_imr; }
HLE_FUNC u64 iGsPutIMR(u64 imr) { u64 old = hle_gs_imr; hle_gs_imr = imr; return old; }

HLE_FUNC s32 SetGsCrt(s32 interlace, s32 mode, s32 field)
{
    rt_log(2, "SetGsCrt interlace=%d mode=%d field=%d", interlace, mode, field);
    return 0;
}
HLE_FUNC s32 SetVSyncFlag(void *flag, void *csr) { (void)flag; (void)csr; return 0; }
HLE_FUNC s32 GetMemorySize(void) { return 0x02000000; }
HLE_FUNC s32 CpuConfig(s32 config) { (void)config; return 0; }

/* newlib's malloc grows its heap with sbrk: a region of PS2 RAM between the executable's bss and the game's
 * first fixed buffer (0x00600000, RhBase_Init's packet area) */
#define HEAP_START 0x00360000u
#define HEAP_END   0x00600000u
static u32 hle_heap_top = HEAP_START;
HLE_FUNC u32 EndOfHeap(void) { return HEAP_END; }
HLE_FUNC u32 sbrk(s32 incr)
{
    u32 old = hle_heap_top;
    if (hle_heap_top + incr > HEAP_END || (s32)(hle_heap_top + incr) < (s32)HEAP_START) {
        rt_log(0, "sbrk(%d): heap full (top %08X)", incr, hle_heap_top);
        return (u32)-1;
    }
    hle_heap_top += incr;
    rt_log(2, "sbrk(%d) -> %08X", incr, old);
    return old;
}

HLE_FUNC s32 Exit(s32 code) { rt_fatal("the game called Exit(%d)", code); return 0; }
HLE_FUNC s32 ResetEE(s32 flags) { (void)flags; return 0; }
