/**
 * @file sys_posix.c
 * @brief hal/sys.h on Linux / Raspberry Pi (procfs) and ESP-IDF (heap figures only)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(ESP_PLATFORM)
#include "esp_random.h"
#include "esp_heap_caps.h"
#elif defined(__linux__)
#include <sys/random.h>
#endif

#include "../../include/hal/sys.h"

void hal_utc_now(char *out, size_t cap)
{
    struct timespec ts;
    struct tm tmv;
    time_t sec;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec = ts.tv_sec;
    gmtime_r(&sec, &tmv);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long)(ts.tv_nsec / 1000000L));
}

bool hal_random(void *out, size_t n)
{
#if defined(ESP_PLATFORM)
    esp_fill_random(out, n);
    return true;
#else
    unsigned char *p = (unsigned char *)out;
#if defined(__linux__)
    while (n > 0) {
        ssize_t r = getrandom(p, n, 0);
        if (r <= 0) break;
        p += r;
        n -= (size_t)r;
    }
    if (n == 0) return true;
#endif
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    bool ok = fread(p, 1, n, f) == n;
    fclose(f);
    return ok;
#endif
}

void hal_exe_dir(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
#if defined(__linux__)
    char path[256];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return;
    slash[1] = '\0';
    snprintf(out, cap, "%.250s", path);
#endif
}

uint32_t hal_exe_size(void)
{
#if defined(__linux__)
    struct stat st;
    if (stat("/proc/self/exe", &st) != 0 || st.st_size < 0) return 0;
    uint64_t size = (uint64_t)st.st_size;            /* off_t is 32-bit on some 32-bit ARM systems */
    return size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
#else
    return 0;
#endif
}

/* --- host load ------------------------------------------------------------ */
#if defined(__linux__)
static uint64_t s_last_busy, s_last_total;
static bool     s_have_baseline;

static bool read_cpu(uint64_t *busy, uint64_t *total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return false;
    uint64_t idle = v[3] + v[4];
    uint64_t sum = 0;
    for (int i = 0; i < 8; i++) sum += v[i];
    *total = sum;
    *busy = sum - idle;
    return true;
}

void hal_host_stats_init(void)
{
    s_have_baseline = read_cpu(&s_last_busy, &s_last_total);
}

void hal_host_stats_poll(hal_host_stats_t *o)
{
    memset(o, 0, sizeof(*o));

    uint64_t busy, total;
    if (read_cpu(&busy, &total)) {
        if (s_have_baseline && total > s_last_total) {
            o->cpu_pct = (int16_t)(((busy - s_last_busy) * 100u) / (total - s_last_total));
            if (o->cpu_pct > 100) o->cpu_pct = 100;
            o->cpu_ok = true;
        }
        s_last_busy = busy; s_last_total = total; s_have_baseline = true;
    }

    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[96];
        unsigned long long mem_total = 0, mem_avail = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %llu kB", &mem_total) == 1) continue;
            if (sscanf(line, "MemAvailable: %llu kB", &mem_avail) == 1) break;
        }
        fclose(f);
        if (mem_total > 0 && mem_avail <= mem_total) {
            o->ram_pct = (int16_t)(((mem_total - mem_avail) * 100u) / mem_total);
            o->ram_ok = true;
        }
    }

    f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long long size = 0, resident = 0, shared = 0;
        if (fscanf(f, "%llu %llu %llu", &size, &resident, &shared) == 3) {
            long page_kb = sysconf(_SC_PAGESIZE) / 1024;
            if (page_kb <= 0) page_kb = 4;
            o->proc_resident_kb = (uint32_t)(resident * (unsigned long long)page_kb);
            o->proc_private_kb = (uint32_t)((resident > shared ? resident - shared : 0) * (unsigned long long)page_kb);
            o->proc_ok = true;
        }
        fclose(f);
    }
}

#elif defined(ESP_PLATFORM)

void hal_host_stats_init(void) {}

void hal_host_stats_poll(hal_host_stats_t *o)
{
    memset(o, 0, sizeof(*o));
    size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (total > 0 && freeb <= total) {
        o->ram_pct = (int16_t)(((total - freeb) * 100u) / total);
        o->ram_ok = true;
        o->proc_private_kb = (uint32_t)((total - freeb) / 1024u);
        o->proc_resident_kb = o->proc_private_kb;
        o->proc_ok = true;
    }
}

#else

void hal_host_stats_init(void) {}
void hal_host_stats_poll(hal_host_stats_t *o) { memset(o, 0, sizeof(*o)); }

#endif
