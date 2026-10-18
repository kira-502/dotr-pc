/* Load the game's executable from the owner's disc into PS2 RAM, as the PS2 loader does. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "runtime.h"

#pragma pack(push, 1)
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Elf32_Ehdr;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } Elf32_Phdr;
#pragma pack(pop)

int elf_load_from_iso(const char *name, elf_info *info, char *err, int errlen)
{
    uint32_t lba, size, i;
    uint8_t *buf;
    Elf32_Ehdr *eh;
    memset(info, 0, sizeof(*info));
    if (!iso_find(name, &lba, &size)) { snprintf(err, errlen, "%s not found on the disc", name); return 0; }
    buf = (uint8_t *)malloc(size + 2048);
    if (!buf || !iso_read((uint64_t)lba * 2048u, size, buf)) { snprintf(err, errlen, "could not read %s", name); free(buf); return 0; }
    sha1(buf, size, info->sha1);
    eh = (Elf32_Ehdr *)buf;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->machine != 8) { snprintf(err, errlen, "%s is not a MIPS ELF", name); free(buf); return 0; }
    info->entry = eh->entry;
    info->image_lo = 0xFFFFFFFFu;
    for (i = 0; i < eh->phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(buf + eh->phoff + i * eh->phentsize);
        if (ph->type != 1 || ph->memsz == 0) continue;
        if (ph->vaddr < DOTR_RAM_MAP_BASE || ph->vaddr + ph->memsz > DOTR_RAM_MAP_END) {
            snprintf(err, errlen, "segment %u at %08X lies outside mapped RAM", i, ph->vaddr); free(buf); return 0;
        }
        if (ph->filesz == 0) {
            /* the 834 empty overlay placeholders at 0x01E00000: nothing to load */
            info->placeholder_segments++;
            continue;
        }
        if (ph->offset + ph->filesz > size) { snprintf(err, errlen, "segment %u truncated", i); free(buf); return 0; }
        memcpy(PS2_PTR(ph->vaddr), buf + ph->offset, ph->filesz);
        memset((uint8_t *)PS2_PTR(ph->vaddr) + ph->filesz, 0, ph->memsz - ph->filesz);
        info->segments++;
        if (ph->vaddr < info->image_lo) info->image_lo = ph->vaddr;
        if (ph->vaddr + ph->memsz > info->image_hi) info->image_hi = ph->vaddr + ph->memsz;
    }
    free(buf);
    return info->segments > 0;
}
