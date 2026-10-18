/* Lane P runtime internals (not part of hw.h). */
#ifndef DOTR_RUNTIME_H
#define DOTR_RUNTIME_H

#include <stdint.h>
#include <wchar.h>
#include "hw.h"
#include "../loader/loader_abi.h"

/* iso.c */
int iso_open(const wchar_t *path);
int iso_read(uint64_t offset, uint32_t size, void *dst);
int iso_read_sectors(uint32_t lba, uint32_t count, void *dst);
int iso_find(const char *path, uint32_t *lba, uint32_t *size);

/* elf.c */
typedef struct elf_info {
    uint32_t entry;
    uint32_t image_lo, image_hi;  /* lowest/highest address of the main loadable segment (incl. bss) */
    uint32_t segments, placeholder_segments;
    uint8_t  sha1[20];
} elf_info;
int elf_load_from_iso(const char *name, elf_info *info, char *err, int errlen);

/* sha1.c */
void sha1(const void *data, uint32_t len, uint8_t out[20]);

/* log.c */
void log_open(const wchar_t *path);
void log_close(void);
void con_printf(const char *fmt, ...);
/* around C-runtime formatting on the game thread: its %f leaves MXCSR all-masked (the EE float traps off) */
typedef struct { unsigned csr; unsigned short cw; } fp_state;
fp_state fp_save(void);
void fp_restore(fp_state s);
extern int rt_gui, rt_gui_quiet;
extern int rt_headless;         /* no window (--headless, rc-check): the watchdog stops the run (runtime/crash.c) */
void rt_problem_box(void);                       /* window build: a message box with the last console text */
void rt_exit(unsigned code);                     /* ExitProcess, with that message box when code != 0 */   /* console + log */

/* functab (generated): PS2 function address -> native function */
typedef struct ps2_func { uint32_t addr; void *native; const char *name; } ps2_func;
extern const ps2_func ps2_functab[];
extern const uint32_t ps2_functab_count;
extern const uint32_t ps2_functab_native;
int  tramp_install(char *err, int errlen);

/* threads.c */
hw_u32 rt_vblanks(void);
void rt_threads_run(void (*main_entry)(void *), void *arg);
extern int rt_fast;
extern int rt_fast_forward;
extern unsigned int rt_frame_limit;
void rt_stop_ok(const char *why);
int  rt_pad_script_load(const wchar_t *path);   /* --pad-script: port 0 from a file (runtime/pad.c) */
int  rt_pad_script_load_port(const wchar_t *path, int port);   /* runtime/padscript.c: 0 unreadable, -line bad line */
int  rt_pad_script_buttons(int port, unsigned *b);
int  rt_pad_script_add(unsigned frame, const char *buttons);   /* one --pad-script line */
int  rt_bot_pad(char *buttons, int size);     /* a bot duel drives port 0: 1 + buttons ("cross", "-") (botduel.c) */
int  rt_bot_setup(const wchar_t *cmd);          /* --bot-duel: headless computer-vs-computer duel (runtime/botduel.c) */
void crash_install(void);
int  crash_low_memory_check(void);          /* self-test: PS2 low memory (null pointers) served, not a crash */
void ee_float_init(void);                        /* runtime/eefloat.c */
int  ee_float_exception(struct _EXCEPTION_POINTERS *e);
void ee_float_add_code(uintptr_t lo, uintptr_t hi);  /* more code under the EE float rule */
void ee_float_check_armed(void);                     /* every vblank: traps still armed */
void crash_log_here(const char *why);

#define SLUS_SHA1 "bee9fc13d12ab2818822d905d820320fc6427f23"

#endif
