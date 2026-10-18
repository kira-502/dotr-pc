/* iop_irx.c - load IRX modules (relocatable IOP ELF, type 0xFF80) into IOP RAM: relocate, patch import stubs into
 * kernel traps, run the start routine. */
#include "iop.h"
#include "iop_hle.h"


static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

static int lib_id(const char *name)
{
    static const char *libs[] = { "", "sysmem", "loadcore", "intrman", "sifcmd", "stdio", "thbase", "vblank",
                                  "sysclib", "thevent", "sifman", "thsemap", "dmacman" };
    int i;
    for (i = 1; i < (int)(sizeof libs / sizeof libs[0]); i++)
        if (!strncmp(name, libs[i], 8)) return i;
    return 0;
}


int iop_load_irx(const u8 *d, u32 size, const char *args, u32 args_len)
{
    u32 shoff, i, base, image_end = 0, entry = 0, gpv = 0;
    u16 shentsize, shnum, shstrndx;
    const u8 *sh, *strs;
    u8 *mem;
    char modname[64] = "?";
    if (size < 0x34 || memcmp(d, "\177ELF", 4) || rd16(d + 0x10) != 0xFF80) return -1;
    shoff = rd32(d + 0x20);
    shentsize = rd16(d + 0x2E); shnum = rd16(d + 0x30); shstrndx = rd16(d + 0x32);
    sh = d + shoff;
    strs = d + rd32(sh + shstrndx * shentsize + 16);
    /* size of the image (sections are laid out by address), then room for it from the IOP heap */
    for (i = 0; i < shnum; i++) {
        const u8 *s = sh + i * shentsize;
        u32 flags = rd32(s + 8), addr = rd32(s + 12), sz = rd32(s + 20);
        if ((flags & 2) && rd32(s + 4) != 0x70000080 && addr + sz > image_end) image_end = addr + sz;
    }
    base = iop_alloc(image_end);
    if (!base) return -1;
    mem = iop_ptr(base);
    /* copy loadable sections to base + sh_addr, clear bss */
    for (i = 0; i < shnum; i++) {
        const u8 *s = sh + i * shentsize;
        u32 type = rd32(s + 4), flags = rd32(s + 8), addr = rd32(s + 12), off = rd32(s + 16), sz = rd32(s + 20);
        if (type == 0x70000080) {          /* .iopmod */
            entry = rd32(d + off + 4);
            gpv = rd32(d + off + 8);
            snprintf(modname, sizeof modname, "%s", (const char *)d + off + 26);
            continue;
        }
        if (!(flags & 2)) continue;        /* SHF_ALLOC */
        if (type == 8) memset(mem + addr, 0, sz);
        else memcpy(mem + addr, d + off, sz);
    }
    /* relocations: every entry is relative to the module base */
    for (i = 0; i < shnum; i++) {
        const u8 *s = sh + i * shentsize;
        u32 type = rd32(s + 4), off = rd32(s + 16), sz = rd32(s + 20), k;
        u32 hi_pend[64], nhi = 0;
        if (type != 9) continue;
        for (k = 0; k < sz / 8; k++) {
            u32 roff = rd32(d + off + k * 8), info = rd32(d + off + k * 8 + 4), rt = info & 0xFF;
            u8 *p = mem + roff;
            u32 w = rd32(p);
            switch (rt) {
            case 2: w += base; memcpy(p, &w, 4); break;                                       /* R_MIPS_32 */
            case 4: w = (w & 0xFC000000u) | ((((w & 0x3FFFFFFu) << 2) + base) >> 2 & 0x3FFFFFFu); /* R_MIPS_26 */
                    memcpy(p, &w, 4); break;
            case 5: if (nhi < 64) hi_pend[nhi++] = roff; break;                               /* R_MIPS_HI16 */
            case 6: {                                                                          /* R_MIPS_LO16 */
                s32 lo = (s16)(w & 0xFFFF);
                u32 j;
                for (j = 0; j < nhi; j++) {
                    u8 *hp = mem + hi_pend[j];
                    u32 hw = rd32(hp);
                    u32 val = ((hw & 0xFFFF) << 16) + (u32)lo + base;
                    hw = (hw & 0xFFFF0000u) | (((val + 0x8000u) >> 16) & 0xFFFF);
                    memcpy(hp, &hw, 4);
                }
                nhi = 0;
                w = (w & 0xFFFF0000u) | ((u32)(lo + (s32)base) & 0xFFFF);
                memcpy(p, &w, 4);
                break;
            }
            default: iop_log(1, "irx: relocation type %u not handled", rt); break;
            }
        }
    }
    /* import tables: magic, link, version, name[8], then (jr ra, li $0,index) pairs */
    for (i = 0; i + 20 <= image_end; i += 4) {
        if (rd32(mem + i) == 0x41E00000u) {
            char lib[9];
            int id;
            u32 j = i + 20;
            memcpy(lib, mem + i + 12, 8); lib[8] = 0;
            id = lib_id(lib);
            if (!id) iop_log(1, "irx: %s imports unknown library %s", modname, lib);
            while (j + 8 <= image_end && rd32(mem + j) == 0x03E00008u) {
                u32 idx = rd32(mem + j + 4) & 0xFFFF;
                u32 sc = 0x0000000Cu | ((((u32)id << 8) | idx) << 6);
                memcpy(mem + j, &sc, 4);
                j += 8;
            }
            i = j;
        }
    }
    iop_log(2, "irx: %s at %06x (%u KB)", modname, base, (unsigned)(image_end >> 10));
    /* start routine: start(argc, argv) */
    {
        u32 sp = iop_hle_main_stack(), argv = sp - 64, strp = sp - 64 - 256, argc = 1, n;
        const char *name = modname;
        n = (u32)strlen(name) + 1;
        memcpy(iop_ptr(strp), name, n);
        iop_write32(argv, strp);
        if (args && args_len) {
            u32 o = 0;
            strp += n;
            while (o < args_len && argc < 15) {
                u32 l = (u32)strnlen(args + o, args_len - o);
                memcpy(iop_ptr(strp), args + o, l); iop_ptr(strp)[l] = 0;
                iop_write32(argv + argc * 4, strp);
                argc++; strp += l + 1; o += l + 1;
            }
        }
        iop_write32(argv + argc * 4, 0);
        iop_gp = base + gpv;
        {
            u32 r = iop_call(base + entry, argc, argv, 0, 0, sp - 512);
            iop_run_threads();
            return (int)(r & 3);
        }
    }
}
