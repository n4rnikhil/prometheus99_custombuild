/**
 * @file display_win32.c
 * @brief Win32 Hardware Abstraction Layer - GDI Direct Band Raster Presentation
 * @standards Zero heap allocations, direct hardware StretchDIBits, L1 cache streaming
 */

#include "../../include/hal/display.h"
#include <psapi.h>
#include <stdio.h>

static struct {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[16];
} s_bmi16;

bool hal_display_init(hal_display_t *disp, HINSTANCE hInstance, int width, int height, const char *title, WNDPROC wndProc)
{
    if (!disp || !wndProc) return false;

    disp->width = width;
    disp->height = height;
    disp->fullscreen = false;

    const char *CLASS_NAME = "IndustrialHMI_WinClass";

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;

    RegisterClassExA(&wc);

    /* Calculate window rect for requested client area */
    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExA(
        0,
        CLASS_NAME,
        title ? title : "Industrial HMI Runtime",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (!hwnd) return false;

    disp->hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    return true;
}

void hal_display_present_band(HDC hdc, const uint8_t *band_buf,
                              int band_y, int band_h, int screen_w,
                              const uint32_t *palette16)
{
    if (!hdc || !band_buf || band_h <= 0 || screen_w <= 0) return;

    /* Populate 4-bpp BITMAPINFO Header */
    s_bmi16.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    s_bmi16.bmiHeader.biWidth         = screen_w;
    s_bmi16.bmiHeader.biHeight        = -band_h; /* Top-down DIB */
    s_bmi16.bmiHeader.biPlanes        = 1;
    s_bmi16.bmiHeader.biBitCount      = 4;       /* 16-color CLUT */
    s_bmi16.bmiHeader.biCompression   = BI_RGB;
    s_bmi16.bmiHeader.biSizeImage     = (DWORD)((screen_w / 2) * band_h);
    s_bmi16.bmiHeader.biXPelsPerMeter = 0;
    s_bmi16.bmiHeader.biYPelsPerMeter = 0;
    s_bmi16.bmiHeader.biClrUsed       = 16;
    s_bmi16.bmiHeader.biClrImportant  = 16;

    /* Populate 16-color CLUT into GDI RGBQUADs (0x00RRGGBB format) */
    if (palette16) {
        for (int i = 0; i < 16; i++) {
            s_bmi16.bmiColors[i].rgbRed      = (BYTE)((palette16[i] >> 16) & 0xFF);
            s_bmi16.bmiColors[i].rgbGreen    = (BYTE)((palette16[i] >> 8) & 0xFF);
            s_bmi16.bmiColors[i].rgbBlue     = (BYTE)(palette16[i] & 0xFF);
            s_bmi16.bmiColors[i].rgbReserved = 0;
        }
    }

    StretchDIBits(
        hdc,
        0, band_y, screen_w, band_h,
        0, 0, screen_w, band_h,
        band_buf,
        (const BITMAPINFO*)&s_bmi16,
        DIB_RGB_COLORS,
        SRCCOPY
    );
}

void hal_display_set_title(HWND hwnd, const char *title)
{
    if (hwnd && title) {
        SetWindowTextA(hwnd, title);
    }
}

void hal_display_trim_memory(void)
{
    HANDLE hProc = GetCurrentProcess();
    EmptyWorkingSet(hProc);
    SetProcessWorkingSetSize(hProc, (SIZE_T)-1, (SIZE_T)-1);
}

void hal_display_shutdown(hal_display_t *disp)
{
    if (!disp) return;
    if (disp->hwnd) {
        disp->hwnd = NULL;
    }
}
