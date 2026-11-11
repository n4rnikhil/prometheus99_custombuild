/**
 * @file render_bench.c
 * @brief Render benchmark: draws whole frames of one screen and reports how long they take
 *
 * No window and no network: K devices are bound exactly as the device manager binds them, with
 * steady values, and the chosen screen is rendered N times into the band buffer (each band is
 * then read once, as a display driver would). Used for docs/benchmarks/.
 *
 *   render_bench <width> <height> <devices> <screen> <frames>
 *     screen: 0 Process, 1 Readings, 2 Alarms, 3 Diagnostics
 *   prints: frames, avg / min / max microseconds per frame, renderer RAM
 *
 * Build (Windows): see docs/benchmarks/README.md
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/tags.h"
#include "core/alarms.h"
#include "core/security.h"
#include "core/watchdog.h"
#include "devices/device_profile.h"
#include "fieldbus/device_info.h"
#include "renderer/micro_renderer.h"
#include "hal/timer.h"

static hmi_renderer_t   g_r;
static hmi_tag_table_t  g_tags;
static hmi_alarm_supervisor_t g_alarms;
static hmi_device_info_t g_devs[HMI_MAX_DEVICES];
static hmi_tag_meta_t   g_meta[HMI_MAX_DEVICES][HMI_DEVICE_TAG_SPAN];
static hmi_read_row_t   g_rows[64];
static volatile uint32_t g_sink;                 /* keeps the "display" reads from being optimised away */

static void present_band(void)
{
    const uint8_t *b = hmi_renderer_get_band_buffer(&g_r);
    uint32_t acc = 0;
    size_t n = (size_t)g_r.band_h * (size_t)(g_r.screen_w / 2);
    for (size_t i = 0; i < n; i += 64) acc += b[i];
    g_sink += acc;
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 800;
    int h = argc > 2 ? atoi(argv[2]) : 480;
    int ndev = argc > 3 ? atoi(argv[3]) : 12;
    int screen = argc > 4 ? atoi(argv[4]) : 0;
    int frames = argc > 5 ? atoi(argv[5]) : 300;
    if (ndev < 0 || ndev > HMI_MAX_DEVICES || frames < 1) { fprintf(stderr, "bad arguments\n"); return 64; }

    hmi_timer_init();
    hmi_tag_registry_reset();
    hmi_tags_init(&g_tags);
    int nprof = 0;
    const hmi_device_profile_t *prof[32];
    for (uint8_t i = 0; i < hmi_profile_count() && nprof < 32; i++)
        if (hmi_profile_at(i)->auto_attach) prof[nprof++] = hmi_profile_at(i);
    for (int d = 0; d < ndev; d++) {
        const hmi_device_profile_t *p = prof[d % nprof];
        hmi_device_info_t *dv = &g_devs[d];
        dv->state = HMI_DEV_ONLINE;
        dv->slot = (uint8_t)d;
        dv->transport = HMI_TRANSPORT_TCP;
        dv->profile = p;
        snprintf(dv->label, sizeof(dv->label), "%.18s %d", p->name, d / nprof + 1);
        snprintf(dv->endpoint, sizeof(dv->endpoint), "10.0.%d.%d:502", d / 200, d % 200 + 10);
        for (uint8_t i = 0; i < p->meta_count; i++) {
            uint8_t rel = (uint8_t)p->meta[i].tag_id;
            if (rel >= HMI_DEVICE_TAG_SPAN) continue;
            g_meta[d][rel] = p->meta[i];
            g_meta[d][rel].tag_id = HMI_DEV_TAG(d, rel);
            g_meta[d][rel].group = dv->label;
            hmi_tag_bind_meta(HMI_DEV_TAG(d, rel), &g_meta[d][rel]);
            int16_t v = (int16_t)(p->meta[i].min_val + (p->meta[i].max_val - p->meta[i].min_val) / 3);
            hmi_tag_write_analog(&g_tags, HMI_DEV_TAG(d, rel), p->meta[i].type == TAG_TYPE_DIGITAL ? 0 : v, QUALITY_GOOD);
        }
        /* a full 60 s trend (a slow ramp with a small wobble), so the tile charts are drawn */
        int16_t lo = p->meta_count ? p->meta[0].min_val : 0, hi = p->meta_count ? p->meta[0].max_val : 100;
        for (int s = 0; s < HMI_TREND_SAMPLES; s++)
            hmi_tags_push_trend(&g_tags, (uint8_t)d, (int16_t)(lo + (hi - lo) / 3 + (s % 7) * (hi - lo) / 200 + s * (hi - lo) / 600));
    }
    hmi_alarms_init(&g_alarms);
    hmi_alarms_evaluate(&g_alarms, &g_tags, 1000);
    static hmi_tag_id_t order[HMI_MAX_TAGS];
    uint16_t nord = hmi_alarms_sorted(&g_alarms, order, HMI_MAX_TAGS);
    static hmi_security_ctx_t sec;
    static hmi_watchdog_t wd;
    hmi_security_init(&sec);
    hmi_watchdog_init(&wd, 0);

    /* Readings: one page of rows, the first device's readings */
    uint16_t nrows = 0;
    if (ndev > 0) {
        g_rows[nrows++] = (hmi_read_row_t){ HMI_READ_ROW_DEVICE, 0, 0 };
        for (uint8_t rel = 0; rel < HMI_DEVICE_TAG_SPAN && nrows < 12; rel++)
            if (hmi_tag_get_meta(HMI_DEV_TAG(0, rel))) g_rows[nrows++] = (hmi_read_row_t){ HMI_READ_ROW_TAG, 0, HMI_DEV_TAG(0, rel) };
    }

    hmi_renderer_init(&g_r, w, h);
    hmi_view_t v;
    memset(&v, 0, sizeof(v));
    v.tags = &g_tags; v.alarms = &g_alarms; v.sec = &sec; v.wd = &wd;
    v.devices = g_devs; v.ndev = (uint8_t)ndev;
    v.alarm_order = order; v.alarm_order_n = nord;
    v.active_screen = (uint8_t)(screen == 1 ? SCREEN_READINGS : screen == 2 ? SCREEN_ALARM_MATRIX
                                : screen == 3 ? SCREEN_FIELD_DIAGNOSTICS : SCREEN_PROCESS_OVERVIEW);
    v.read_rows = g_rows; v.read_nrows = nrows; v.read_pages = 1; v.read_matches = nrows; v.read_total = nrows;
    v.read_query = "";
    v.ui_wd_alive = true; v.now_ms = 31000; v.ui_frame_hz = 30;

    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    for (int f = 0; f < frames + 10; f++) {
        uint64_t t0 = hmi_timer_get_us();
        for (int b = 0; b < g_r.band_count; b++) {
            hmi_renderer_render_band(&g_r, b, &v);
            present_band();
        }
        uint64_t dt = hmi_timer_get_us() - t0;
        if (f < 10) continue;                       /* warm-up */
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }
    printf("screen=%d size=%dx%d devices=%d frames=%d avg_us=%llu min_us=%llu max_us=%llu renderer_bytes=%u band_buffer=%u bands=%d\n",
           screen, w, h, ndev, frames, (unsigned long long)(total / (uint64_t)frames), (unsigned long long)mn,
           (unsigned long long)mx, (unsigned)sizeof(hmi_renderer_t), (unsigned)HMI_DISPLAY_MAX_BUF_SIZE, g_r.band_count);
    return (int)(g_sink & 0u);
}
