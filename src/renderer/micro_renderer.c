/**
 * @file micro_renderer.c
 * @brief Declarative Micro-Rasterizer Implementation
 * @standards Apple HIG Aesthetics & ISA-101 Situational Awareness
 */

#include "../../include/renderer/micro_renderer.h"
#include "../../include/renderer/clut16.h"
#include "../../include/renderer/font5x7.h"
#include <string.h>
#include <stdio.h>

/* --- Procedural 7-Segment Lookup (0 bytes of font tables!) ---
 * Segment bit mapping:
 *   Bit 0: A (Top)
 *   Bit 1: B (Top-Right)
 *   Bit 2: C (Bottom-Right)
 *   Bit 3: D (Bottom)
 *   Bit 4: E (Bottom-Left)
 *   Bit 5: F (Top-Left)
 *   Bit 6: G (Middle)
 */
static const uint8_t s_7seg_digits[10] = {
    0x3F, /* 0: A,B,C,D,E,F   */
    0x06, /* 1: B,C           */
    0x5B, /* 2: A,B,D,E,G     */
    0x4F, /* 3: A,B,C,D,G     */
    0x66, /* 4: B,C,F,G       */
    0x6D, /* 5: A,C,D,F,G     */
    0x7D, /* 6: A,C,D,E,F,G   */
    0x07, /* 7: A,B,C         */
    0x7F, /* 8: A,B,C,D,E,F,G */
    0x6F  /* 9: A,B,C,D,F,G   */
};

void hmi_renderer_init(hmi_renderer_t *rend, int screen_w, int screen_h)
{
    if (!rend) return;
    memset(rend, 0, sizeof(hmi_renderer_t));
    hmi_renderer_resize(rend, screen_w, screen_h);
}

void hmi_renderer_resize(hmi_renderer_t *rend, int screen_w, int screen_h)
{
    if (!rend) return;
    rend->screen_w = (screen_w > 0) ? screen_w : HMI_DISPLAY_WIDTH_DEFAULT;
    rend->screen_h = (screen_h > 0) ? screen_h : HMI_DISPLAY_HEIGHT_DEFAULT;

    int row_bytes = rend->screen_w / 2; /* 4-bit = 2 pixels per byte */
    if (row_bytes <= 0) row_bytes = 400;

    /* Elastic band height: fills up to 19,200 bytes L1 cache */
    rend->band_h = HMI_DISPLAY_MAX_BUF_SIZE / row_bytes;
    if (rend->band_h > rend->screen_h) rend->band_h = rend->screen_h;
    if (rend->band_h < 4) rend->band_h = 4;

    rend->band_count = (rend->screen_h + rend->band_h - 1) / rend->band_h;
}

void hmi_renderer_set_theme(hmi_renderer_t *rend, bool high_contrast)
{
    if (rend) rend->high_contrast = high_contrast;
}

/* --- Fast 4-bit Pixel Put with Band Clipping --- */
static inline void put_pixel(hmi_renderer_t *r, int x, int y, uint8_t color)
{
    if (x < 0 || x >= r->screen_w) return;
    if (y < r->current_band_y || y >= r->current_band_y + r->band_h) return;

    int local_y = y - r->current_band_y;
    int row_bytes = r->screen_w / 2;
    size_t byte_idx = (size_t)local_y * row_bytes + ((size_t)x >> 1);

    uint8_t c = (uint8_t)(color & 0x0F);
    if (x & 1) {
        /* Odd pixel: low nibble */
        r->buffer[byte_idx] = (uint8_t)((r->buffer[byte_idx] & 0xF0) | c);
    } else {
        /* Even pixel: high nibble */
        r->buffer[byte_idx] = (uint8_t)((r->buffer[byte_idx] & 0x0F) | (c << 4));
    }
}

