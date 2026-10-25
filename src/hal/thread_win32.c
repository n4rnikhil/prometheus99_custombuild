/**
 * @file thread_win32.c
 * @brief hal/thread.h on Win32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../include/hal/thread.h"

static DWORD WINAPI trampoline(LPVOID p)
{
    hal_thread_t *t = (hal_thread_t *)p;
    t->fn(t->arg);
    return 0;
}

bool hal_thread_start(hal_thread_t *t, void (*fn)(void *), void *arg, hal_prio_t prio, uint32_t stack_bytes)
{
    if (!t || !fn) return false;
    t->fn = fn;
    t->arg = arg;
    t->started = false;
    HANDLE h = CreateThread(NULL, stack_bytes, trampoline, t, 0, NULL);
    if (!h) return false;
    SetThreadPriority(h, prio == HAL_PRIO_HIGH ? THREAD_PRIORITY_TIME_CRITICAL
                       : prio == HAL_PRIO_LOW  ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
    t->handle = (uintptr_t)h;
    t->started = true;
    return true;
}

void hal_thread_join(hal_thread_t *t, uint32_t timeout_ms)
{
    if (!t || !t->started) return;
    WaitForSingleObject((HANDLE)t->handle, timeout_ms);
    CloseHandle((HANDLE)t->handle);
    t->started = false;
}
