/*
 * C-library functions that newlib implements in hand-written R5900 assembly (kept as inline asm in the
 * decomp), and libgcc's 64-bit multiply. Standard C semantics; plain loops on purpose (this file is compiled
 * with -fno-builtin and its names are renamed ps2_* in the objects, so they never call the PC C runtime).
 */
#include "hle.h"

HLE_FUNC void *memmove(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else if (d > s) { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

HLE_FUNC void *memcpy(void *dst, const void *src, s32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n-- > 0) *d++ = *s++;
    return dst;
}

HLE_FUNC void *memset(void *dst, s32 c, u32 n)
{
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

HLE_FUNC void *memchr(const void *s, s32 c, u32 n)
{
    const u8 *p = (const u8 *)s;
    for (; n--; p++) if (*p == (u8)c) return (void *)p;
    return 0;
}

HLE_FUNC s32 memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (; n--; x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

HLE_FUNC s32 strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (s32)(p - s);
}

HLE_FUNC char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) {}
    return dst;
}

HLE_FUNC void *strncpy(void *dst, const void *src, u32 n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return dst;
}

HLE_FUNC char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != 0) {}
    return dst;
}

HLE_FUNC char *strchr(const char *s, s32 c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

HLE_FUNC s32 strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

HLE_FUNC s32 strncmp(const char *a, const char *b, u32 n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}

/* libgcc __muldi3 */
HLE_FUNC s64 func_001A05E8(s64 u, s64 v) { return (s64)((u64)u * (u64)v); }

/* ---- Sony libdev (VU0 device control) and libgraph path control: nothing to do on the PC ---------- */
HLE_FUNC void sceDevVu0Reset(void) {}
HLE_FUNC s32 sceDevVu0CheckBusy(void) { return 0; }
HLE_FUNC s32 sceDevVu0Pause(void) { return 0; }
HLE_FUNC s32 sceDevVu0Continue(void) { return 0; }
HLE_FUNC s32 sceDevVu0Sync(void) { return 0; }
HLE_FUNC void sceDevVu0Exec(s32 addr) { (void)addr; HLE_TODO("sceDevVu0Exec (VU0 micro mode)"); }
HLE_FUNC void sceDevVu0PutDBit(s32 on) { (void)on; }
HLE_FUNC void sceDevVu0PutTBit(s32 on) { (void)on; }
HLE_FUNC s32 sceDevVu0GetDBit(void) { return 0; }
HLE_FUNC s32 sceDevVu0GetTBit(void) { return 0; }
HLE_FUNC u32 sceDevVu0GetTpc(void) { return 0; }
HLE_FUNC s32 sceDevVu0GetCnd(void *cnd) { (void)cnd; return 0; }
HLE_FUNC s32 sceDevVu0PutCnd(void *cnd) { (void)cnd; return 0; }
HLE_FUNC void sceGsResetPath(void) {}
HLE_FUNC s32 sceGsSyncPath(s32 mode, s32 timeout) { (void)mode; (void)timeout; return 0; }

/* newlib's system calls for its stdio (the game only prints debug text through it; no files are opened by name).
 * fstat and isatty answer as the game's own newlib does (main_022.c: a character device, st_mode 0x2000; a tty);
 * write takes the bytes (the PS2 sends them to its debug console); read is end of file; lseek fails as there. */
HLE_FUNC s32 fstat(s32 arg0, void *arg1)
{
    (void)arg0;
    *(s64 *)((u8 *)arg1 + 0x48) = 0;
    *(s32 *)((u8 *)arg1 + 4) = 0x2000;
    return 0;
}
HLE_FUNC s32 write(s32 mode, s32 a, s32 b) { (void)mode; (void)a; return b; }
HLE_FUNC s32 read(s32 fd, char *buf, s32 n) { (void)fd; (void)buf; (void)n; return 0; }
HLE_FUNC s32 lseek(void) { return -1; }
HLE_FUNC s32 close(void) { return 0; }
HLE_FUNC s32 isatty(void) { return 1; }
