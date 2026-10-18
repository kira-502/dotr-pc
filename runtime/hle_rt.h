/*
 * Runtime services for the HLE layer (hle/*.c).
 *
 * hle/*.c implements Sony's EE libraries (kernel, SIF, CD/DVD, pad, memory card, GS/DMA glue) with the game's
 * own headers, so it cannot include <windows.h> (names like CreateThread clash). Everything Windows-side lives
 * in runtime/ behind these plain-C calls.
 */
#ifndef DOTR_HLE_RT_H
#define DOTR_HLE_RT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Marks an HLE definition: the port generator then drops the decomp's version and its stub. */
#define HLE_FUNC

void rt_log(int level, const char *fmt, ...);
void rt_fatal(const char *fmt, ...);          /* logs, prints and stops the game */

/* time and frames */
unsigned int rt_vblank_count(void);
void rt_wait_vblank(void);                    /* sleeps to the next 60 Hz field; runs vblank work */
double rt_seconds(void);
void rt_on_vblank(void (*fn)(void));          /* HLE work done once per vblank (e.g. finished-read callbacks) */

/* EE threads (cooperative, PS2 priority rules); entry(arg) runs on a fixed-address stack */
int  rt_thread_create(void (*entry)(void *), void *arg, int priority, unsigned int stack_size);
int  rt_thread_start(int id, void *arg);
int  rt_thread_self(void);
void rt_thread_exit(void);
void rt_thread_delete(int id);
int  rt_thread_sleep(void);                   /* SleepThread */
int  rt_thread_wakeup(int id);                /* WakeupThread / iWakeupThread */
int  rt_thread_rotate(int priority);          /* RotateThreadReadyQueue */
int  rt_thread_change_priority(int id, int priority);
void rt_thread_yield(void);                   /* give other ready threads a turn (busy-wait loops) */

/* semaphores */
int  rt_sema_create(int init, int max);
int  rt_sema_delete(int id);
int  rt_sema_wait(int id);
int  rt_sema_poll(int id);
int  rt_sema_signal(int id);

/* interrupt handlers the game registers (INTC causes: 2 vblank start, 3 vblank end; DMAC: channel) */
int  rt_intc_add(int cause, int (*handler)(int), int next, void *arg);
int  rt_intc_remove(int cause, int id);
int  rt_intc_enable(int cause, int on);
int  rt_dmac_add(int channel, int (*handler)(int), int next, void *arg);
int  rt_dmac_remove(int channel, int id);
int  rt_dmac_enable(int channel, int on);

/* disc */
int  rt_disc_find(const char *path, unsigned int *lba, unsigned int *size);
int  rt_disc_read(unsigned int lba, unsigned int sectors, void *dst);

/* SIF / IOP (runtime/sif.c; sound modules go to lane C via hw/sif.h). Addresses are EE / IOP addresses. */
int  rt_iop_load_module(const char *path, const char *args, unsigned int args_len);   /* module id > 0 */
int  rt_rpc_bound(unsigned int sid);
int  rt_rpc_call(unsigned int sid, unsigned int fno, unsigned int send, unsigned int ssize, unsigned int recv,
                 unsigned int rsize);
void rt_sif_dma(unsigned int iop_addr, unsigned int src, unsigned int size);
unsigned int rt_iop_alloc(unsigned int size);

/* pad (runtime/pad.c): the 20-byte DualShock 2 reply for port 0/1 in pressure mode (scePadRead layout);
 * returns 1 when connected */
int  rt_pad_read(int port, unsigned char *buf20);
int  rt_pad_connected(int port);

/* memory cards (runtime/memcard.c): Saves\card1, Saves\card2 next to the exe; Sony's result codes */
int  rt_mc_present(int port);
int  rt_mc_getinfo(int port, int *type, int *free_kb, int *format);
int  rt_mc_open(int port, const char *name, int mode);
int  rt_mc_close(int fd);
int  rt_mc_seek(int fd, int offset, int origin);
int  rt_mc_read(int fd, void *buf, int size);
int  rt_mc_write(int fd, const void *buf, int size);
int  rt_mc_mkdir(int port, const char *name);
int  rt_mc_chdir(int port, const char *name, char *old);
int  rt_mc_delete(int port, const char *name);
int  rt_mc_getdir(int port, const char *name, int max, void *table);   /* 64-byte sceMcTblGetDir entries */

#ifdef __cplusplus
}
#endif

#endif
