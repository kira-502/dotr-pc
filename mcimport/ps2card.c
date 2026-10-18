/* ps2card.c - read a PS2 memory card image (PCSX2 .ps2) and copy its saves into the port's save folders.
 *
 * Card layout (the standard PS2 card file system): page 0 holds the superblock; clusters are pages_per_cluster
 * pages; the FAT is reached through ifc_list (indirect FAT clusters -> FAT clusters -> one u32 per cluster, bit 31
 * = in use, low 31 bits = next cluster, 0x7FFFFFFF = end); directories are arrays of 512-byte entries. Cluster
 * numbers in the FAT and in directory entries count from alloc_offset. Dates on the card are Tokyo time. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2card.h"

#define MAGIC "Sony PS2 Memory Card Format "

typedef struct {
    const uint8_t *img;
    size_t size;
    uint32_t page_len, ppc, stride, cluster_len, clusters, alloc_offset, root;
    uint32_t ifc[32];
    uint8_t *clbuf;
} card;

typedef struct {
    uint16_t mode;
    uint32_t length, cluster;
    uint8_t modified[8];
    char name[33];
} dirent;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* absolute cluster -> its bytes (pages joined, ECC skipped); NULL past the end of the image */
static const uint8_t *cluster_at(card *c, uint32_t abs)
{
    uint32_t i;
    for (i = 0; i < c->ppc; i++) {
        size_t off = ((size_t)abs * c->ppc + i) * c->stride;
        if (off + c->page_len > c->size) return NULL;
        memcpy(c->clbuf + i * c->page_len, c->img + off, c->page_len);
    }
    return c->clbuf;
}

/* FAT entry of a relative cluster */
static uint32_t fat_next(card *c, uint32_t rel)
{
    uint32_t per = c->cluster_len / 4, fi = rel / per, ii = fi / per;
    const uint8_t *b;
    uint32_t fat_cl;
    if (ii >= 32 || !(b = cluster_at(c, c->ifc[ii]))) return 0xFFFFFFFFu;
    fat_cl = rd32(b + (fi % per) * 4);
    if (!(b = cluster_at(c, fat_cl))) return 0xFFFFFFFFu;
    return rd32(b + (rel % per) * 4);
}

/* read len bytes of the chain starting at relative cluster rel; returns a malloc'd buffer or NULL */
static uint8_t *read_chain(card *c, uint32_t rel, uint32_t len)
{
    uint8_t *out = (uint8_t *)malloc(len ? len : 1);
    uint32_t done = 0, guard = 0;
    if (!out) return NULL;
    while (done < len) {
        const uint8_t *b;
        uint32_t n = len - done < c->cluster_len ? len - done : c->cluster_len, e;
        if (rel >= c->clusters || ++guard > c->clusters || !(b = cluster_at(c, rel + c->alloc_offset))) { free(out); return NULL; }
        memcpy(out + done, b, n);
        done += n;
        if (done >= len) break;
        e = fat_next(c, rel);
        if (!(e & 0x80000000u) || (e & 0x7FFFFFFFu) == 0x7FFFFFFFu) { free(out); return NULL; }
        rel = e & 0x7FFFFFFFu;
    }
    return out;
}

static void parse_dirent(const uint8_t *p, dirent *d)
{
    d->mode = rd16(p);
    d->length = rd32(p + 4);
    d->cluster = rd32(p + 0x10);
    memcpy(d->modified, p + 0x18, 8);
    memcpy(d->name, p + 0x40, 32);
    d->name[32] = 0;
}

/* a directory's entries (count = its own length field); returns a malloc'd array of count*512 bytes */
static uint8_t *read_dir(card *c, uint32_t rel, uint32_t count)
{
    if (count == 0 || count > 4096) return NULL;
    return read_chain(c, rel, count * 512);
}