void hmi_draw_rect(hmi_renderer_t *r, int x, int y, int w, int h, uint8_t color)
{
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (x2 > r->screen_w) x2 = r->screen_w;
    if (x >= x2) return;

    /* Clip to active band [current_band_y, current_band_y + band_h) */
    int band_end = r->current_band_y + r->band_h;
    int cy1 = (y < r->current_band_y) ? r->current_band_y : y;
    int cy2 = (y2 > band_end) ? band_end : y2;
    if (cy1 >= cy2) return;

    uint8_t c4 = (uint8_t)(color & 0x0F);
    uint8_t double_c = (uint8_t)((c4 << 4) | c4);
    int local_y1 = cy1 - r->current_band_y;
    int local_y2 = cy2 - r->current_band_y;
    int row_bytes = r->screen_w / 2;

    /* Full scanline optimization */
    if (x == 0 && (x2 - x) == r->screen_w) {
        size_t start = (size_t)local_y1 * row_bytes;
        size_t total = (size_t)(local_y2 - local_y1) * row_bytes;
        memset(&r->buffer[start], double_c, total);
        return;
    }

    for (int py = local_y1; py < local_y2; py++) {
        int px = x;
        size_t row_start = (size_t)py * row_bytes;

        /* Leading odd pixel */
        if (px & 1) {
            size_t b_idx = row_start + ((size_t)px >> 1);
            r->buffer[b_idx] = (uint8_t)((r->buffer[b_idx] & 0xF0) | c4);
            px++;
        }

        /* Middle paired bytes */
        int pairs = (x2 - px) / 2;
        if (pairs > 0) {
            size_t b_idx = row_start + ((size_t)px >> 1);
            memset(&r->buffer[b_idx], double_c, (size_t)pairs);
            px += pairs * 2;
        }

        /* Trailing even pixel */
        if (px < x2) {
            size_t b_idx = row_start + ((size_t)px >> 1);
            r->buffer[b_idx] = (uint8_t)((r->buffer[b_idx] & 0x0F) | (c4 << 4));
        }
    }
}

void hmi_draw_border_rect(hmi_renderer_t *r, int x, int y, int w, int h,
                          uint8_t fill_col, uint8_t border_col, int border_w)
{
    hmi_draw_rect(r, x, y, w, h, border_col);
    hmi_draw_rect(r, x + border_w, y + border_w, w - 2 * border_w, h - 2 * border_w, fill_col);
}

void hmi_draw_text(hmi_renderer_t *r, int x, int y, const char *str, uint8_t color, uint8_t scale)
{
    if (!str || scale == 0) return;
    int cx = x;

    while (*str) {
        char c = *str++;
        if (c == '\n') {
            cx = x;
            y += (FONT5X7_HEIGHT + 2) * scale;
            continue;
        }
        const uint8_t *glyph = hmi_font_get_glyph(c);
        for (int col = 0; col < FONT5X7_WIDTH; col++) {
            uint8_t line = glyph[col];
            for (int row = 0; row < FONT5X7_HEIGHT; row++) {
                if (line & (1 << row)) {
                    int px = cx + col * scale;
                    int py = y + row * scale;
                    if (scale == 1) {
                        put_pixel(r, px, py, color);
                    } else {
                        hmi_draw_rect(r, px, py, scale, scale, color);
                    }
                }
            }
        }
        cx += (FONT5X7_WIDTH + 1) * scale;
    }
}

/* --- Procedural 7-Segment Big Bold Digits (0 Bytes of Font Tables!) --- */
static void draw_single_7seg(hmi_renderer_t *r, int x, int y, char ch, int h, uint8_t color)
{
    if (ch < '0' || ch > '9') return;
    uint8_t mask = s_7seg_digits[ch - '0'];

    int w = (h * 6) / 10;
    int thick = h / 7;
    if (thick < 2) thick = 2;
    int mid_y = y + h / 2 - thick / 2;

    /* A: Top horizontal */
    if (mask & 0x01) hmi_draw_rect(r, x + thick, y, w - 2 * thick, thick, color);
    /* B: Top-right vertical */
    if (mask & 0x02) hmi_draw_rect(r, x + w - thick, y + thick, thick, (h / 2) - thick, color);
    /* C: Bottom-right vertical */
    if (mask & 0x04) hmi_draw_rect(r, x + w - thick, mid_y + thick, thick, (h / 2) - thick, color);
    /* D: Bottom horizontal */
    if (mask & 0x08) hmi_draw_rect(r, x + thick, y + h - thick, w - 2 * thick, thick, color);
    /* E: Bottom-left vertical */
    if (mask & 0x10) hmi_draw_rect(r, x, mid_y + thick, thick, (h / 2) - thick, color);
    /* F: Top-left vertical */
    if (mask & 0x20) hmi_draw_rect(r, x, y + thick, thick, (h / 2) - thick, color);
    /* G: Middle horizontal */
    if (mask & 0x40) hmi_draw_rect(r, x + thick, mid_y, w - 2 * thick, thick, color);
}

