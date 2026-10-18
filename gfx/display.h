/* display.h - the game window (Direct3D 11). Only used when gfx is not headless. */
#ifndef DOTR_DISPLAY_H
#define DOTR_DISPLAY_H

#include "hw.h"

int  display_open(int scale);
void display_close(void);
void display_present(const hw_u32 *rgba, int w, int h);
/* With the GPU renderer: the window uses its device (call before display_open), and presents its picture texture
 * (an ID3D11Texture2D, R8G8B8A8 family) of which the top-left w x h is the TV picture. */
void display_use_device(void *d3d11_device);
void display_present_texture(void *texture, int w, int h);
void display_pump(void);
int  display_focused(void);
int  display_quit(void);
int  display_take_f12(void);   /* 1 once per F12 press */
double display_now_ms(void);   /* a steady clock for the speed line in the log */

/* For the headless tests: where a w x h picture goes in the back buffer of a cw x ch window, and the present
 * path run on an off-screen target and read back (0 = every pixel right). */
void display_layout(int w, int h, int cw, int ch, int *nw, int *nh, int *x, int *y);
int  display_selftest(const hw_u32 *rgba, int w, int h, int cw, int ch, int *mismatches);

#endif
