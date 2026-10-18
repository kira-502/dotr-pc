/*
 * iop.h - the small IOP emulator inside sound/ (lane G): an R3000A interpreter with 2 MB of IOP RAM, an IRX
 * loader, high-level kernel calls for the 16 imports KCEJEAST.IRX uses, the IOP DMA channel to SPU2, and the
 * SPU2 itself (spu2.c). Written from public documentation of the hardware; no emulator code is copied.
 */
#ifndef DOTR_IOP_H
#define DOTR_IOP_H

#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define IOP_RAM_SIZE 0x200000u

extern u8 iop_ram[IOP_RAM_SIZE];

/* ---- CPU (iop_cpu.c) ------------------------------------------------------------------------------- */

typedef struct iop_regs {
    u32 r[32];
    u32 hi, lo, pc;
} iop_regs;

extern iop_regs iop;
extern u32 iop_gp;        /* gp of the running module (set by the loader) */
extern int iop_halt;      /* set by a blocking kernel call: the current call ends there */

#define IOP_RET_MAGIC 0x1FFFFF00u   /* return address that ends a host-initiated call */

void iop_cpu_reset(void);
/* Run func(a0..a3) on the stack at sp until it returns. Returns v0. Nested calls (interrupts, RPC) are fine:
 * the caller's registers are saved and restored. */
u32  iop_call(u32 func, u32 a0, u32 a1, u32 a2, u32 a3, u32 sp);
u64  iop_instructions(void);

/* memory, by IOP address (kseg mirrors handled) */
u32  iop_read32(u32 a);
u16  iop_read16(u32 a);
u8   iop_read8(u32 a);
void iop_write32(u32 a, u32 v);
void iop_write16(u32 a, u16 v);
void iop_write8(u32 a, u8 v);
static inline u8 *iop_ptr(u32 a) { return iop_ram + (a & (IOP_RAM_SIZE - 1)); }

/* ---- hardware registers (iop_hw.c) ----------------------------------------------------------------- */

u32  iop_hw_read(u32 a, int size);
void iop_hw_write(u32 a, u32 v, int size);
void iop_hw_reset(void);
/* SPU2 pulls AutoDMA input: copy n bytes of the running channel 4 (core 0) / 7 (core 1) transfer into dst;
 * returns bytes copied (0 when the channel is idle). */
u32  iop_dma_pull(int core, u8 *dst, u32 n);
void iop_hw_tick(void);        /* deliver DMA completions that are due */

/* ---- kernel (iop_hle.c) ---------------------------------------------------------------------------- */

void iop_hle_reset(void);
void iop_hle_call(u32 code);   /* syscall trap from a patched import stub */
void iop_raise_intr(int intr); /* request an interrupt (9 SPU2, 36 DMA4, 40 DMA7) */
void iop_check_intr(void);     /* run pending interrupt handlers if enabled */
void iop_vblank(void);         /* run the registered vblank handlers */
/* RPC: call the server registered for sid with fno and the request already copied to its buffer. */
int  iop_rpc_find(u32 sid, u32 *buf);
u32  iop_rpc_call(u32 sid, u32 fno, u32 size);
u32  iop_alloc(u32 size);
int  iop_free(u32 addr);      /* 0 ok, -1 not an allocated block */
void iop_run_threads(void);

/* ---- IRX loader (iop_irx.c) ------------------------------------------------------------------------ */

/* Load an IRX image at the next free IOP address, resolve its imports to HLE traps and run its start
 * routine. Returns the start routine's result (0 = resident), or -1 when the file is not an IRX. */
int  iop_load_irx(const u8 *data, u32 size, const char *args, u32 args_len);

/* ---- SPU2 (spu2.c) --------------------------------------------------------------------------------- */

void spu2_reset(void);
u16  spu2_read(u32 a);
void spu2_write(u32 a, u16 v);
/* Mix n stereo frames at 48 kHz into out (interleaved L/R). */
void spu2_render(s16 *out, int n);
void spu2_dma_write_core(int core, const u8 *src, u32 bytes);  /* normal DMA into SPU RAM at the core's TSA */
void spu2_dma_read_core(int core, u8 *dst, u32 bytes);         /* and out of it */
int  spu2_adma_active(int core);
void spu2_dma_done(int core);
extern u16 spu2_mem[0x100000];                  /* 2 MB sound RAM, 16-bit words */

void iop_log(int level, const char *fmt, ...);

#endif
