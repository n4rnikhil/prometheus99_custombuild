/**
 * @file main.c
 * @brief Industrial HMI Runtime Application Entry Point
 * @standards IEC 62443 / IEC 61508 SIL-ready / Apple HIG Ergonomics
 *
 * Fully static memory budget: Zero heap allocations.
 * Lock-free Seqlock, Pluggable Schneider PLC Driver, 18.75 KB L1-Cache Band Buffer.
 */

#include "../include/config.h"
#include "../include/core/seqlock.h"
#include "../include/core/tags.h"
#include "../include/core/alarms.h"
#include "../include/core/watchdog.h"
#include "../include/core/security.h"
#include "../include/core/crc32.h"
#include "../include/renderer/micro_renderer.h"
#include "../include/drivers/plc_driver.h"
#include "../include/drivers/driver_modbus.h"
#include "../include/drivers/driver_host.h"
#include "../include/hal/timer.h"
#include "../include/hal/input_queue.h"
#include "../include/hal/display.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <psapi.h>

/* Forward declare Win32 display hook */
void hmi_display_set_target_hdc(HDC hdc);

/* --- Global Subsystems (100% Static BSS) --- */
static hmi_seqlock_t          g_seqlock;
static hmi_tag_table_t        g_shared_tags;
static hmi_alarm_supervisor_t g_shared_alarms;
static hmi_watchdog_t         g_watchdog;
static hmi_security_ctx_t     g_security;
static hmi_renderer_t         g_renderer;

static const hmi_plc_driver_t *g_modbus_driver = NULL;
static const hmi_plc_driver_t *g_host_driver = NULL;

static bool     g_running = true;
static HWND     g_hwnd = NULL;
static HANDLE   g_fieldbus_thread = NULL;
static uint8_t  g_active_screen = SCREEN_PROCESS_OVERVIEW;
static bool     g_horn_active = false;

static const char *WND_CLASS_NAME = "IndustrialHMIClass";

/* --- High-Speed 1,000 Hz Fieldbus Background Thread --- */
static unsigned __stdcall fieldbus_worker_thread(void *arg)
{
    (void)arg;
    hmi_tag_table_t local_tags;
    hmi_alarm_supervisor_t local_alarms;

    hmi_tags_init(&local_tags);
    hmi_alarms_init(&local_alarms);

    while (g_running) {
        uint32_t now = hmi_timer_get_ms();

        /* 1. Poll Pluggable PLC Drivers */
        if (g_modbus_driver && g_modbus_driver->poll_cyclic) {
            g_modbus_driver->poll_cyclic(&local_tags, now);
        }
        if (g_host_driver && g_host_driver->poll_cyclic) {
            g_host_driver->poll_cyclic(&local_tags, now);
        }

        /* 2. Evaluate ISA-18.2 Alarms */
        hmi_alarms_evaluate(&local_alarms, &local_tags, now);

        /* 3. Independent Watchdog Heartbeat */
        hmi_watchdog_ping_fieldbus(&g_watchdog, now);
        hmi_watchdog_evaluate(&g_watchdog, now);

        /* 4. Atomic Lock-Free Publish to UI Thread via Seqlock */
        hmi_seqlock_write_begin(&g_seqlock);
        g_shared_tags = local_tags;
        g_shared_alarms = local_alarms;
        hmi_seqlock_write_end(&g_seqlock);

        /* Accurate 1 ms sleep via timeBeginPeriod(1) */
        hmi_timer_sleep_ms(1);
    }
    return 0;
}

