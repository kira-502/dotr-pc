/* ps2card.h - read the file system of a PS2 memory card image (PCSX2 .ps2: 8 MB, pages of 512 bytes, with or
 * without 16 bytes of ECC after each page) and copy its saves into the PC port's save folders. */
#ifndef DOTR_PS2CARD_H
#define DOTR_PS2CARD_H

#include <stdint.h>

/* Import every save directory of the image into dest (e.g. "...\Saves\card1"), as dest\DIR\FILE, the layout of
 * runtime/memcard.c. Saves that already exist are skipped unless overwrite. Returns the number of saves copied,
 * or -1 when the file is not a PS2 card. The report lists what was done, one line per save. */
int ps2card_import(const char *image, const char *dest, int overwrite, char *report, int report_size);

/* For tests: build a formatted 8 MB card image (ecc = 1 adds the 16-byte spare area per page) holding one save
 * directory with the given files. */
int ps2card_make_test_image(const char *path, int ecc, const char *dir_name, int nfiles, const char *const *names,
                            const uint8_t *const *data, const uint32_t *sizes);

#endif
