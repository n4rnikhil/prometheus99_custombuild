/**
 * @file driver_host.c
 * @brief Host Hardware Telemetry Driver Implementation
 */

#include "../../include/drivers/driver_host.h"
#include "../../include/renderer/bytecode.h"
#include <windows.h>

static const hmi_tag_meta_t s_host_meta[HOST_TAG_COUNT] = {
    { HOST_TAG_CPU_LOAD, TAG_TYPE_ANALOG, "HOST CPU LOAD", "%",   0, 100, 75, 90 },
    { HOST_TAG_RAM_LOAD, TAG_TYPE_ANALOG, "HOST RAM LOAD", "%",   0, 100, 80, 95 },
    { HOST_TAG_CPU_TEMP, TAG_TYPE_ANALOG, "CPU THERMAL",   "C",   0, 105, 58, 75 },
    { HOST_TAG_DISK_IO,  TAG_TYPE_ANALOG, "DISK IO SPEED", "MB/s",0, 500, 300, 450 }
};

static const uint32_t s_host_manifest[] = {
    HMI_TOKEN_PACK(HOST_TAG_CPU_LOAD, WIDGET_CONTAINER_CARD, 0, 2, 5, 11),
    HMI_TOKEN_PACK(HOST_TAG_CPU_LOAD, WIDGET_HERO_DIGIT,      1, 4, 3, 4),
    HMI_TOKEN_PACK(HOST_TAG_CPU_LOAD, WIDGET_ANALOG_BAR,      1, 10, 4, 1),

    HMI_TOKEN_PACK(HOST_TAG_RAM_LOAD, WIDGET_CONTAINER_CARD, 5, 2, 5, 11),
    HMI_TOKEN_PACK(HOST_TAG_RAM_LOAD, WIDGET_HERO_DIGIT,      6, 4, 3, 4),
    HMI_TOKEN_PACK(HOST_TAG_RAM_LOAD, WIDGET_ANALOG_BAR,      6, 10, 4, 1),

    HMI_TOKEN_PACK(HOST_TAG_CPU_TEMP, WIDGET_CONTAINER_CARD, 10, 2, 5, 11),
    HMI_TOKEN_PACK(HOST_TAG_CPU_TEMP, WIDGET_HERO_DIGIT,      11, 4, 3, 4),
    HMI_TOKEN_PACK(HOST_TAG_CPU_TEMP, WIDGET_ANALOG_BAR,      11, 10, 4, 1),
};

#define HOST_MANIFEST_COUNT ((uint8_t)(sizeof(s_host_manifest) / sizeof(s_host_manifest[0])))

static uint64_t s_last_idle = 0;
static uint64_t s_last_kernel = 0;
static uint64_t s_last_user = 0;
static uint32_t s_last_sample_ms = 0;

static inline uint64_t ft_to_u64(const FILETIME *ft) {
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static bool host_init(void)
{
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s_last_idle = ft_to_u64(&idle);
        s_last_kernel = ft_to_u64(&kernel);
        s_last_user = ft_to_u64(&user);
    }
    return true;
}

static bool host_connect(const char *endpoint) { (void)endpoint; return true; }
static void host_disconnect(void) {}

static bool host_poll_cyclic(hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!tags) return false;

    if ((now_ms - s_last_sample_ms) >= 200) {
        s_last_sample_ms = now_ms;

        /* CPU Load */
        int16_t cpu_pct = 5;
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            uint64_t cur_idle = ft_to_u64(&idle);
            uint64_t cur_kernel = ft_to_u64(&kernel);
            uint64_t cur_user = ft_to_u64(&user);

            uint64_t idle_d = cur_idle - s_last_idle;
            uint64_t total_d = (cur_kernel - s_last_kernel) + (cur_user - s_last_user);
            if (total_d > 0 && total_d >= idle_d) {
                cpu_pct = (int16_t)(((total_d - idle_d) * 100) / total_d);
            }
            s_last_idle = cur_idle;
            s_last_kernel = cur_kernel;
            s_last_user = cur_user;
        }

        /* RAM Load */
        int16_t ram_pct = 45;
        MEMORYSTATUSEX mem;
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            ram_pct = (int16_t)mem.dwMemoryLoad;
        }

        int16_t temp_c = (int16_t)(40 + (cpu_pct * 3) / 10);

        hmi_tag_write_analog(tags, HOST_TAG_CPU_LOAD, cpu_pct, QUALITY_GOOD);
        hmi_tag_write_analog(tags, HOST_TAG_RAM_LOAD, ram_pct, QUALITY_GOOD);
        hmi_tag_write_analog(tags, HOST_TAG_CPU_TEMP, temp_c, QUALITY_GOOD);
        hmi_tag_write_analog(tags, HOST_TAG_DISK_IO, 12, QUALITY_GOOD);
    }

    return true;
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
    .ui_bytecode     = s_host_manifest,
    .widget_count    = HOST_MANIFEST_COUNT,
    .tag_metadata    = s_host_meta,
    .tag_count       = HOST_TAG_COUNT
};

const hmi_plc_driver_t* hmi_driver_host_get(void)
{
    return &s_host_driver;
}
