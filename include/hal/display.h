/**
 * @file display.h
 * @brief Hardware Abstraction Layer - Display & Window Presentation
 * @standards Apple HIG Ergonomics / ISA-101 Situational Awareness
 */

#ifndef INDUSTRIAL_HMI_DISPLAY_H
#define INDUSTRIAL_HMI_DISPLAY_H

#include "../config.h"
#include <windows.h>

typedef struct {
    HWND hwnd;
    int  width;
    int  height;
    bool fullscreen;
} hal_display_t;

/* Initialize display subsystem and create window */
bool hal_display_init(hal_display_t *disp, HINSTANCE hInstance, int width, int height, const char *title, WNDPROC wndProc);

/* Present a single 4-bpp elastic band to the target HDC */
void hal_display_present_band(HDC hdc, const uint8_t *band_buf,
                              int band_y, int band_h, int screen_w,
                              const uint32_t *palette16);

/* Set window title with system status and current role */
void hal_display_set_title(HWND hwnd, const char *title);

/* Trim process working set (RAM footprint optimization) */
void hal_display_trim_memory(void);

/* Clean up display resources */
void hal_display_shutdown(hal_display_t *disp);

#endif /* INDUSTRIAL_HMI_DISPLAY_H */