/* --- Win32 Window Procedure --- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_SIZE: {
            int new_w = LOWORD(lParam);
            int new_h = HIWORD(lParam);
            if (new_w > 100 && new_h > 100) {
                hmi_renderer_resize(&g_renderer, new_w, new_h);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            hmi_display_set_target_hdc(hdc);

            /* 1. Lock-Free Atomic State Snapshot via Seqlock */
            hmi_tag_table_t snap_tags;
            hmi_alarm_supervisor_t snap_alarms;
            uint32_t seq;
            do {
                seq = hmi_seqlock_read_begin(&g_seqlock);
                snap_tags = g_shared_tags;
                snap_alarms = g_shared_alarms;
            } while (hmi_seqlock_read_retry(&g_seqlock, seq));

            /* 2. Select UI Bytecode Manifest based on Active Screen */
            const uint32_t *tokens = NULL;
            uint8_t token_count = 0;
            if (g_active_screen == SCREEN_PROCESS_OVERVIEW && g_modbus_driver) {
                tokens = g_modbus_driver->ui_bytecode;
                token_count = g_modbus_driver->widget_count;
            } else if (g_active_screen == SCREEN_HOST_TELEMETRY && g_host_driver) {
                tokens = g_host_driver->ui_bytecode;
                token_count = g_host_driver->widget_count;
            }

            /* 3. Render Elastic Bands directly into L1-Cache (Zero Heap!) */
            for (int b = 0; b < g_renderer.band_count; b++) {
                hmi_renderer_render_band(&g_renderer, b, tokens, token_count,
                                         &snap_tags, &snap_alarms,
                                         &g_security, &g_watchdog, g_active_screen);

                int band_y = b * g_renderer.band_h;
                int band_h = g_renderer.band_h;
                if ((band_y + band_h) > g_renderer.screen_h) {
                    band_h = g_renderer.screen_h - band_y;
                }

                hmi_display_flush_band(0, band_y, g_renderer.screen_w, band_h,
                                       hmi_renderer_get_band_buffer(&g_renderer),
                                       hmi_renderer_get_palette(&g_renderer));
            }

            hmi_display_set_target_hdc(NULL);
            EndPaint(hwnd, &ps);
            EmptyWorkingSet(GetCurrentProcess());
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            uint32_t now = hmi_timer_get_ms();
            hmi_security_register_activity(&g_security, now);

            /* Check if clicking Emergency Action Banner */
            if (y >= 48 && y <= 110) {
                hmi_alarms_acknowledge(&g_shared_alarms, now);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            /* Check Bottom Navigation Dock */
            int dock_y = g_renderer.screen_h - 44;
            if (y >= dock_y) {
                int tab_w = (g_renderer.screen_w - 32) / 5;
                int tab_idx = (x - 16) / tab_w;
                if (tab_idx >= 0 && tab_idx < 5) {
                    g_active_screen = (uint8_t)tab_idx;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            uint32_t now = hmi_timer_get_ms();
            hmi_security_register_activity(&g_security, now);

            if (wParam >= '1' && wParam <= '5') {
                g_active_screen = (uint8_t)(wParam - '1');
            } else if (wParam == 'A' || wParam == VK_SPACE) {
                hmi_alarms_acknowledge(&g_shared_alarms, now);
            } else if (wParam == 'C') {
                hmi_renderer_set_theme(&g_renderer, !g_renderer.high_contrast);
            } else if (wParam == 'P') {
                /* Toggle Pump A via Modbus Coil 1 */
                if (g_modbus_driver && g_modbus_driver->write_coil) {
                    int16_t cur = hmi_tag_read_value(&g_shared_tags, SCHNEIDER_TAG_PUMP_RUNNING);
                    g_modbus_driver->write_coil(1, !cur);
                }
            } else if (wParam == 'E') {
                /* Trigger Emergency Stop */
                hmi_alarms_trigger_estop(&g_shared_alarms, now);
                if (g_modbus_driver && g_modbus_driver->write_coil) {
                    g_modbus_driver->write_coil(5, true);
                }
            } else if (wParam == 'W') {
                hmi_watchdog_inject_fault(&g_watchdog);
            } else if (wParam == 'R') {
                /* Cycle RBAC Role */
                hmi_role_t next_role = (hmi_role_t)((g_security.current_role + 1) % 4);
                hmi_security_set_role(&g_security, next_role, now);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("=====================================================\n");
    printf("   AEGIS Industrial HMI Runtime (C99 Clean Architecture)\n");
    printf("   Standards: IEC 62443 / IEC 61508 / Apple HIG Ergonomics\n");
    printf("=====================================================\n");

    /* 1. Initialize HAL Subsystems */
    hmi_timer_init();
    hmi_input_init();
    hmi_display_init();

    /* 2. Initialize Core Subsystems */
    hmi_seqlock_init(&g_seqlock);
    hmi_tags_init(&g_shared_tags);
    hmi_alarms_init(&g_shared_alarms);
    hmi_security_init(&g_security);
    hmi_watchdog_init(&g_watchdog, hmi_timer_get_ms());

    /* 3. Initialize Pluggable Drivers */
    g_modbus_driver = hmi_driver_modbus_get();
    g_host_driver = hmi_driver_host_get();

    if (g_modbus_driver && g_modbus_driver->init) {
        g_modbus_driver->init();
        hmi_tag_register_meta(g_modbus_driver->tag_metadata, g_modbus_driver->tag_count);
    }
    if (g_host_driver && g_host_driver->init) {
        g_host_driver->init();
    }

    /* 4. Initialize Declarative Micro-Rasterizer */
    hmi_renderer_init(&g_renderer, HMI_DISPLAY_WIDTH_DEFAULT, HMI_DISPLAY_HEIGHT_DEFAULT);

    /* 5. Register Win32 Window Class */
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WND_CLASS_NAME;
    RegisterClassExA(&wc);

    RECT rc = {0, 0, HMI_DISPLAY_WIDTH_DEFAULT, HMI_DISPLAY_HEIGHT_DEFAULT};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExA(
        0, WND_CLASS_NAME,
        "AEGIS Industrial HMI: Schneider Electric Modicon M340/M580",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL
    );

    if (!g_hwnd) {
        fprintf(stderr, "[ERROR] Window Creation Failed.\n");
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    /* 6. Launch 1,000 Hz Fieldbus Background Thread */
    g_fieldbus_thread = (HANDLE)_beginthreadex(NULL, 16384, fieldbus_worker_thread, NULL, 0, NULL);
    if (!g_fieldbus_thread) {
        fprintf(stderr, "[ERROR] Fieldbus Thread Launch Failed.\n");
        return 1;
    }

    printf("[READY] Fieldbus: 1,000 Hz | UI: 30 Hz | Display: Elastic L1 Band\n");
    printf("[KEYS]  [1-5] Screens | [A/Space] Ack Alarm | [P] Pump | [E] E-Stop | [C] Contrast\n");

    /* 7. Master Event Loop (30 Hz UI Tick + Message Pump) */
    MSG msg;
    uint32_t last_ui_tick = hmi_timer_get_ms();

    while (g_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        uint32_t now = hmi_timer_get_ms();
        if ((now - last_ui_tick) >= HMI_UI_FRAME_PERIOD_MS) {
            last_ui_tick = now;

            /* Watchdog Heartbeat from UI side */
            hmi_watchdog_ping_ui(&g_watchdog, now);

            /* Security Inactivity Timeout Check */
            hmi_security_check_timeout(&g_security, now);

            /* Alarm Audio Supervisor: Non-annoying calm alert pulse */
            bool trip_sounding = g_shared_alarms.horn_sounding;
            if (trip_sounding != g_horn_active) {
                g_horn_active = trip_sounding;
                if (trip_sounding) {
                    MessageBeep(MB_ICONHAND);
                }
            }

            /* Refresh Display */
            InvalidateRect(g_hwnd, NULL, FALSE);
        }

        hmi_timer_sleep_ms(5);
    }

    /* Cleanup */
    if (g_fieldbus_thread) {
        WaitForSingleObject(g_fieldbus_thread, 1000);
        CloseHandle(g_fieldbus_thread);
    }
    hmi_timer_shutdown();
    UnregisterClassA(WND_CLASS_NAME, hInst);

    printf("[EXIT] HMI Terminated Gracefully.\n");
    return 0;
}
