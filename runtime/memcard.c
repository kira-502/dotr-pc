/*
 * Memory cards as folders next to the exe: Saves\card1 (port 0, always inserted) and Saves\card2 (port 1,
 * inserted when that folder exists); "saves=<folder>" in DotR-PC.ini moves them. A PS2 path "/DIR/FILE" is the
 * file Saves\cardN\DIR\FILE. hle/memcard.c turns Sony's libmc calls into these.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"

#define MAX_FD 8
static FILE *fds[MAX_FD];
static char cwd[2][256] = { "/", "/" };
static char root[2][MAX_PATH];
static int seen[2];

/* The cards' folder: Saves next to the exe, or the folder named by "saves=<folder>" in DotR-PC.ini (DOTR_SAVES; a
 * relative path is taken from the exe's folder), e.g. one set of cards shared by several copies of the game */
static const char *card_root(int port)
{
    if (!root[port][0]) {
        char exe[MAX_PATH], base[MAX_PATH], *s;
        const char *saves = getenv("DOTR_SAVES");
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        s = strrchr(exe, '\\');
        if (s) s[1] = 0;
        if (saves && saves[0]) {
            if (saves[0] == '\\' || saves[0] == '/' || (saves[0] && saves[1] == ':')) snprintf(base, MAX_PATH, "%s", saves);
            else snprintf(base, MAX_PATH, "%s%s", exe, saves);
        } else {
            snprintf(base, MAX_PATH, "%sSaves", exe);
        }
        if (port == 0) CreateDirectoryA(base, NULL);
        snprintf(root[port], MAX_PATH, "%s\\card%d", base, port + 1);
        if (port == 0) CreateDirectoryA(root[port], NULL);
        if (port == 0 && GetFileAttributesA(root[port]) == INVALID_FILE_ATTRIBUTES)
            hw_log(0, "memory card 1: %s cannot be created (Windows error %lu): the game sees no memory card and "
                   "cannot save; put the game in a folder you can write to", root[port], GetLastError());
        if (port == 0) hw_log(2, "memory cards: %s", base);
    }
    return root[port];
}

