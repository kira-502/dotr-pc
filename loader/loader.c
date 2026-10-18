/*
 * DotR.exe - the tiny loader (PORT-PLAN.md 4.1).
 *
 * Linked at /BASE:0x10000 /FIXED with no C runtime. Windows (WOW64) already places heaps, stacks and mapped
 * files in low memory before any of our code runs, so reserving at run time is too late (measured: 0x20000..
 * 0x750000 busy at entry). Instead the image itself claims the range: a zero-filled array in .bss, which the
 * kernel maps together with the image when it creates the process (the technique of Cxbx-Reloaded's
 * cxbxr-ldr). Our few hundred bytes of code and data sit below 0x100000, the array covers
 *
 *   0x00100000..0x01FFFFFF  EE main RAM
 *   0x02000000..0x023FFFFF  the runtime's fixed-address stacks
 *
 * and the scratchpad (0x70000000, 16 KB) is reserved at entry. The uncached mirrors (0x2xxxxxxx/0x3xxxxxxx)
 * are not mapped: image pages cannot be aliased; the runtime strips mirror bits where the game uses them.
 * Then dotr.dll (game + runtime) is loaded and takes over.
 */
#include <windows.h>
#include "loader_abi.h"

/* Must be the only large object: it pushes everything after it out of the claimed range. */
unsigned char ps2_address_space_placeholder[DOTR_STACKS_BASE + DOTR_STACKS_SIZE];

static HANDLE g_out;

#ifdef DOTR_GUI
/* the window build (the release's "Yu-Gi-Oh! The Duelists of the Roses.exe"): no console, problems in a message box */
static char g_msg[512];
static int g_msg_len;

static void out(const char *s)
{
    while (*s && g_msg_len < (int)sizeof g_msg - 1) g_msg[g_msg_len++] = *s++;
}

static void show(void)
{
    if (g_msg_len) MessageBoxA(NULL, g_msg, "Yu-Gi-Oh! The Duelists of the Roses", MB_OK | MB_ICONERROR);
}
#else
static void out(const char *s)
{
    DWORD n = 0, len = 0;
    while (s[len]) len++;
    if (g_out && g_out != INVALID_HANDLE_VALUE) WriteFile(g_out, s, len, &n, NULL);
}

static void show(void) {}
#endif

static void out_hex(const char *label, DWORD v)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[16];
    int i;
    out(label);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) buf[2 + i] = hx[(v >> (28 - 4 * i)) & 15];
    buf[10] = '\r'; buf[11] = '\n'; buf[12] = 0;
    out(buf);
}

static int map_memory(dotr_memory_map *m)
{
    DWORD lo = (DWORD)ps2_address_space_placeholder;
    DWORD hi = lo + sizeof(ps2_address_space_placeholder);
    MEMORY_BASIC_INFORMATION mbi;
    if (lo > DOTR_RAM_MAP_BASE || hi < DOTR_STACKS_BASE + DOTR_STACKS_SIZE) {
        m->error_step = 1; m->error_code = lo; return 0;
    }
    if (!VirtualQuery((void *)DOTR_RAM_MAP_BASE, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE) {
        m->error_step = 2; m->error_code = GetLastError(); return 0;
    }
    m->ram = (void *)DOTR_RAM_MAP_BASE;
    m->stacks = (void *)DOTR_STACKS_BASE;
    m->spr = VirtualAlloc((void *)DOTR_SPR_BASE, DOTR_SPR_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (m->spr != (void *)DOTR_SPR_BASE) { m->error_code = GetLastError(); m->error_step = 5; return 0; }
    /* register pages (EE registers, VU memory, GS privileged registers): plain memory holding register values
     * for the runtime's virtual hardware */
    if (VirtualAlloc((void *)0x10000000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)0x10000000 ||
        VirtualAlloc((void *)0x11000000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)0x11000000 ||
        VirtualAlloc((void *)0x12000000, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)0x12000000) {
        m->error_code = GetLastError(); m->error_step = 6; return 0;
    }
    return 1;
}

DWORD CALLBACK rawMain(void)
{
    static dotr_memory_map m;
    HMODULE dll;
    dotr_main_fn entry;
    DWORD rc;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
#ifdef DOTR_GUI
    SetEnvironmentVariableW(L"DOTR_GUI", L"1");     /* dotr.dll: message boxes instead of console text */
#endif
    m.abi_version = DOTR_LOADER_ABI;
    m.ok = map_memory(&m);
    if (!m.ok) {
        out("DotR-PC: could not reserve the PS2 memory layout on this PC.\r\n");
        out_hex("  step: ", m.error_step);
        out_hex("  code: ", m.error_code);
        /* still load the runtime so the self-test can report in full */
    }
    dll = LoadLibraryW(L"dotr.dll");
    if (!dll) {
        out("DotR-PC: dotr.dll is missing or could not be loaded.\r\n");
        out_hex("  Windows error: ", GetLastError());
        show();
        ExitProcess(2);
    }
    entry = (dotr_main_fn)GetProcAddress(dll, "dotr_main");
    if (!entry) {
        out("DotR-PC: dotr.dll has no dotr_main.\r\n");
        show();
        ExitProcess(3);
    }
    rc = (DWORD)entry(&m, GetCommandLineW());
    ExitProcess(rc);
    return rc;
}
