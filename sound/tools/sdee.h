/* sdee.h - the game's EE-side sound protocol for the tools (sdee.c). */
#ifndef DOTR_SDEE_H
#define DOTR_SDEE_H

extern unsigned int sdee_reply[4];
unsigned sdee_rpc(unsigned cmd, unsigned a1, unsigned a2, unsigned a3, unsigned a4);
int      sdee_boot(void);                 /* KCEJEAST from the open disc into the IOP */
unsigned sdee_init_device(void);          /* SD_InitializeDevice; returns the IOP sound buffer */
int      sdee_load_group(unsigned iopbuf, int slot, const unsigned char *grp, unsigned size);

#endif
