/* mcimport_tests - the importer on synthetic cards (with and without ECC), never on a real card. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2card.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int file_equals(const char *path, const uint8_t *data, uint32_t size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    int ok;
    if (!f) return 0;
    b = (uint8_t *)malloc(size + 1);
    ok = b && fread(b, 1, size + 1, f) == size && !memcmp(b, data, size);
    free(b);
    fclose(f);
    return ok;
}

static void rm_tree(const char *dir)
{
    char pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p); else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

int main(void)
{
    char tmp[MAX_PATH], card[MAX_PATH], dest[MAX_PATH], path[MAX_PATH], report[4096];
    static uint8_t big[5000], small[100];
    const char *names[3] = { "BASLUS-20515", "icon.sys", "view.ico" };
    const uint8_t *data[3] = { big, small, small };
    uint32_t sizes[3] = { sizeof big, sizeof small, 0 };
    int ecc, i;
    for (i = 0; i < (int)sizeof big; i++) big[i] = (uint8_t)(i * 7 + (i >> 8));
    for (i = 0; i < (int)sizeof small; i++) small[i] = (uint8_t)(255 - i);
    GetTempPathA(sizeof tmp, tmp);
    for (ecc = 0; ecc < 2; ecc++) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        SYSTEMTIME st;
        snprintf(card, sizeof card, "%sdotr_mctest_%d.ps2", tmp, ecc);
        snprintf(dest, sizeof dest, "%sdotr_mctest_saves_%d", tmp, ecc);
        rm_tree(dest);
        CHECK(ps2card_make_test_image(card, ecc, "BASLUS-20515DOTR", 3, names, data, sizes));
        CHECK(ps2card_import(card, dest, 0, report, sizeof report) == 1);
        snprintf(path, sizeof path, "%s\\BASLUS-20515DOTR\\BASLUS-20515", dest);
        CHECK(file_equals(path, big, sizeof big));
        snprintf(path, sizeof path, "%s\\BASLUS-20515DOTR\\icon.sys", dest);
        CHECK(file_equals(path, small, sizeof small));
        snprintf(path, sizeof path, "%s\\BASLUS-20515DOTR\\view.ico", dest);
        CHECK(file_equals(path, small, 0));
        /* card time 2005-01-02 03:04:05 Tokyo = 2005-01-01 18:04:05 UTC */
        snprintf(path, sizeof path, "%s\\BASLUS-20515DOTR\\BASLUS-20515", dest);
        CHECK(GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && FileTimeToSystemTime(&fa.ftLastWriteTime, &st) &&
              st.wYear == 2005 && st.wMonth == 1 && st.wDay == 1 && st.wHour == 18 && st.wMinute == 4 && st.wSecond == 5);
        /* already there: skipped, unless overwrite */
        CHECK(ps2card_import(card, dest, 0, report, sizeof report) == 0 && strstr(report, "skipped"));
        CHECK(ps2card_import(card, dest, 1, report, sizeof report) == 1);
        rm_tree(dest);
        DeleteFileA(card);
    }
    /* not a card */
    snprintf(card, sizeof card, "%sdotr_mctest_bad.ps2", tmp);
    {
        FILE *f = fopen(card, "wb");
        if (f) { fwrite(big, 1, sizeof big, f); fclose(f); }
    }
    CHECK(ps2card_import(card, dest, 0, report, sizeof report) == -1);
    DeleteFileA(card);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
