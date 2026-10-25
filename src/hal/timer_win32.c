/**
 * @file timer.c
 * @brief QPC time base, 1 ms scheduler resolution and a drift-free periodic pacer
 */

#include "../../include/hal/timer.h"
#include <windows.h>
#include <timeapi.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static LARGE_INTEGER s_qpc_freq;
static LARGE_INTEGER s_qpc_boot;

void hmi_timer_init(void)
{
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&s_qpc_freq);
    QueryPerformanceCounter(&s_qpc_boot);
    if (s_qpc_freq.QuadPart <= 0) s_qpc_freq.QuadPart = 1;
}

void hmi_timer_shutdown(void)
{
    timeEndPeriod(1);
}

uint64_t hmi_timer_get_us(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)(now.QuadPart - s_qpc_boot.QuadPart);
    uint64_t freq = (uint64_t)s_qpc_freq.QuadPart;
    /* Split to avoid overflowing ticks * 1e6 */
    return (ticks / freq) * 1000000u + ((ticks % freq) * 1000000u) / freq;
}

uint32_t hmi_timer_get_ms(void)
{
    return (uint32_t)(hmi_timer_get_us() / 1000u);
}

void hmi_timer_sleep_ms(uint32_t ms)
{
    Sleep(ms);
}

bool hmi_pacer_init(hmi_pacer_t *p, uint32_t period_us)
{
    if (!p || period_us == 0) return false;
    p->period_us = period_us;
    p->overruns = 0;
    p->handle = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    p->high_res = (p->handle != NULL);
    p->next_deadline_us = hmi_timer_get_us() + period_us;
    return true;
}

void hmi_pacer_wait(hmi_pacer_t *p)
{
    if (!p) return;
    uint64_t now = hmi_timer_get_us();

    if (now > p->next_deadline_us + 10u * (uint64_t)p->period_us) {
        /* Far behind (stall, debugger, suspend): re-base instead of bursting. */
        p->overruns++;
        p->next_deadline_us = now + p->period_us;
        return;
    }

    if (now < p->next_deadline_us) {
        uint64_t wait_us = p->next_deadline_us - now;
        if (p->high_res) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(wait_us * 10u);    /* relative, 100 ns units */
            if (SetWaitableTimer((HANDLE)p->handle, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject((HANDLE)p->handle, INFINITE);
            } else {
                Sleep(1);
            }
        } else {
            Sleep(1);   /* ~1 ms with timeBeginPeriod(1) */
        }
    }
    p->next_deadline_us += p->period_us;
}

void hmi_pacer_close(hmi_pacer_t *p)
{
    if (!p) return;
    if (p->handle) {
        CloseHandle((HANDLE)p->handle);
        p->handle = NULL;
    }
    p->high_res = false;
}
