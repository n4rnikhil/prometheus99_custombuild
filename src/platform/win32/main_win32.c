/**
 * @file main_win32.c
 * @brief Windows shell: window, message loop, GDI band presentation, keyboard / mouse mapping
 *
 * Everything portable lives in src/app.c; this file only translates Win32 events into app_*
 * calls and implements the hooks declared in include/hal/platform.h.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#include "../../../include/app.h"
#include "../../../include/hal/platform.h"
#include "../../../include/hal/sys.h"
#include "../../../include/hal/timer.h"
#include "../../../include/core/alarm_log.h"
#include "../../../include/renderer/layout.h"

#define UI_MODAL_TIMER_ID 1u

static HWND g_hwnd;
static int  g_modal_depth;       /* > 0 while Windows runs a drag / resize / menu loop */

/* ------------------------------------------------------------------------- */
/* Hooks for the application core                                             */
/* ------------------------------------------------------------------------- */
void hal_console_print(const char *text)
{
    /* GUI-subsystem exe: attach to the parent console if there is one. */
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written;
        if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    }
}

void hal_beep(void)
{
    MessageBeep(MB_ICONHAND);
}

void hal_trim_memory(void)
{
    HANDLE hProc = GetCurrentProcess();
    EmptyWorkingSet(hProc);
    SetProcessWorkingSetSize(hProc, (SIZE_T)-1, (SIZE_T)-1);
}

void hal_request_repaint(void)
{
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, FALSE);
        UpdateWindow(g_hwnd);
    }
}

void hal_request_quit(void)
{
    if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
}

void hal_fatal(const char *message)
{
    MessageBoxA(NULL, message, "Industrial HMI", MB_ICONERROR);
}

/* ------------------------------------------------------------------------- */
/* Presentation: one 4-bpp band at a time through StretchDIBits                */
/* ------------------------------------------------------------------------- */
static struct {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[16];
} s_bmi16;

static void present_band(void *ctx, const uint8_t *band, int y, int rows, int width, const uint32_t *palette16)
{
    HDC hdc = (HDC)ctx;
    if (!hdc || !band || rows <= 0 || width <= 0) return;

    s_bmi16.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    s_bmi16.bmiHeader.biWidth         = width;
    s_bmi16.bmiHeader.biHeight        = -rows;   /* top-down DIB */
    s_bmi16.bmiHeader.biPlanes        = 1;
    s_bmi16.bmiHeader.biBitCount      = 4;       /* 16-colour CLUT */
    s_bmi16.bmiHeader.biCompression   = BI_RGB;
    s_bmi16.bmiHeader.biSizeImage     = (DWORD)((width / 2) * rows);
    s_bmi16.bmiHeader.biXPelsPerMeter = 0;
    s_bmi16.bmiHeader.biYPelsPerMeter = 0;
    s_bmi16.bmiHeader.biClrUsed       = 16;
    s_bmi16.bmiHeader.biClrImportant  = 16;
    if (palette16) {
        for (int i = 0; i < 16; i++) {
            s_bmi16.bmiColors[i].rgbRed      = (BYTE)((palette16[i] >> 16) & 0xFF);
            s_bmi16.bmiColors[i].rgbGreen    = (BYTE)((palette16[i] >> 8) & 0xFF);
            s_bmi16.bmiColors[i].rgbBlue     = (BYTE)(palette16[i] & 0xFF);
            s_bmi16.bmiColors[i].rgbReserved = 0;
        }
    }
    StretchDIBits(hdc, 0, y, width, rows, 0, 0, width, rows, band, (const BITMAPINFO *)&s_bmi16,
                  DIB_RGB_COLORS, SRCCOPY);
}

