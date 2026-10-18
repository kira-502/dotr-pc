/* iso9660.h - read files from the game disc image (tools only). */
#ifndef DOTR_ISO9660_H
#define DOTR_ISO9660_H

int iso_open(const char *path);
/* Whole file by path (e.g. "IOP/KCEJEAST.IRX"); malloc'd, NULL if missing. */
unsigned char *iso_read_file(const char *path, unsigned *size_out);

#endif
