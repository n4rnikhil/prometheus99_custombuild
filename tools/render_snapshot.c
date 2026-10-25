/**
 * @file render_snapshot.c
 * @brief Portable, OS-free reference "platform layer": renders HMI screens to BMP files.
 *
 * Uses ONLY the C standard library plus the portable core (tags, alarms,
 * security, layout, renderer, fonts, device profile). It is the smallest
 * possible port of the HMI and shows exactly what a Linux framebuffer or a
 * microcontroller LCD port has to provide: a place to put each rendered band.
 *
 *   gcc -std=c99 -O2 -I include tools/render_snapshot.c src/core/tags.c src/core/alarms.c \
 *       src/core/security.c src/core/sha256.c src/core/crc32.c src/core/watchdog.c \
 *       src/renderer/layout.c src/renderer/micro_renderer.c src/renderer/font_inter.c \
 *       src/drivers/device_profiles.c src/drivers/profiles/profile_tank_skid.c profile_drive.c profile_generic.c ...
 *   ./render_snapshot 800 480 out_prefix      -> out_prefix_0.bmp .. out_prefix_5.bmp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/core/tags.h"
#include "../include/core/alarms.h"
#include "../include/core/security.h"
#include "../include/core/watchdog.h"
#include "../include/drivers/device_profile.h"
#include "../include/drivers/device_info.h"
#include "../include/renderer/micro_renderer.h"

static hmi_renderer_t g_r;
static uint8_t g_row[3 * HMI_DISPLAY_MAX_WIDTH + 4];

/* The whole "display driver": convert one band to 24-bit BGR rows and append them. */
static void put_band(FILE *f, long row_bytes, int out_w, int y0, int rows, int total_h)
{
    const uint32_t *pal = hmi_renderer_get_palette(&g_r);
    const uint8_t *band = hmi_renderer_get_band_buffer(&g_r);
    for (int ry = 0; ry < rows; ry++) {
        const uint8_t *src = band + (size_t)ry * (size_t)(g_r.screen_w / 2);
        for (int x = 0; x < out_w; x++) {
            uint8_t idx = (uint8_t)((x & 1) ? (src[x >> 1] & 0x0F) : (src[x >> 1] >> 4));
            g_row[3 * x + 0] = (uint8_t)(pal[idx] & 0xFF);
            g_row[3 * x + 1] = (uint8_t)((pal[idx] >> 8) & 0xFF);
            g_row[3 * x + 2] = (uint8_t)((pal[idx] >> 16) & 0xFF);
        }
        /* BMP rows are stored bottom-up */
        long pos = 54L + (long)(total_h - 1 - (y0 + ry)) * row_bytes;
        fseek(f, pos, SEEK_SET);
        fwrite(g_row, 1, (size_t)row_bytes, f);
    }
}

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int write_screen(const char *path, int w, int h, const hmi_view_t *v)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    long row_bytes = ((long)w * 3 + 3) & ~3L;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    le32(hdr + 2, (uint32_t)(54 + row_bytes * h));
    le32(hdr + 10, 54);
    le32(hdr + 14, 40);
    le32(hdr + 18, (uint32_t)w);
    le32(hdr + 22, (uint32_t)h);
    hdr[26] = 1; hdr[28] = 24;
    le32(hdr + 34, (uint32_t)(row_bytes * h));
    fwrite(hdr, 1, sizeof(hdr), f);
    memset(g_row, 0, sizeof(g_row));
    for (int b = 0; b < g_r.band_count; b++) {
        hmi_renderer_render_band(&g_r, b, v);
        int rows = g_r.band_h;
        if (g_r.current_band_y + rows > h) rows = h - g_r.current_band_y;
        if (rows > 0) put_band(f, row_bytes, w, g_r.current_band_y, rows, h);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 800;
    int h = argc > 2 ? atoi(argv[2]) : 480;
    const char *prefix = argc > 3 ? argv[3] : "snapshot";
    if (w < 64 || h < 64 || w > HMI_DISPLAY_MAX_WIDTH || h > HMI_DISPLAY_MAX_HEIGHT) { fprintf(stderr, "bad size\n"); return 64; }

    static hmi_tag_table_t tags;
    static hmi_alarm_supervisor_t alarms;
    static hmi_security_ctx_t sec;
    static hmi_watchdog_t wd;

    /* Two plugged-in devices, bound exactly as the device manager binds them */
    static hmi_device_info_t devs[2];
    static hmi_tag_meta_t slot_meta[2][HMI_DEVICE_TAG_SPAN];
    const hmi_device_profile_t *prof[2] = { &g_profile_tank_skid, &g_profile_drive };
    static const char * const label[2] = { "Tank skid 1", "Drive 1" };
    static const char * const product[2] = { "BMX P34 2020", "ATV320U15N4C" };
    hmi_tag_registry_reset();
    hmi_tags_init(&tags);
    for (uint8_t d = 0; d < 2; d++) {
        devs[d].state = HMI_DEV_ONLINE;
        devs[d].slot = d;
        devs[d].transport = HMI_TRANSPORT_TCP;
        devs[d].profile = prof[d];
        snprintf(devs[d].label, sizeof(devs[d].label), "%s", label[d]);
        snprintf(devs[d].endpoint, sizeof(devs[d].endpoint), "192.168.1.%u:502", 50u + d);
        snprintf(devs[d].vendor, sizeof(devs[d].vendor), "Schneider Electric");
        snprintf(devs[d].product, sizeof(devs[d].product), "%s", product[d]);
        for (uint8_t i = 0; i < prof[d]->meta_count; i++) {
            uint8_t rel = prof[d]->meta[i].tag_id;
            slot_meta[d][rel] = prof[d]->meta[i];
            slot_meta[d][rel].tag_id = HMI_DEV_TAG(d, rel);
            slot_meta[d][rel].group = devs[d].label;
            hmi_tag_bind_meta(HMI_DEV_TAG(d, rel), &slot_meta[d][rel]);
            hmi_tag_write_analog(&tags, HMI_DEV_TAG(d, rel), 0, QUALITY_GOOD);
        }
    }
    /* A representative plant state: tank in its H band, pump running, drive at 50 Hz */
    static const int16_t tank[8] = { 872, 1450, 25, 141, 57, 1, 0, 0 };
    static const int16_t drive[6] = { 500, 170, 500, 50, 1, 0 };
    for (uint8_t i = 0; i < 8; i++) hmi_tag_write_analog(&tags, HMI_DEV_TAG(0, i), tank[i], QUALITY_GOOD);
    for (uint8_t i = 0; i < 6; i++) hmi_tag_write_analog(&tags, HMI_DEV_TAG(1, i), drive[i], QUALITY_GOOD);
    for (int i = 0; i < 60; i++) {
        hmi_tags_push_trend(&tags, 0, (int16_t)(780 + i * 3 / 2 + (i % 7) - 3));
        hmi_tags_push_trend(&tags, 1, (int16_t)(480 + (i % 9)));
    }
    hmi_alarms_init(&alarms);
    hmi_alarms_log_event(&alarms, 20, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, 0, "HMI started: plug-and-play");
    hmi_alarms_log_event(&alarms, 3100, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, 0, "Tank skid 1 found at 192.168.1.50:502");
    hmi_alarms_log_event(&alarms, 6400, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, 0, "Drive 1 found at 192.168.1.51:502");
    hmi_alarms_evaluate(&alarms, &tags, 31000);
    hmi_security_init(&sec);
    hmi_security_login(&sec, ROLE_OPERATOR, "1111", 0);
    hmi_watchdog_init(&wd, 0);
    hmi_discovery_stats_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.sweeps = 9; ds.targets = 254; ds.found = 2; ds.last_sweep_ms = 940;

    hmi_renderer_init(&g_r, w, h);
    int rc = 0;
    for (uint8_t s = 0; s <= SCREEN_COUNT; s++) {
        hmi_view_t v;
        memset(&v, 0, sizeof(v));
        v.tags = &tags; v.alarms = &alarms; v.sec = &sec; v.wd = &wd; v.disc = &ds;
        v.devices = devs; v.ndev = 2;
        v.active_screen = (uint8_t)(s == SCREEN_COUNT ? SCREEN_PROCESS_OVERVIEW : s);
        if (s == SCREEN_COUNT) v.ndev = 1;          /* extra frame: a single device gets the full layout */
        v.ui_wd_alive = true; v.now_ms = 31000; v.ui_frame_hz = 30;
        char path[512];
        snprintf(path, sizeof(path), "%s_%u.bmp", prefix, (unsigned)s);
        rc |= write_screen(path, w, h, &v);
        printf("wrote %s\n", path);
    }
    return rc;
}