/* ------------------------------------------------------------------------- */
/* Window                                                                     */
/* ------------------------------------------------------------------------- */
static uint16_t map_key(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint16_t)vk;
    if (vk >= '0' && vk <= '9') return (uint16_t)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (uint16_t)('0' + (vk - VK_NUMPAD0));
    switch (vk) {
        case VK_TAB:    return HMI_K_TAB;
        case VK_LEFT:   return HMI_K_LEFT;
        case VK_RIGHT:  return HMI_K_RIGHT;
        case VK_RETURN: return HMI_K_ENTER;
        case VK_ESCAPE: return HMI_K_ESC;
        case VK_BACK:   return HMI_K_BACK;
        case VK_SPACE:  return ' ';
        case VK_DECIMAL:
        case VK_OEM_PERIOD: return '.';
        case VK_SUBTRACT:
        case VK_OEM_MINUS:  return '-';
        default:        return 0;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   /* every pixel is painted by the band streamer */

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            RECT r = { 0, 0, HMI_LAYOUT_MIN_W, HMI_LAYOUT_MIN_H };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }

        case WM_SIZE: {
            int w = LOWORD(lParam), h = HIWORD(lParam);
            if (wParam != SIZE_MINIMIZED && w > 0 && h > 0) {
                app_resize(w, h);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            app_render(present_band, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            app_pointer_down(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEMOVE:
            app_pointer_move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            /* classify first: ReleaseCapture() sends WM_CAPTURECHANGED, which cancels the gesture */
            app_pointer_up(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;

        case WM_CAPTURECHANGED:
            if ((HWND)lParam != hwnd) app_pointer_cancel();
            return 0;

        case WM_KEYDOWN: {
            if (lParam & (1L << 30)) return 0;   /* auto-repeat: one key press = one command */
            uint16_t key = map_key(wParam);
            if (key) app_key(key, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }

        /* Windows runs its own message loop for window drag / resize and the system menu: keep the
         * UI frame (heartbeat, supervision, audit-log sync) alive from a timer meanwhile. */
        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP:
            if (g_modal_depth++ == 0) SetTimer(hwnd, UI_MODAL_TIMER_ID, HMI_UI_FRAME_PERIOD_MS, NULL);
            return 0;

        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP:
            if (g_modal_depth > 0 && --g_modal_depth == 0) KillTimer(hwnd, UI_MODAL_TIMER_ID);
            return 0;

        case WM_TIMER:
            if (wParam == UI_MODAL_TIMER_ID && g_modal_depth > 0) app_frame(hmi_timer_get_ms());
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_modal_depth = 0;
            app_stop();
            g_hwnd = NULL;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static bool create_window(HINSTANCE hInstance)
{
    const char *class_name = "IndustrialHMI_WinClass";
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = class_name;
    RegisterClassExA(&wc);

    RECT wr = { 0, 0, HMI_DISPLAY_WIDTH_DEFAULT, HMI_DISPLAY_HEIGHT_DEFAULT };   /* client area */
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, class_name, "Prometheus HMI - plug-and-play PLC runtime", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
                                NULL, NULL, hInstance, NULL);
    if (!hwnd) return false;
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Crash record: last-resort diagnostics written next to the exe              */
/* ------------------------------------------------------------------------- */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char dir[HMI_PATH_MAX], path[HMI_PATH_MAX + 16], line[256];
    hal_exe_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s%s", dir, "crash.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE && ep && ep->ExceptionRecord) {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        int n = snprintf(line, sizeof(line), "crash: code=0x%08lX offset=0x%llX thread=%lu uptime_ms=%lu\r\n",
                         (unsigned long)ep->ExceptionRecord->ExceptionCode,
                         (unsigned long long)(addr - base), (unsigned long)GetCurrentThreadId(),
                         (unsigned long)hmi_timer_get_ms());
        DWORD w;
        if (n > 0) WriteFile(h, line, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    hmi_alarm_log_close();      /* flush what we have */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Windows 11 throttles timers and CPU speed of background / minimised windows
 * (EcoQoS). A control-room HMI must keep its timing, so opt out. Resolved at
 * run time: older Windows versions simply do not have the call. */
static void disable_power_throttling(void)
{
    typedef struct { ULONG Version, ControlMask, StateMask; } throttling_state_t;
    typedef BOOL (WINAPI *set_info_fn)(HANDLE, int, LPVOID, DWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    set_info_fn fn = k32 ? (set_info_fn)(void (*)(void))GetProcAddress(k32, "SetProcessInformation") : NULL;
    if (!fn) return;
    throttling_state_t st;
    st.Version = 1;                    /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */
    st.ControlMask = 0x1u | 0x4u;      /* EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION */
    st.StateMask = 0;                  /* 0 = throttling OFF for the controlled features */
    fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, (DWORD)sizeof(st));
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    SetUnhandledExceptionFilter(crash_filter);
    int rc = app_parse_args(__argc, __argv);
    if (rc >= 0) return rc;

    disable_power_throttling();
    rc = app_init();
    if (rc != 0) return rc;
    if (!create_window(hInstance)) { app_shutdown(); return 1; }
    if (!app_start_workers()) { app_shutdown(); return 2; }

    MSG msg;
    while (app_running()) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app_stop();
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!app_running()) break;

        /* Sleep until the next frame is due or until input arrives */
        MsgWaitForMultipleObjects(0, NULL, FALSE, app_tick(), QS_ALLINPUT);
    }

    app_shutdown();
    g_hwnd = NULL;
    return 0;
}
