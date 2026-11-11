/*
 * LVGL side of the render comparison: an 800 x 480 dashboard like the HMI's Process screen with
 * 12 devices (header with title, three status pills and an EMERGENCY button; 12 tiles of name,
 * status dot, caption, run state and value; a 6-tab dock), redrawn in full every frame.
 *   lvgl_bench <draw buffer bytes> <frames>
 * Prints: avg / min / max microseconds per full frame, LVGL heap use, draw buffer size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "lvgl.h"

static LARGE_INTEGER s_freq, s_t0;
static uint64_t now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart - s_t0.QuadPart) * 1000000 / s_freq.QuadPart);
}
static uint32_t tick_ms(void) { return (uint32_t)(now_us() / 1000); }

static volatile uint32_t g_sink;
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    uint32_t n = (uint32_t)lv_area_get_size(a) * 2u, acc = 0;
    for (uint32_t i = 0; i < n; i += 64) acc += px[i];      /* read like a display driver would */
    g_sink += acc;
    lv_display_flush_ready(d);
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t c, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int radius, lv_color_t bg, lv_color_t border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, border, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}

int main(int argc, char **argv)
{
    uint32_t buf_bytes = argc > 1 ? (uint32_t)atoi(argv[1]) : 76800u;
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_t0);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(800, 480);
    static uint8_t buf[800 * 480 * 2];
    if (buf_bytes > sizeof(buf)) buf_bytes = sizeof(buf);
    lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_color_t bg = lv_color_hex(0x0E1014), card = lv_color_hex(0x1A1D23), line = lv_color_hex(0x2C3038);
    lv_color_t text = lv_color_hex(0xF2F3F5), muted = lv_color_hex(0x9AA0A8), ok = lv_color_hex(0x30C060);
    lv_color_t red = lv_color_hex(0xF04438);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, bg, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* header */
    box(scr, 0, 0, 800, 56, 0, card, line);
    label(scr, "Prometheus HMI", &lv_font_montserrat_20, text, 56, 8);
    label(scr, "12 devices online", &lv_font_montserrat_14, muted, 56, 32);
    static const char * const pills[3] = { "HMI OK", "No alarms", "Viewer" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *p = box(scr, 336 + i * 116, 8, 108, 40, 20, lv_color_hex(0x23262D), line);
        lv_obj_t *d = box(p, 12, 16, 8, 8, 4, ok, ok);
        (void)d;
        label(p, pills[i], &lv_font_montserrat_14, text, 26, 10);
    }
    lv_obj_t *e = box(scr, 680, 8, 104, 40, 10, red, red);
    label(e, "EMERGENCY", &lv_font_montserrat_14, lv_color_white(), 8, 10);

    /* 12 tiles, 4 x 3 */
    static const char * const names[4] = { "Tank skid", "Altivar drive", "Conveyor", "Boiler" };
    static const char * const caps[4] = { "Tank 01 level", "Output frequency", "Belt speed", "Water temp" };
    static const char * const vals[4] = { "72.9 %", "30.0 Hz", "30.0 m/min", "79.4 C" };
    char t[40];
    for (int k = 0; k < 12; k++) {
        int col = k % 4, row = k / 4;
        lv_obj_t *tile = box(scr, 16 + col * 195, 68 + row * 100, 183, 88, 12, card, line);
        box(tile, 12, 15, 8, 8, 4, ok, ok);
        snprintf(t, sizeof(t), "%s %d", names[k % 4], k / 4 + 1);
        label(tile, t, &lv_font_montserrat_14, text, 26, 8);
        label(tile, caps[k % 4], &lv_font_montserrat_14, muted, 12, 34);
        box(tile, 104, 40, 8, 8, 4, ok, ok);
        label(tile, "Running", &lv_font_montserrat_14, text, 116, 34);
        label(tile, vals[k % 4], &lv_font_montserrat_20, text, 12, 56);
    }

    /* dock */
    box(scr, 0, 420, 800, 60, 0, card, line);
    static const char * const tabs[6] = { "Process", "Readings", "Alarms", "Diagnostics", "Telemetry", "Settings" };
    for (int i = 0; i < 6; i++) label(scr, tabs[i], &lv_font_montserrat_14, i ? muted : text, 20 + i * 133, 452);

    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    for (int f = 0; f < frames + 10; f++) {
        lv_obj_invalidate(scr);
        uint64_t t0 = now_us();
        lv_refr_now(disp);
        uint64_t dt = now_us() - t0;
        if (f < 10) continue;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("lvgl %d.%d.%d size=800x480 frames=%d avg_us=%llu min_us=%llu max_us=%llu draw_buffer=%u heap_max_used=%u heap_size=%u\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, frames,
           (unsigned long long)(total / (uint64_t)frames), (unsigned long long)mn, (unsigned long long)mx,
           (unsigned)buf_bytes, (unsigned)mon.max_used, (unsigned)mon.total_size);
    return (int)(g_sink & 0u);
}