static int open_card(card *c, const uint8_t *img, size_t size)
{
    uint32_t i, npages;
    memset(c, 0, sizeof *c);
    if (size < 0x154 || memcmp(img, MAGIC, strlen(MAGIC))) return 0;
    c->img = img; c->size = size;
    c->page_len = rd16(img + 0x28);
    c->ppc = rd16(img + 0x2A);
    c->clusters = rd32(img + 0x30);
    c->alloc_offset = rd32(img + 0x34);
    c->root = rd32(img + 0x3C);
    for (i = 0; i < 32; i++) c->ifc[i] = rd32(img + 0x50 + i * 4);
    if (c->page_len != 512 && c->page_len != 1024) return 0;
    if (c->ppc == 0 || c->ppc > 16 || c->clusters == 0) return 0;
    c->cluster_len = c->page_len * c->ppc;
    npages = c->clusters * c->ppc;
    /* raw dump with the 16-byte spare area (ECC) after each 512-byte page, or plain pages */
    c->stride = size >= (size_t)npages * (c->page_len + c->page_len / 32) ? c->page_len + c->page_len / 32 : c->page_len;
    c->clbuf = (uint8_t *)malloc(c->cluster_len);
    return c->clbuf != NULL;
}

/* the same character rules as runtime/memcard.c */
static void host_name(const char *in, char *out, size_t n)
{
    size_t i;
    for (i = 0; in[i] && i + 1 < n; i++)
        out[i] = (strchr("<>:\"\\|/", in[i]) || (unsigned char)in[i] < 32) ? '_' : in[i];
    out[i] = 0;
}

/* card time (Tokyo, UTC+9) -> file time */
static void set_mtime(const char *path, const uint8_t *t)
{
    SYSTEMTIME st;
    FILETIME ft;
    ULARGE_INTEGER u;
    HANDLE h;
    memset(&st, 0, sizeof st);
    st.wSecond = t[1]; st.wMinute = t[2]; st.wHour = t[3]; st.wDay = t[4]; st.wMonth = t[5];
    st.wYear = (WORD)(t[6] | (t[7] << 8));
    if (!SystemTimeToFileTime(&st, &ft)) return;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 9ull * 3600 * 10000000;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

/* copy one directory (and what is under it) into host folder dest; returns 0 on a read error */
static int copy_dir(card *c, const dirent *dir, const char *dest, int depth)
{
    uint8_t *ents = read_dir(c, dir->cluster, dir->length);
    uint32_t i;
    int ok = 1;
    if (!ents) return 0;
    CreateDirectoryA(dest, NULL);
    for (i = 0; i < dir->length && ok; i++) {
        dirent e;
        char nm[64], path[MAX_PATH];
        parse_dirent(ents + i * 512, &e);
        if (!(e.mode & 0x8000) || !strcmp(e.name, ".") || !strcmp(e.name, "..") || !e.name[0]) continue;
        host_name(e.name, nm, sizeof nm);
        snprintf(path, sizeof path, "%s\\%s", dest, nm);
        if (e.mode & 0x20) {
            if (depth < 8) ok = copy_dir(c, &e, path, depth + 1);
        } else {
            uint8_t *data = read_chain(c, e.cluster, e.length);
            FILE *f;
            if (!data) { ok = 0; break; }
            f = fopen(path, "wb");
            if (!f) { free(data); ok = 0; break; }
            ok = fwrite(data, 1, e.length, f) == e.length;
            fclose(f);
            free(data);
            set_mtime(path, e.modified);
        }
    }
    free(ents);
    if (ok) set_mtime(dest, dir->modified);
    return ok;
}

static int load_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || !(*data = (uint8_t *)malloc((size_t)n))) { fclose(f); return 0; }
    *size = fread(*data, 1, (size_t)n, f);
    fclose(f);
    return *size == (size_t)n;
}

static void say(char *report, int report_size, const char *fmt, const char *a, const char *b)
{
    size_t l;
    if (!report || report_size <= 0) return;
    l = strlen(report);
    if (l + 1 < (size_t)report_size) snprintf(report + l, (size_t)report_size - l, fmt, a, b);
}

