/* What DotR.exe (loader) hands to dotr.dll (runtime). Shared by loader/ and runtime/. */
#ifndef DOTR_LOADER_ABI_H
#define DOTR_LOADER_ABI_H

#include <stdint.h>
#include <wchar.h>

#define DOTR_LOADER_ABI     1u

#define DOTR_RAM_MAP_BASE   0x00100000u   /* EE RAM mapped from here ... */
#define DOTR_RAM_MAP_END    0x02000000u   /* ... to the end of the 32 MB */
#define DOTR_RAM_MAP_SIZE   (DOTR_RAM_MAP_END - DOTR_RAM_MAP_BASE)
#define DOTR_SPR_BASE       0x70000000u
#define DOTR_SPR_SIZE       0x00004000u
#define DOTR_STACKS_BASE    0x02000000u
#define DOTR_STACKS_SIZE    0x00400000u

typedef struct dotr_memory_map {
    uint32_t abi_version;
    int      ok;            /* 1 = every region is at its address */
    uint32_t error_step;    /* 1 placeholder misplaced, 2 RAM not image-backed, 5 scratchpad */
    uint32_t error_code;    /* GetLastError() at that step */
    void    *ram, *spr, *stacks;   /* uncached mirrors are not mapped */
} dotr_memory_map;

typedef int (__cdecl *dotr_main_fn)(dotr_memory_map *map, const wchar_t *cmdline);

#endif
