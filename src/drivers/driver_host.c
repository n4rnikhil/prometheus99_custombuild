/**
 * @file driver_host.c
 * @brief Host telemetry: system CPU load, system RAM load, and this process's memory
 *
 * All values come from documented Win32 APIs. No value is synthesised.
 */

#include "../../include/drivers/driver_host.h"
#include "../../include/renderer/bytecode.h"
#include <windows.h>
#include <psapi.h>

static const hmi_tag_meta_t s_host_meta[HOST_TAG_COUNT] = {
    /* id                   type            name               unit  min  max    warn   trip   dec dig  action1                          action2 */
    { HOST_TAG_CPU_LOAD,     TAG_TYPE_ANALOG, "CPU load",       "%",  0,   100,   85,    97,    0,  0,   "Check for runaway processes",    "HMI loop timing may degrade", NULL },
    { HOST_TAG_RAM_LOAD,     TAG_TYPE_ANALOG, "Memory in use",  "%",  0,   100,   92,    98,    0,  0,   "Close non-essential programs",   "The host may start paging", NULL },
    { HOST_TAG_PROC_PRIV_KB, TAG_TYPE_ANALOG, "HMI private memory", "KB", 0,   16384, 0,     0,     0,  0,   NULL,                             NULL, NULL },
    { HOST_TAG_PROC_WS_KB,   TAG_TYPE_ANALOG, "HMI working set", "KB", 0,   16384, 0,     0,     0,  0,   NULL,                             NULL, NULL },
};

static const uint32_t s_host_manifest[] = {
    HMI_TOKEN_PACK(HOST_TAG_CPU_LOAD,     WIDGET_CONTAINER_CARD, 0, 0, 5, 0),
    HMI_TOKEN_PACK(HOST_TAG_CPU_LOAD,     WIDGET_HERO_DIGIT,     0, 2, 5, 5),
    HMI_TOKEN_PACK(HOST_TAG_CPU_LOAD,     WIDGET_ANALOG_BAR,     0, 9, 5, 2),

    HMI_TOKEN_PACK(HOST_TAG_RAM_LOAD,     WIDGET_CONTAINER_CARD, 5, 0, 5, 0),
    HMI_TOKEN_PACK(HOST_TAG_RAM_LOAD,     WIDGET_HERO_DIGIT,     5, 2, 5, 5),
    HMI_TOKEN_PACK(HOST_TAG_RAM_LOAD,     WIDGET_ANALOG_BAR,     5, 9, 5, 2),

    HMI_TOKEN_PACK(HOST_TAG_PROC_PRIV_KB, WIDGET_CONTAINER_CARD, 10, 0, 6, 0),
    HMI_TOKEN_PACK(HOST_TAG_PROC_PRIV_KB, WIDGET_HERO_DIGIT,     10, 2, 6, 5),
    HMI_TOKEN_PACK(HOST_TAG_PROC_PRIV_KB, WIDGET_ANALOG_BAR,     10, 9, 6, 2),
    HMI_TOKEN_PACK(HOST_TAG_PROC_WS_KB,   WIDGET_ANALOG_BAR,     10, 12, 6, 2),
};

#define HOST_MANIFEST_COUNT ((uint8_t)(sizeof(s_host_manifest) / sizeof(s_host_manifest[0])))
#define HOST_SAMPLE_PERIOD_MS 500u

static uint64_t s_last_idle;
static uint64_t s_last_kernel;
static uint64_t s_last_user;
static uint32_t s_last_sample_ms;
static bool     s_have_baseline;
static bool     s_first_poll = true;

static uint64_t ft_to_u64(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static int16_t clamp_kb(SIZE_T bytes)
{
    SIZE_T kb = bytes / 1024u;
    return (int16_t)(kb > 32767u ? 32767u : kb);
}

static bool host_init(void)
{
    FILETIME idle, kernel, user;
    s_have_baseline = false;
    s_first_poll = true;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s_last_idle = ft_to_u64(&idle);
        s_last_kernel = ft_to_u64(&kernel);
        s_last_user = ft_to_u64(&user);
        s_have_baseline = true;
    }
    return true;
}

static bool host_connect(const char *endpoint) { (void)endpoint; return true; }
static void host_disconnect(void) {}

static bool host_poll_cyclic(hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!tags) return false;
    if (!s_first_poll && (uint32_t)(now_ms - s_last_sample_ms) < HOST_SAMPLE_PERIOD_MS) return true;
    s_first_poll = false;
    s_last_sample_ms = now_ms;

    /* CPU: kernel time includes idle time, so busy = (kernel + user) - idle. */
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        uint64_t ci = ft_to_u64(&idle), ck = ft_to_u64(&kernel), cu = ft_to_u64(&user);
        if (s_have_baseline) {
            uint64_t idle_d = ci - s_last_idle;
            uint64_t total_d = (ck - s_last_kernel) + (cu - s_last_user);
            if (total_d > 0 && total_d >= idle_d) {
                int16_t cpu = (int16_t)(((total_d - idle_d) * 100u) / total_d);
                hmi_tag_write_analog(tags, HOST_TAG_CPU_LOAD, cpu, QUALITY_GOOD);
            }
        }
        s_last_idle = ci; s_last_kernel = ck; s_last_user = cu;
        s_have_baseline = true;
    } else {
        hmi_tag_set_quality(tags, HOST_TAG_CPU_LOAD, QUALITY_COMM_FAULT);
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        hmi_tag_write_analog(tags, HOST_TAG_RAM_LOAD, (int16_t)mem.dwMemoryLoad, QUALITY_GOOD);
    } else {
        hmi_tag_set_quality(tags, HOST_TAG_RAM_LOAD, QUALITY_COMM_FAULT);
    }

    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        hmi_tag_write_analog(tags, HOST_TAG_PROC_PRIV_KB, clamp_kb(pmc.PrivateUsage), QUALITY_GOOD);
        hmi_tag_write_analog(tags, HOST_TAG_PROC_WS_KB, clamp_kb(pmc.WorkingSetSize), QUALITY_GOOD);
    } else {
        hmi_tag_set_quality(tags, HOST_TAG_PROC_PRIV_KB, QUALITY_COMM_FAULT);
        hmi_tag_set_quality(tags, HOST_TAG_PROC_WS_KB, QUALITY_COMM_FAULT);
    }
    return true;
}

static void host_get_stats(hmi_driver_stats_t *out)
{
    if (!out) return;
    ZeroMemory(out, sizeof(*out));
    out->mode = HMI_DRV_MODE_HOST;
    out->link_state = HMI_LINK_UP;
    lstrcpynA(out->endpoint, "LOCAL HOST", (int)sizeof(out->endpoint));
    out->poll_period_ms = HOST_SAMPLE_PERIOD_MS;
}

static const hmi_plc_driver_t s_host_driver = {
    .driver_name     = "Host Machine Telemetry",
    .default_port    = 0,
    .init            = host_init,
    .connect         = host_connect,
    .disconnect      = host_disconnect,
    .poll_cyclic     = host_poll_cyclic,
    .write_coil      = NULL,
    .write_register  = NULL,
    .get_stats       = host_get_stats,
    .ui_bytecode     = s_host_manifest,
    .widget_count    = HOST_MANIFEST_COUNT,
    .tag_metadata    = s_host_meta,
    .tag_count       = HOST_TAG_COUNT
};

const hmi_plc_driver_t* hmi_driver_host_get(void)
{
    return &s_host_driver;
}
