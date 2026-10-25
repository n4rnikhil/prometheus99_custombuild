/**
 * @file sys_win32.c
 * @brief hal/sys.h on Win32
 */

#define _CRT_RAND_S              /* rand_s(): OS random numbers */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../include/hal/sys.h"

void hal_utc_now(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

bool hal_random(void *out, size_t n)
{
    unsigned char *p = (unsigned char *)out;
    while (n > 0) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) return false;
        size_t k = n < sizeof(r) ? n : sizeof(r);
        memcpy(p, &r, k);
        p += k;
        n -= k;
    }
    return true;
}

void hal_exe_dir(char *out, size_t cap)
{
    char path[MAX_PATH];
    if (!out || cap == 0) return;
    out[0] = '\0';
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) return;
    char *slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = '\0';
    snprintf(out, cap, "%.250s", path);
}

uint32_t hal_exe_size(void)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) return 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    return fad.nFileSizeHigh ? 0xFFFFFFFFu : fad.nFileSizeLow;
}

/* --- host load ------------------------------------------------------------ */
static uint64_t s_last_idle, s_last_kernel, s_last_user;
static bool     s_have_baseline;

static uint64_t ft_to_u64(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static uint32_t kb(SIZE_T bytes)
{
    SIZE_T v = bytes / 1024u;
    return (uint32_t)(v > 0xFFFFFFFFu ? 0xFFFFFFFFu : v);
}

void hal_host_stats_init(void)
{
    FILETIME idle, kernel, user;
    s_have_baseline = false;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s_last_idle = ft_to_u64(&idle);
        s_last_kernel = ft_to_u64(&kernel);
        s_last_user = ft_to_u64(&user);
        s_have_baseline = true;
    }
}

void hal_host_stats_poll(hal_host_stats_t *o)
{
    memset(o, 0, sizeof(*o));

    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        uint64_t ci = ft_to_u64(&idle), ck = ft_to_u64(&kernel), cu = ft_to_u64(&user);
        if (s_have_baseline) {
            /* kernel time includes idle time, so busy = (kernel + user) - idle */
            uint64_t idle_d = ci - s_last_idle;
            uint64_t total_d = (ck - s_last_kernel) + (cu - s_last_user);
            if (total_d > 0 && total_d >= idle_d) {
                o->cpu_pct = (int16_t)(((total_d - idle_d) * 100u) / total_d);
                o->cpu_ok = true;
            }
        }
        s_last_idle = ci; s_last_kernel = ck; s_last_user = cu;
        s_have_baseline = true;
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        o->ram_pct = (int16_t)mem.dwMemoryLoad;
        o->ram_ok = true;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        o->proc_private_kb = kb(pmc.PrivateUsage);
        o->proc_resident_kb = kb(pmc.WorkingSetSize);
        o->proc_ok = true;
    }
}
