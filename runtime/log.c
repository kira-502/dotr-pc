/* The port log (DotR-PC.log next to the exe) and console output. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <xmmintrin.h>
#include "runtime.h"

static FILE *g_log, *g_session;    /* DotR-PC.log (the current session) and Logs\DotR-PC-<time>.log (kept) */
static int g_level = 2;
int rt_gui;             /* the window build (DOTR_GUI from its loader): problems as message boxes, not console text */
int rt_gui_quiet;       /* the player already knows (cancelled the disc dialog): no message box */
static char g_con[2048];    /* the last console text, for the message box */

void report_prune(const char *folder, const char *pattern, int keep, int dirs);   /* runtime/report.c */
void report_record(const char *fmt, ...);

/* DotR-PC.log as before, and the same lines in Logs\DotR-PC-<yyyy-mm-dd_hh-mm-ss>.log next to it (the newest 20
 * kept): a restart no longer erases the last session's evidence */
void log_open(const wchar_t *path)
{
    wchar_t dir[MAX_PATH], *s;
    SYSTEMTIME t;
    g_log = _wfopen(path, L"w");
    wcsncpy(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    s = wcsrchr(dir, L'\\');
    if (s) s[1] = 0; else dir[0] = 0;
    wcsncat(dir, L"Logs", MAX_PATH - wcslen(dir) - 1);
    if (CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        char adir[MAX_PATH];
        wchar_t file[MAX_PATH];
        GetLocalTime(&t);
        snprintf(adir, MAX_PATH, "%ls", dir);
        report_prune(adir, "DotR-PC-*.log", 19, 0);
        swprintf(file, MAX_PATH, L"%ls\\DotR-PC-%04d-%02d-%02d_%02d-%02d-%02d.log", dir, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
        g_session = _wfopen(file, L"w");
    }
}

void log_close(void)
{
    if (g_log) fclose(g_log);
    if (g_session) fclose(g_session);
    g_log = g_session = NULL;
}

/* The C runtime's float formatting (%f) leaves MXCSR all-masked on the calling thread (lane I: 1900 -> 1F80 across
 * one line), which disarmed the EE float traps on the game thread until the next field: both are put back. */
fp_state fp_save(void)
{
    fp_state s;
    s.csr = _mm_getcsr();
    __asm__ volatile("fnstcw %0" : "=m"(s.cw));
    return s;
}
void fp_restore(fp_state s)
{
    _mm_setcsr(s.csr & ~0x3Fu);                  /* the mode as it was, no flags left over from the formatting */
    __asm__ volatile("fldcw %0" : : "m"(s.cw));
}

void hw_log(int level, const char *fmt, ...)
{
    va_list ap;
    fp_state fs;
    char line[1024];
    if (level > g_level) return;
    fs = fp_save();
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    report_record("%c %s", "EWID"[level & 3], line);      /* the flight recorder (runtime/report.c) */
    if (g_log) {
        fprintf(g_log, "[%u] %c %s\n", hw_vblank_count(), "EWID"[level & 3], line);
        fflush(g_log);    /* every line reaches the file: a hang, a kill or a fast-fail loses nothing (no line buffering in the MSVC CRT) */
    }
    if (g_session) {
        fprintf(g_session, "[%u] %c %s\n", hw_vblank_count(), "EWID"[level & 3], line);
        fflush(g_session);
    }
    fp_restore(fs);
}

void con_printf(const char *fmt, ...)
{
    va_list ap;
    fp_state fs = fp_save();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    {
        size_t n = strlen(g_con);
        if (n > sizeof g_con / 2) { memmove(g_con, g_con + n - sizeof g_con / 4, sizeof g_con / 4 + 1); n = strlen(g_con); }
        va_start(ap, fmt);
        vsnprintf(g_con + n, sizeof g_con - n, fmt, ap);
        va_end(ap);
    }
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    if (g_session) {
        va_start(ap, fmt);
        vfprintf(g_session, fmt, ap);
        va_end(ap);
        fflush(g_session);
    }
    fp_restore(fs);
}

/* the window build: the game stopped with a problem, so say so in a message box (the details are in the log) */
void rt_problem_box(void)
{
    char msg[2600];
    if (!rt_gui || rt_gui_quiet) return;
    snprintf(msg, sizeof msg, "The game stopped because of a problem.\n\n%s\nThe details are in DotR-PC.log next to the "
             "game; please send that file along if you ask for help.", g_con[0] ? g_con : "");
    MessageBoxA(NULL, msg, "Yu-Gi-Oh! The Duelists of the Roses", MB_OK | MB_ICONERROR);
}

void rt_exit(unsigned code)
{
    if (code) rt_problem_box();
    ExitProcess(code);
}