void hmi_draw_seven_seg_number(hmi_renderer_t *r, int x, int y, int16_t value,
                               uint8_t decimals, int digit_h, uint8_t color)
{
    char buf[16];
    if (decimals == 1) {
        snprintf(buf, sizeof(buf), "%d.%d", value / 10, (value >= 0 ? value : -value) % 10);
    } else if (decimals == 2) {
        snprintf(buf, sizeof(buf), "%d.%02d", value / 100, (value >= 0 ? value : -value) % 100);
    } else {
        snprintf(buf, sizeof(buf), "%d", value);
    }

    int digit_w = (digit_h * 6) / 10;
    int spacing = digit_w / 4;
    int cx = x;

    for (int i = 0; buf[i]; i++) {
        if (buf[i] == '.') {
            int dot_sz = digit_h / 6;
            if (dot_sz < 2) dot_sz = 2;
            hmi_draw_rect(r, cx, y + digit_h - dot_sz, dot_sz, dot_sz, color);
            cx += dot_sz + spacing;
        } else if (buf[i] == '-') {
            int thick = digit_h / 7;
            hmi_draw_rect(r, cx, y + digit_h / 2 - thick / 2, digit_w, thick, color);
            cx += digit_w + spacing;
        } else {
            draw_single_7seg(r, cx, y, buf[i], digit_h, color);
            cx += digit_w + spacing;
        }
    }
}

/* --- ISA-101 High Performance Limit Bar --- */
void hmi_draw_analog_bar(hmi_renderer_t *r, int x, int y, int w, int h,
                         int16_t cur_val, int16_t min_val, int16_t max_val,
                         int16_t warn_val, int16_t trip_val, uint8_t color)
{
    if (w <= 0 || h <= 0 || max_val <= min_val) return;

    /* Inactive background track */
    hmi_draw_rect(r, x, y, w, h, COLOR_DARK_GRAY);

    /* Compute filled fraction */
    int fill_w = ((cur_val - min_val) * w) / (max_val - min_val);
    if (fill_w < 0) fill_w = 0;
    if (fill_w > w) fill_w = w;

    uint8_t bar_col = color;
    if (trip_val > min_val && cur_val >= trip_val) bar_col = COLOR_CRIMSON_TRIP;
    else if (warn_val > min_val && cur_val >= warn_val) bar_col = COLOR_AMBER_WARN;

    if (fill_w > 0) {
        hmi_draw_rect(r, x, y, fill_w, h, bar_col);
    }

    /* Warning limit tick mark */
    if (warn_val > min_val && warn_val < max_val) {
        int wx = x + ((warn_val - min_val) * w) / (max_val - min_val);
        hmi_draw_rect(r, wx - 1, y - 2, 2, h + 4, COLOR_AMBER_WARN);
    }

    /* Trip limit tick mark */
    if (trip_val > min_val && trip_val < max_val) {
        int tx = x + ((trip_val - min_val) * w) / (max_val - min_val);
        hmi_draw_rect(r, tx - 1, y - 2, 2, h + 4, COLOR_CRIMSON_TRIP);
    }
}

/* --- Apple-Style Rounded Status Pill Badge --- */
void hmi_draw_status_pill(hmi_renderer_t *r, int x, int y, int w, int h,
                          const char *text, uint8_t status_color)
{
    /* Card capsule */
    hmi_draw_border_rect(r, x, y, w, h, COLOR_TITANIUM_CARD, status_color, 1);

    /* Solid indicator dot */
    int dot_sz = h / 3;
    if (dot_sz < 4) dot_sz = 4;
    int dot_y = y + (h - dot_sz) / 2;
    hmi_draw_rect(r, x + 8, dot_y, dot_sz, dot_sz, status_color);

    /* Text */
    if (text) {
        hmi_draw_text(r, x + 8 + dot_sz + 6, y + (h - 7) / 2, text, COLOR_CHALK_WHITE, 1);
    }
}

/* --- Calm Action-Oriented Emergency Banner (Apple HIG / Zero Panic!) --- */
void hmi_draw_action_banner(hmi_renderer_t *r, int x, int y, int w, int h,
                            const hmi_emergency_action_t *act)
{
    if (!act || !act->active) return;

    /* Elevated crimson caution card */
    hmi_draw_border_rect(r, x, y, w, h, COLOR_TITANIUM_CARD, COLOR_CRIMSON_TRIP, 2);

    /* Warning headline */
    hmi_draw_text(r, x + 16, y + 10, act->title, COLOR_CRIMSON_TRIP, 1);

    /* Clear step-by-step guidance so operator doesn't panic */
    hmi_draw_text(r, x + 16, y + 26, act->action_step1, COLOR_CHALK_WHITE, 1);
    hmi_draw_text(r, x + 16, y + 40, act->action_step2, COLOR_SILVER_MUTED, 1);

    /* Prominent Apple-style action button */
    int btn_w = 260;
    int btn_h = 36;
    int btn_x = x + w - btn_w - 16;
    int btn_y = y + (h - btn_h) / 2;
    hmi_draw_border_rect(r, btn_x, btn_y, btn_w, btn_h, COLOR_CRIMSON_TRIP, COLOR_WHITE_PURE, 1);
    hmi_draw_text(r, btn_x + 18, btn_y + 14, "[ ACKNOWLEDGE & SILENCE ]", COLOR_WHITE_PURE, 1);
}