int ps2card_import(const char *image, const char *dest, int overwrite, char *report, int report_size)
{
    uint8_t *img = NULL, *root;
    size_t size = 0;
    card c;
    dirent rd;
    uint32_t i, count;
    int copied = 0;
    if (report && report_size > 0) report[0] = 0;
    if (!load_file(image, &img, &size)) { say(report, report_size, "cannot read %s%s\n", image, ""); return -1; }
    if (!open_card(&c, img, size)) { free(img); say(report, report_size, "%s is not a PS2 memory card%s\n", image, ""); return -1; }
    /* the root's "." entry holds the number of root entries */
    root = read_chain(&c, c.root, 512);
    if (!root) { free(c.clbuf); free(img); return -1; }
    parse_dirent(root, &rd);
    count = rd.length;
    free(root);
    rd.cluster = c.root;
    root = read_dir(&c, c.root, count);
    if (!root) { free(c.clbuf); free(img); return -1; }
    CreateDirectoryA(dest, NULL);
    for (i = 0; i < count; i++) {
        dirent e;
        char nm[64], path[MAX_PATH];
        DWORD a;
        parse_dirent(root + i * 512, &e);
        if (!(e.mode & 0x8000) || !(e.mode & 0x20) || !strcmp(e.name, ".") || !strcmp(e.name, "..") || !e.name[0]) continue;
        host_name(e.name, nm, sizeof nm);
        snprintf(path, sizeof path, "%s\\%s", dest, nm);
        a = GetFileAttributesA(path);
        if (a != INVALID_FILE_ATTRIBUTES && !overwrite) { say(report, report_size, "skipped %s (already there)%s\n", e.name, ""); continue; }
        if (copy_dir(&c, &e, path, 0)) { say(report, report_size, "copied %s -> %s\n", e.name, path); copied++; }
        else say(report, report_size, "FAILED %s (damaged on the card)%s\n", e.name, "");
    }
    free(root);
    free(c.clbuf);
    free(img);
    return copied;
}

/* ---- test images ----------------------------------------------------------------------------------- */

