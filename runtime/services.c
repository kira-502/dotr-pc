/* Runtime services the HLE layer calls (hle_rt.h): logging, stopping, disc. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"
#include "sif.h"

void rt_log(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    fp_state fs = fp_save();
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hw_log(level, "%s", buf);
    fp_restore(fs);
}

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_printf("DotR-PC stopped: %s (frame %u)\n", buf, rt_vblanks());
    hw_log(0, "STOP: %s", buf);
    snd_shutdown();
    log_close();
    rt_exit(1);
}

/* the clean end of a limited run (--frames N) */
void rt_stop_ok(const char *why)
{
    con_printf("DotR-PC: %s after %u frames\n", why, rt_vblanks());
    hw_log(2, "end: %s", why);
    snd_shutdown();
    log_close();
    ExitProcess(0);
}

int rt_disc_find(const char *path, unsigned int *lba, unsigned int *size)
{
    return iso_find(path, lba, size);
}

int rt_disc_read(unsigned int lba, unsigned int sectors, void *dst)
{
    return iso_read_sectors(lba, sectors, dst);
}

void *hw_disc_read_file(const char *path, hw_u32 *size)
{
    const char *p = strchr(path, ':');
    unsigned int lba, n;
    void *buf;
    if (!rt_disc_find(p ? p + 1 : path, &lba, &n)) return NULL;
    buf = malloc(((size_t)n + 2047) & ~(size_t)2047);
    if (!buf) return NULL;
    if (!rt_disc_read(lba, (n + 2047) / 2048, buf)) { free(buf); return NULL; }
    if (size) *size = n;
    return buf;
}
