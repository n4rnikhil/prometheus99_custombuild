/**
 * @file timer.c
 * @brief High-Resolution Precision System Timer Implementation
 */

#include "../../include/hal/timer.h"
#include <windows.h>
#include <timeapi.h>

static LARGE_INTEGER s_qpc_freq;
static LARGE_INTEGER s_qpc_boot;

void hmi_timer_init(void)
{
    /* Request 1 ms high-resolution timer from Windows multimedia subsystem */
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&s_qpc_freq);
    QueryPerformanceCounter(&s_qpc_boot);
}

void hmi_timer_shutdown(void)
{
    timeEndPeriod(1);
}

uint32_t hmi_timer_get_ms(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t diff = now.QuadPart - s_qpc_boot.QuadPart;
    return (uint32_t)((diff * 1000) / s_qpc_freq.QuadPart);
}

void hmi_timer_sleep_ms(uint32_t ms)
{
    Sleep(ms);
}