/* --- Master Band Render Dispatch with Bounding-Box Culling --- */
void hmi_renderer_render_band(hmi_renderer_t *r, int band_idx,
                              const uint32_t *screen_tokens, uint8_t token_count,
                              const hmi_tag_table_t *tags,
                              const hmi_alarm_supervisor_t *alarms,
                              const hmi_security_ctx_t *sec,
                              const hmi_watchdog_t *wd,
                              uint8_t active_screen_id)
{
    if (!r) return;
    r->current_band_y = band_idx * r->band_h;

    /* 1. Clear band to Obsidian background */
    uint8_t bg_col = COLOR_OBSIDIAN_BG;
    uint8_t double_bg = (uint8_t)((bg_col << 4) | bg_col);
    int row_bytes = r->screen_w / 2;
    memset(r->buffer, double_bg, (size_t)r->band_h * row_bytes);

    /* 2. Top Header Bar (Y: 0..42 px) */
    if (r->current_band_y < 46) {
        hmi_draw_rect(r, 0, 0, r->screen_w, 42, COLOR_TITANIUM_CARD);
        hmi_draw_rect(r, 0, 42, r->screen_w, 1, COLOR_CARD_BORDER);

        /* Apple-style Station branding */
        hmi_draw_text(r, 16, 17, "SCHNEIDER M580 HMI", COLOR_CHALK_WHITE, 1);

        /* System Health / Watchdog Pill */
        if (wd && wd->system_healthy) {
            hmi_draw_status_pill(r, 220, 10, 140, 24, "NOMINAL", COLOR_EMERALD_OK);
        } else {
            hmi_draw_status_pill(r, 220, 10, 140, 24, "WD FAULT", COLOR_CRIMSON_TRIP);
        }

        /* User Role Badge */
        const char *role_str = (sec && sec->current_role == ROLE_ENGINEER) ? "ENGINEER" :
                               (sec && sec->current_role == ROLE_MAINTENANCE) ? "MAINT" :
                               (sec && sec->current_role == ROLE_OPERATOR) ? "OPERATOR" : "VIEWER";
        hmi_draw_status_pill(r, 380, 10, 120, 24, role_str, COLOR_PURPLE_SECURE);

        /* Alarm count indicator */
        if (alarms && alarms->highest_severity >= ALARM_SEV_WARNING) {
            hmi_draw_status_pill(r, 520, 10, 140, 24, "ALARM ACTIVE", COLOR_CRIMSON_TRIP);
        } else {
            hmi_draw_status_pill(r, 520, 10, 140, 24, "NO ALARMS", COLOR_EMERALD_OK);
        }

        /* High-contrast indicator */
        hmi_draw_text(r, r->screen_w - 110, 17, "[C] THEME", COLOR_SILVER_MUTED, 1);
    }

    /* 3. Emergency Action Banner (Y: 48..110 px if active) */
    int content_top_y = 52;
    if (alarms && alarms->current_action.active) {
        hmi_draw_action_banner(r, 16, 48, r->screen_w - 32, 60, &alarms->current_action);
        content_top_y = 116;
    }

    /* 4. Declarative Widget Token Dispatch with Bounding-Box Culling */
    if (screen_tokens && tags) {
        for (uint8_t i = 0; i < token_count; i++) {
            uint32_t tok = screen_tokens[i];
            uint8_t tag_id = hmi_token_get_tag(tok);
            uint8_t type   = hmi_token_get_type(tok);

            /* Convert Permille Coordinates to Pixels */
            int px = HMI_NORM_TO_PIXEL(hmi_token_get_norm_x(tok), r->screen_w);
            int py = HMI_NORM_TO_PIXEL(hmi_token_get_norm_y(tok), r->screen_h);
            int pw = HMI_NORM_TO_PIXEL(hmi_token_get_norm_w(tok), r->screen_w);
            int ph = HMI_NORM_TO_PIXEL(hmi_token_get_norm_h(tok), r->screen_h);

            /* Shift down if Emergency Banner is active */
            if (alarms && alarms->current_action.active && py < 400) {
                py += 20;
            }

            /* BOUNDING-BOX CULLING: Skip widget if outside this band! */
            if ((py + ph) <= r->current_band_y || py >= (r->current_band_y + r->band_h)) {
                continue; /* Zero wasted rasterization cycles! */
            }

            const hmi_tag_meta_t *meta = hmi_tag_get_meta(tag_id);
            int16_t val = tags->tags[tag_id].value;
            uint8_t quality = tags->tags[tag_id].quality;

            switch (type) {
                case WIDGET_CONTAINER_CARD:
                    hmi_draw_border_rect(r, px, py, pw, ph, COLOR_TITANIUM_CARD, COLOR_CARD_BORDER, 1);
                    if (meta && meta->name) {
                        hmi_draw_text(r, px + 12, py + 12, meta->name, COLOR_SILVER_MUTED, 1);
                    }
                    break;

                case WIDGET_HERO_DIGIT:
                    if (quality == QUALITY_GOOD || quality == QUALITY_SIMULATED) {
                        hmi_draw_seven_seg_number(r, px, py, val, meta ? 1 : 0, ph, COLOR_CHALK_WHITE);
                    } else {
                        hmi_draw_text(r, px, py + ph / 2, "---", COLOR_AMBER_WARN, 2);
                    }
                    if (meta && meta->unit) {
                        hmi_draw_text(r, px + pw - 30, py + ph - 16, meta->unit, COLOR_CYAN_ACCENT, 1);
                    }
                    break;

                case WIDGET_ANALOG_BAR:
                    if (meta) {
                        hmi_draw_analog_bar(r, px, py, pw, ph, val, meta->min_val, meta->max_val,
                                            meta->warn_high, meta->trip_high, COLOR_CYAN_ACCENT);
                    }
                    break;

                case WIDGET_STATUS_PILL:
                    if (val > 0) {
                        hmi_draw_status_pill(r, px, py, pw, ph, meta ? meta->name : "ON", COLOR_EMERALD_OK);
                    } else {
                        hmi_draw_status_pill(r, px, py, pw, ph, "STOPPED", COLOR_DARK_GRAY);
                    }
                    break;

                case WIDGET_ACTION_BUTTON:
                    hmi_draw_border_rect(r, px, py, pw, ph, COLOR_SURFACE_HOVER, COLOR_CARD_BORDER, 1);
                    if (meta && meta->name) {
                        hmi_draw_text(r, px + 12, py + (ph - 7) / 2, meta->name, COLOR_CHALK_WHITE, 1);
                    }
                    break;

                default:
                    break;
            }
        }
    }

    /* 5. Bottom Navigation Dock (Y: ScreenHeight - 44 px) */
    int dock_y = r->screen_h - 44;
    if ((dock_y + 44) > r->current_band_y && dock_y < (r->current_band_y + r->band_h)) {
        hmi_draw_rect(r, 0, dock_y, r->screen_w, 44, COLOR_TITANIUM_CARD);
        hmi_draw_rect(r, 0, dock_y, r->screen_w, 1, COLOR_CARD_BORDER);

        static const char * const nav_tabs[5] = {
            "[1] PROCESS", "[2] DIAGS", "[3] TELEMETRY", "[4] ALARMS", "[5] SETTINGS"
        };

        int tab_w = (r->screen_w - 32) / 5;
        for (int t = 0; t < 5; t++) {
            int tx = 16 + t * tab_w;
            bool is_active = (t == active_screen_id);
            uint8_t pill_bg = is_active ? COLOR_CYAN_ACCENT : COLOR_TITANIUM_CARD;
            uint8_t text_col = is_active ? COLOR_OBSIDIAN_BG : COLOR_SILVER_MUTED;

            hmi_draw_border_rect(r, tx + 4, dock_y + 6, tab_w - 8, 32, pill_bg,
                                 is_active ? COLOR_CHALK_WHITE : COLOR_CARD_BORDER, 1);
            hmi_draw_text(r, tx + 14, dock_y + 18, nav_tabs[t], text_col, 1);
        }
    }
}

const uint8_t* hmi_renderer_get_band_buffer(const hmi_renderer_t *r)
{
    return r ? r->buffer : NULL;
}

const uint32_t* hmi_renderer_get_palette(const hmi_renderer_t *r)
{
    if (r && r->high_contrast) {
        return g_hmi_palette_contrast;
    }
    return g_hmi_palette_dark;
}
