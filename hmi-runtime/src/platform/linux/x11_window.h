/**
 * @file x11_window.h
 * @brief Optional desktop window for the Linux shell (X11 / XWayland, libX11 loaded at run time)
 */
#ifndef HMI_X11_WINDOW_H
#define HMI_X11_WINDOW_H
#include <stdbool.h>
#include <stdint.h>

bool x11_available(void);                            /* a desktop session ($DISPLAY) is present */
bool x11_open(int w, int h, const char *title);      /* false: no libX11 or no display - fall back */
int  x11_fd(void);                                   /* poll() this for input */
void x11_size(int *w, int *h);
void x11_present_band(void *ctx, const uint8_t *band, int y, int rows, int width, const uint32_t *pal);
void x11_flush(void);
bool x11_events(void);                               /* deliver input to the app; true = repaint */
void x11_close(void);

#endif
