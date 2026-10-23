/**
 * @file display_win32.c
 * @brief Win32 GDI 4-Bit DIB Band Flush Driver Implementation
 */

#include "../../include/hal/display.h"
#include <windows.h>
#include <string.h>

static HDC s_target_hdc = NULL;
static struct {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[16];
} s_dib_info;

void hmi_display_init(void)
{
    memset(&s_dib_info, 0, sizeof(s_dib_info));
    s_dib_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_dib_info.bmiHeader.biPlanes = 1;
    s_dib_info.bmiHeader.biBitCount = 4;
    s_dib_info.bmiHeader.biCompression = BI_RGB;
    s_dib_info.bmiHeader.biClrUsed = 16;
    s_dib_info.bmiHeader.biClrImportant = 16;
}

void hmi_display_set_target_hdc(HDC hdc)
{
    s_target_hdc = hdc;
}

void hmi_display_flush_band(int x, int y, int w, int h, const uint8_t *pixels_4bit, const uint32_t *palette16)
{
    if (!s_target_hdc || !pixels_4bit || !palette16 || w <= 0 || h <= 0) return;

    /* Setup bitmap geometry for this band */
    s_dib_info.bmiHeader.biWidth = w;
    s_dib_info.bmiHeader.biHeight = -h; /* Top-down DIB */

    /* Copy current palette into DIB table */
    for (int i = 0; i < 16; i++) {
        uint32_t c = palette16[i];
        s_dib_info.bmiColors[i].rgbRed   = (BYTE)((c >> 16) & 0xFF);
        s_dib_info.bmiColors[i].rgbGreen = (BYTE)((c >> 8)  & 0xFF);
        s_dib_info.bmiColors[i].rgbBlue  = (BYTE)(c & 0xFF);
        s_dib_info.bmiColors[i].rgbReserved = 0;
    }

    StretchDIBits(s_target_hdc,
                  x, y, w, h,
                  0, 0, w, h,
                  pixels_4bit,
                  (const BITMAPINFO*)&s_dib_info,
                  DIB_RGB_COLORS,
                  SRCCOPY);
}
