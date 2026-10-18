/*
 * rc_testhost.c - runs a lane R test program (rc_unit, rc_diff) as a dotr.dll under the port's own loader.
 * Smart App Control blocks freshly built test .exe files; the loader DotR.exe is allowed, and Windows still checks
 * this DLL when it loads. The test's main() is compiled as rc_host_main (-Dmain=rc_host_main); this file gives it
 * argv from the loader's command line and a 64 MB stack (rc_diff recurses deep).
 *
 * usage (in its own folder, never next to the game's dotr.dll): DotR.exe [test arguments]
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int rc_host_main(int argc, char **argv);

static int g_argc, g_ret;
static char *g_argv[16];

static DWORD WINAPI run(LPVOID p)
{
    (void)p;
    g_ret = rc_host_main(g_argc, g_argv);
    fflush(stdout);
    return 0;
}

__declspec(dllexport) int __cdecl dotr_main(void *map, const wchar_t *cmd)
{
    static char buf[2048];
    char *s = buf, *ctx = NULL, *tok;
    HANDLE h;
    (void)map;
    WideCharToMultiByte(CP_UTF8, 0, cmd, -1, buf, sizeof buf, NULL, NULL);
    if (*s == '"') { s = strchr(s + 1, '"'); s = s ? s + 1 : buf + strlen(buf); }   /* the exe path */
    else { while (*s && *s != ' ') s++; }
    g_argv[g_argc++] = "rc_test";
    for (tok = strtok_s(s, " ", &ctx); tok && g_argc < 15; tok = strtok_s(NULL, " ", &ctx)) g_argv[g_argc++] = tok;
    h = CreateThread(NULL, 64u << 20, run, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return 2;
    WaitForSingleObject(h, INFINITE);
    return g_ret;
}