int rt_mc_present(int port)
{
    DWORD a;
    if (port < 0 || port > 1) return 0;
    a = GetFileAttributesA(card_root(port));
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* PS2 name (absolute, or relative to the card's current directory) -> host path; 0 if it leaves the card */
static int host_path(int port, const char *name, char *out, char *ps2_abs)
{
    char abs_[512], parts[32][64];
    int n = 0, i;
    const char *p;
    if (name[0] == '/') snprintf(abs_, sizeof abs_, "%s", name);
    else snprintf(abs_, sizeof abs_, "%s/%s", cwd[port], name);
    for (p = abs_; *p;) {
        int len = 0;
        while (*p == '/') p++;
        if (!*p) break;
        while (p[len] && p[len] != '/') len++;
        if (len == 1 && p[0] == '.') {}
        else if (len == 2 && p[0] == '.' && p[1] == '.') { if (n) n--; }
        else if (n < 32 && len < 64) { memcpy(parts[n], p, (size_t)len); parts[n][len] = 0; n++; }
        else return 0;
        p += len;
    }
    snprintf(out, MAX_PATH, "%s", card_root(port));
    if (ps2_abs) strcpy(ps2_abs, n ? "" : "/");
    for (i = 0; i < n; i++) {
        char *c;
        for (c = parts[i]; *c; c++)
            if (strchr("<>:\"\\|", *c) || (unsigned char)*c < 32) *c = '_';
        strncat(out, "\\", MAX_PATH - strlen(out) - 1);
        strncat(out, parts[i], MAX_PATH - strlen(out) - 1);
        if (ps2_abs) { strcat(ps2_abs, "/"); strcat(ps2_abs, parts[i]); }
    }
    return 1;
}

/* 0 = same card as last time, -1 = a (new) formatted card, < -2 = no card */
int rt_mc_getinfo(int port, int *type, int *free_kb, int *format)
{
    /* no card; the next card in that slot is a "changed card" (-1) once, as when a PS2 card is swapped */
    if (!rt_mc_present(port)) { seen[port] = 0; *type = 0; *free_kb = 0; *format = 0; return -10; }
    *type = 2;              /* PS2 card */
    *free_kb = 7000;        /* of 8 MB */
    *format = 1;
    if (!seen[port]) { seen[port] = 1; return -1; }
    return 0;
}

/* mode: 1 read, 2 write, 3 both, 0x200 create */
int rt_mc_open(int port, const char *name, int mode)
{
    char path[MAX_PATH];
    int fd;
    FILE *f;
    if (!rt_mc_present(port) || !host_path(port, name, path, NULL)) return -4;
    for (fd = 0; fd < MAX_FD && fds[fd]; fd++) {}
    if (fd == MAX_FD) return -7;
    f = fopen(path, "r+b");
    if (!f && (mode & 0x200)) f = fopen(path, "w+b");
    if (!f) return -4;
    fds[fd] = f;
    return fd;
}

int rt_mc_close(int fd)
{
    if (fd < 0 || fd >= MAX_FD || !fds[fd]) return -4;
    fclose(fds[fd]);
    fds[fd] = NULL;
    return 0;
}

int rt_mc_seek(int fd, int offset, int origin)
{
    if (fd < 0 || fd >= MAX_FD || !fds[fd]) return -4;
    fseek(fds[fd], offset, origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET);
    return (int)ftell(fds[fd]);
}

int rt_mc_read(int fd, void *buf, int size)
{
    if (fd < 0 || fd >= MAX_FD || !fds[fd]) return -4;
    fflush(fds[fd]);
    return (int)fread(buf, 1, (size_t)size, fds[fd]);
}

int rt_mc_write(int fd, const void *buf, int size)
{
    int n;
    if (fd < 0 || fd >= MAX_FD || !fds[fd]) return -4;
    n = (int)fwrite(buf, 1, (size_t)size, fds[fd]);
    fflush(fds[fd]);
    return n;
}

int rt_mc_mkdir(int port, const char *name)
{
    char path[MAX_PATH];
    if (!rt_mc_present(port) || !host_path(port, name, path, NULL)) return -4;
    return CreateDirectoryA(path, NULL) ? 0 : -4;
}

int rt_mc_chdir(int port, const char *name, char *old)
{
    char path[MAX_PATH], abs_[512];
    DWORD a;
    if (old) strcpy(old, cwd[port]);
    if (!rt_mc_present(port) || !host_path(port, name, path, abs_)) return -4;
    a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return -4;
    snprintf(cwd[port], sizeof cwd[port], "%s", abs_);
    return 0;
}

int rt_mc_delete(int port, const char *name)
{
    char path[MAX_PATH];
    if (!rt_mc_present(port) || !host_path(port, name, path, NULL)) return -4;
    if (DeleteFileA(path) || RemoveDirectoryA(path)) return 0;
    return -4;
}

/* sceMcTblGetDir: create time, modify time (8 bytes each), size, attributes, reserved, name[32] */
typedef struct {
    unsigned char created[8], modified[8];
    unsigned int size;
    unsigned short attr, res1;
    unsigned int res2, pda;
    char name[32];
} mc_dirent;

static void mc_time(unsigned char *t, const FILETIME *ft)
{
    SYSTEMTIME st;
    FileTimeToSystemTime(ft, &st);
    t[0] = 0; t[1] = (unsigned char)st.wSecond; t[2] = (unsigned char)st.wMinute; t[3] = (unsigned char)st.wHour;
    t[4] = (unsigned char)st.wDay; t[5] = (unsigned char)st.wMonth;
    t[6] = (unsigned char)st.wYear; t[7] = (unsigned char)(st.wYear >> 8);
}

/* name may end in a wildcard ("/DIR/*"); returns the number of entries written */
int rt_mc_getdir(int port, const char *name, int max, void *table)
{
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    mc_dirent *e = (mc_dirent *)table;
    int n = 0;
    if (!rt_mc_present(port) || !host_path(port, name, path, NULL)) return -4;
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (n >= max) break;
        memset(&e[n], 0, sizeof e[n]);
        mc_time(e[n].created, &fd.ftCreationTime);
        mc_time(e[n].modified, &fd.ftLastWriteTime);
        e[n].size = fd.nFileSizeLow;
        e[n].attr = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x8427 : 0x8497;
        snprintf(e[n].name, sizeof e[n].name, "%s", fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
