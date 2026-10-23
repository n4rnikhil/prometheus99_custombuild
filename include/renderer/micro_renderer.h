/**
 * @file micro_renderer.h
 * @brief Declarative Micro-Rasterizer & Elastic L1-Cache Band Buffer
 * @standards Apple HIG Aesthetics / ISA-101 Situational Awareness
 *
 * Zero heap allocations. Constant 18.75 KB L1-cache RAM footprint regardless of screen size.
 */

#ifndef INDUSTRIAL_HMI_MICRO_RENDERER_H
#define INDUSTRIAL_HMI_MICRO_RENDERER_H

#include "../config.h"
#include "../core/tags.h"
#include "../core/alarms.h"
#include "../core/security.h"
#include "../core/watchdog.h"
#include "bytecode.h"

typedef struct {
    int screen_w;
    int screen_h;
    int band_h;
    int band_count;
    int current_band_y;
    bool high_contrast;
    uint8_t buffer[HMI_DISPLAY_MAX_BUF_SIZE];
} hmi_renderer_t;

/* --- Core Renderer Lifecycle --- */
void hmi_renderer_init(hmi_renderer_t *rend, int screen_w, int screen_h);
void hmi_renderer_resize(hmi_renderer_t *rend, int screen_w, int screen_h);
void hmi_renderer_set_theme(hmi_renderer_t *rend, bool high_contrast);

/* --- Rasterization Primitives (Clipped to active band) --- */
void hmi_draw_rect(hmi_renderer_t *r, int x, int y, int w, int h, uint8_t color);
void hmi_draw_border_rect(hmi_renderer_t *r, int x, int y, int w, int h, uint8_t fill_col, uint8_t border_col, int border_w);
void hmi_draw_text(hmi_renderer_t *r, int x, int y, const char *str, uint8_t color, uint8_t scale);

/* --- Apple-Inspired High-Performance Industrial Widgets --- */
void hmi_draw_seven_seg_number(hmi_renderer_t *r, int x, int y, int16_t value, uint8_t decimals, int digit_h, uint8_t color);
void hmi_draw_analog_bar(hmi_renderer_t *r, int x, int y, int w, int h, int16_t cur_val, int16_t min_val, int16_t max_val, int16_t warn_val, int16_t trip_val, uint8_t color);
void hmi_draw_status_pill(hmi_renderer_t *r, int x, int y, int w, int h, const char *text, uint8_t status_color);
void hmi_draw_action_banner(hmi_renderer_t *r, int x, int y, int w, int h, const hmi_emergency_action_t *act);

/* --- Full Screen Band-Buffer Dispatch --- */
void hmi_renderer_render_band(hmi_renderer_t *r, int band_idx,
                              const uint32_t *screen_tokens, uint8_t token_count,
                              const hmi_tag_table_t *tags,
                              const hmi_alarm_supervisor_t *alarms,
                              const hmi_security_ctx_t *sec,
                              const hmi_watchdog_t *wd,
                              uint8_t active_screen_id);

const uint8_t* hmi_renderer_get_band_buffer(const hmi_renderer_t *r);
const uint32_t* hmi_renderer_get_palette(const hmi_renderer_t *r);

#endif /* INDUSTRIAL_HMI_MICRO_RENDERER_H */
