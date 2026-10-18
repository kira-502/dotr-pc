/* iso9660.c - just enough ISO 9660 to read a file of the owner's disc by path (tools only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iso9660.h"

/* ---- ISO 9660 (just enough to find a file by path) ------------------------------------------------- */

static FILE *iso;

int iso_open(const char *path) { iso = fopen(path, "rb"); return iso != NULL; }

static int read_sectors(unsigned lba, unsigned n, unsigned char *out)
{
    if (_fseeki64(iso, (long long)lba * 2048, SEEK_SET)) return 0;
    return fread(out, 2048, n, iso) == n;
}

unsigned char *iso_read_file(const char *path, unsigned *size_out)
{
    unsigned char pvd[2048], *dir = NULL;
    unsigned lba, size;
    char part[64];
    const char *p = path;
    if (!read_sectors(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) return NULL;
    memcpy(&lba, pvd + 156 + 2, 4); memcpy(&size, pvd + 156 + 10, 4);
    while (*p) {
        unsigned n = 0, off = 0, found = 0, nsec;
        while (*p == '/' || *p == '\\') p++;
        while (*p && *p != '/' && *p != '\\' && n < 63) part[n++] = *p++;
        part[n] = 0;
        nsec = (size + 2047) / 2048;
        free(dir);
        dir = (unsigned char *)malloc((size_t)nsec * 2048);
        if (!read_sectors(lba, nsec, dir)) { free(dir); return NULL; }
        while (off < size) {
            unsigned rl = dir[off], nl, k;
            if (!rl) { off = (off / 2048 + 1) * 2048; continue; }
            nl = dir[off + 32];
            for (k = 0; k < n && k < nl; k++)
                if ((dir[off + 33 + k] & ~0x20) != (part[k] & ~0x20) && dir[off + 33 + k] != part[k]) break;
            if (k == n && (nl == n || dir[off + 33 + n] == ';')) {
                memcpy(&lba, dir + off + 2, 4); memcpy(&size, dir + off + 10, 4);
                found = 1;
                break;
            }
            off += rl;
        }
        if (!found) { free(dir); return NULL; }
    }
    free(dir);
    {
        unsigned char *data = (unsigned char *)malloc(((size_t)size + 2047) & ~(size_t)2047);
        if (!data || !read_sectors(lba, (size + 2047) / 2048, data)) { free(data); return NULL; }
        *size_out = size;
        return data;
    }
}