int ps2card_make_test_image(const char *path, int ecc, const char *dir_name, int nfiles, const char *const *names,
                            const uint8_t *const *data, const uint32_t *sizes)
{
    enum { PAGE = 512, PPC = 2, CL = 1024, CLUSTERS = 8192, IFC = 8, FAT0 = 9, ALLOC = 41 };
    uint32_t stride = PAGE + (ecc ? 16 : 0), next = 0, i;
    size_t size = (size_t)CLUSTERS * PPC * stride;
    uint8_t *img = (uint8_t *)calloc(size, 1), *cl, *sb;
    uint32_t *fat;
    FILE *f;
    int ok;
    uint32_t root_cl, dir_cl, file_cl[64];
    if (!img || nfiles > 62) { free(img); return 0; }
    cl = (uint8_t *)calloc((size_t)CLUSTERS, CL);   /* logical clusters, spread into pages at the end */
    fat = (uint32_t *)calloc((size_t)(CLUSTERS - ALLOC), 4);
    if (!cl || !fat) { free(img); free(cl); free(fat); return 0; }
#define ALLOC_CHAIN(n, first) do { uint32_t k_; (first) = next; for (k_ = 0; k_ < (n); k_++) { fat[next] = 0x80000000u | (k_ + 1 < (n) ? next + 1 : 0x7FFFFFFFu); next++; } } while (0)
#define CLP(rel) (cl + ((size_t)(rel) + ALLOC) * CL)
    /* superblock */
    sb = cl;
    memcpy(sb, MAGIC, strlen(MAGIC));
    memcpy(sb + 0x1C, "1.2.0.0", 7);
    sb[0x28] = PAGE & 0xFF; sb[0x29] = PAGE >> 8; sb[0x2A] = PPC; sb[0x2C] = 16;
    { uint32_t v[4] = { CLUSTERS, ALLOC, CLUSTERS - 16, 0 }; memcpy(sb + 0x30, v, 16); }
    { uint32_t v = IFC; memcpy(sb + 0x50, &v, 4); }
    sb[0x150] = 2; sb[0x151] = 0x52;
    /* root: ".", "..", the save directory */
    ALLOC_CHAIN(2, root_cl);
    ALLOC_CHAIN((uint32_t)((nfiles + 2) * 512 + CL - 1) / CL, dir_cl);
    for (i = 0; i < (uint32_t)nfiles; i++) {
        uint32_t n = (sizes[i] + CL - 1) / CL;
        ALLOC_CHAIN(n ? n : 1, file_cl[i]);
    }
    {
        uint8_t ents[3 * 512];
        uint32_t len3 = 3, dn = (uint32_t)nfiles + 2;
        uint8_t when[8] = { 0, 30, 15, 12, 24, 12, 0xD4, 0x07 };   /* 2004-12-24 12:15:30 */
        memset(ents, 0, sizeof ents);
        ents[0] = 0x27; ents[1] = 0x84; memcpy(ents + 4, &len3, 4); memcpy(ents + 0x10, &root_cl, 4); ents[0x40] = '.';
        ents[512] = 0x27; ents[513] = 0xA4; ents[512 + 0x40] = '.'; ents[512 + 0x41] = '.';
        ents[1024] = 0x27; ents[1025] = 0x84; memcpy(ents + 1024 + 4, &dn, 4); memcpy(ents + 1024 + 0x10, &dir_cl, 4);
        memcpy(ents + 1024 + 0x18, when, 8);
        snprintf((char *)ents + 1024 + 0x40, 32, "%s", dir_name);
        memcpy(CLP(root_cl), ents, sizeof ents);   /* root chain is contiguous */
    }
    {
        uint8_t *d = CLP(dir_cl);   /* contiguous */
        uint8_t when[8] = { 0, 5, 4, 3, 2, 1, 0xD5, 0x07 };       /* 2005-01-02 03:04:05 */
        d[0] = 0x27; d[1] = 0x84; d[0x40] = '.';
        d[512] = 0x27; d[513] = 0xA4; d[512 + 0x40] = '.'; d[512 + 0x41] = '.';
        for (i = 0; i < (uint32_t)nfiles; i++) {
            uint8_t *e = d + (i + 2) * 512;
            e[0] = 0x17; e[1] = 0x84;
            memcpy(e + 4, &sizes[i], 4);
            memcpy(e + 0x10, &file_cl[i], 4);
            memcpy(e + 0x18, when, 8);
            snprintf((char *)e + 0x40, 32, "%s", names[i]);
            memcpy(CLP(file_cl[i]), data[i], sizes[i]);   /* contiguous */
        }
    }
    /* FAT: one indirect cluster listing FAT clusters FAT0.. */
    {
        uint32_t per = CL / 4, nfat = (CLUSTERS - ALLOC + per - 1) / per, k;
        uint32_t *ind = (uint32_t *)(cl + (size_t)IFC * CL);
        for (k = 0; k < nfat; k++) {
            ind[k] = FAT0 + k;
            memcpy(cl + (size_t)(FAT0 + k) * CL, fat + k * per,
                   (k + 1) * per <= (uint32_t)(CLUSTERS - ALLOC) ? CL : ((CLUSTERS - ALLOC) - k * per) * 4);
        }
        /* unused entries stay 0 (free) */
    }
    for (i = 0; i < (uint32_t)CLUSTERS * PPC; i++) memcpy(img + (size_t)i * stride, cl + (size_t)i * PAGE, PAGE);
    f = fopen(path, "wb");
    ok = f && fwrite(img, 1, size, f) == size;
    if (f) fclose(f);
    free(img); free(cl); free(fat);
    return ok;
#undef ALLOC_CHAIN
#undef CLP
}
