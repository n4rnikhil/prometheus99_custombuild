/**
 * @file micro_renderer.c
 * @brief Band-buffer rasteriser, anti-aliased primitives and the five HMI screens
 * @standards ISA-101 (grey graphics, colour only for abnormal state) / ISO 9241 touch targets
 *
 * Portable C99. No OS calls, no heap, integer-only pixel maths.
 * Every primitive clips to the active band AND to the clip rect, so a frame
 * is produced band by band from one 19,200-byte buffer.
 */

#include "../../include/renderer/micro_renderer.h"
#include "../../include/renderer/clut16.h"
#include "../../include/drivers/driver_modbus.h"
#include <string.h>
#include <stdio.h>

/* Short colour names */
#define C_BG      COLOR_OBSIDIAN_BG
#define C_CARD    COLOR_TITANIUM_CARD
#define C_LINE    COLOR_CARD_BORDER
#define C_TEXT    COLOR_CHALK_WHITE
#define C_MUTED   COLOR_SILVER_MUTED
#define C_TRACK   COLOR_DARK_GRAY
#define C_TEAL    COLOR_CYAN_ACCENT
#define C_BLUE    COLOR_BLUE_ACCENT
#define C_GREEN   COLOR_EMERALD_OK
#define C_ORANGE  COLOR_AMBER_WARN
#define C_RED     COLOR_CRIMSON_TRIP
#define C_YELLOW  COLOR_VIBRANT_ORANGE
#define C_PURPLE  COLOR_PURPLE_SECURE
#define C_SURF    COLOR_SURFACE_HOVER
#define C_DIM     COLOR_DIM_TEXT
#define C_WHITE   COLOR_WHITE_PURE

/* ========================================================================= */
/* Lifecycle                                                                  */
/* ========================================================================= */

static const uint32_t* palette_of(const hmi_renderer_t *r)
{
    return (r && r->high_contrast) ? g_hmi_palette_contrast : g_hmi_palette_dark;
}

static void build_blend_lut(hmi_renderer_t *r)
{
    const uint32_t *pal = palette_of(r);
    for (int fg = 0; fg < 16; fg++) {
        for (int bg = 0; bg < 16; bg++) {
            r->blend[fg][bg][0] = (uint8_t)bg;
            r->blend[fg][bg][3] = (uint8_t)fg;
            for (int a = 1; a <= 2; a++) {
                uint32_t wa = (uint32_t)a, wb = 3u - wa;
                int tr = (int)((((pal[fg] >> 16) & 0xFFu) * wa + ((pal[bg] >> 16) & 0xFFu) * wb) / 3u);
                int tg = (int)((((pal[fg] >> 8) & 0xFFu) * wa + ((pal[bg] >> 8) & 0xFFu) * wb) / 3u);
                int tb = (int)(((pal[fg] & 0xFFu) * wa + (pal[bg] & 0xFFu) * wb) / 3u);
                int best = bg;
                long best_d = 0x7FFFFFFFL;
                for (int k = 0; k < 16; k++) {
                    int dr = (int)((pal[k] >> 16) & 0xFF) - tr;
                    int dg = (int)((pal[k] >> 8) & 0xFF) - tg;
                    int db = (int)(pal[k] & 0xFF) - tb;
                    long d = 3L * dr * dr + 4L * dg * dg + 2L * db * db;   /* rough perceptual weights */
                    if (d < best_d) { best_d = d; best = k; }
                }
                r->blend[fg][bg][a] = (uint8_t)best;
            }
        }
    }
}

void hmi_renderer_init(hmi_renderer_t *rend, int screen_w, int screen_h)
{
    if (!rend) return;
    memset(rend, 0, sizeof(*rend));
    hmi_renderer_resize(rend, screen_w, screen_h);
    build_blend_lut(rend);
}

void hmi_renderer_resize(hmi_renderer_t *rend, int screen_w, int screen_h)
{
    if (!rend) return;
    if (screen_w < 8) screen_w = 8;
    if (screen_w > HMI_DISPLAY_MAX_WIDTH) screen_w = HMI_DISPLAY_MAX_WIDTH;
    screen_w = (screen_w + 7) & ~7;              /* 4-bpp rows DWORD aligned */
    if (screen_h < 1) screen_h = 1;
    if (screen_h > HMI_DISPLAY_MAX_HEIGHT) screen_h = HMI_DISPLAY_MAX_HEIGHT;

    rend->screen_w = screen_w;
    rend->screen_h = screen_h;
    int row_bytes = screen_w / 2;
    rend->band_h = HMI_DISPLAY_MAX_BUF_SIZE / row_bytes;   /* >= 5 for 7680 px */
    if (rend->band_h > screen_h) rend->band_h = screen_h;
    if (rend->band_h < 1) rend->band_h = 1;
    rend->band_count = (screen_h + rend->band_h - 1) / rend->band_h;
    rend->current_band_y = 0;
    rend->clip_x0 = 0; rend->clip_y0 = 0;
    rend->clip_x1 = screen_w; rend->clip_y1 = screen_h;
}

void hmi_renderer_set_theme(hmi_renderer_t *rend, bool high_contrast)
{
    if (!rend) return;
    rend->high_contrast = high_contrast;
    build_blend_lut(rend);
}

const uint8_t* hmi_renderer_get_band_buffer(const hmi_renderer_t *r) { return r ? r->buffer : NULL; }
const uint32_t* hmi_renderer_get_palette(const hmi_renderer_t *r)    { return palette_of(r); }

/* ========================================================================= */
/* Clipping & pixels                                                          */
/* ========================================================================= */

typedef struct { int x0, y0, x1, y1; } span_t;

/* Visible region = clip rect AND active band */
static span_t vis(const hmi_renderer_t *r)
{
    span_t s;
    s.x0 = r->clip_x0 < 0 ? 0 : r->clip_x0;
    s.x1 = r->clip_x1 > r->screen_w ? r->screen_w : r->clip_x1;
    int by0 = r->current_band_y, by1 = r->current_band_y + r->band_h;
    s.y0 = r->clip_y0 > by0 ? r->clip_y0 : by0;
    s.y1 = r->clip_y1 < by1 ? r->clip_y1 : by1;
    return s;
}

static void set_clip(hmi_renderer_t *r, hmi_rect_t c)
{
    r->clip_x0 = c.x; r->clip_y0 = c.y; r->clip_x1 = c.x + c.w; r->clip_y1 = c.y + c.h;
}

static void reset_clip(hmi_renderer_t *r)
{
    r->clip_x0 = 0; r->clip_y0 = 0; r->clip_x1 = r->screen_w; r->clip_y1 = r->screen_h;
}

static bool band_hits(const hmi_renderer_t *r, int y, int h)
{
    return (y + h) > r->current_band_y && y < r->current_band_y + r->band_h;
}

static inline uint8_t *px_ptr(hmi_renderer_t *r, int x, int y)
{
    return &r->buffer[(size_t)(y - r->current_band_y) * (size_t)(r->screen_w / 2) + (size_t)(x >> 1)];
}

static inline uint8_t px_get(hmi_renderer_t *r, int x, int y)
{
    uint8_t b = *px_ptr(r, x, y);
    return (uint8_t)((x & 1) ? (b & 0x0F) : (b >> 4));
}

static inline void px_set(hmi_renderer_t *r, int x, int y, uint8_t c)
{
    uint8_t *p = px_ptr(r, x, y);
    c &= 0x0F;
    *p = (x & 1) ? (uint8_t)((*p & 0xF0) | c) : (uint8_t)((*p & 0x0F) | (c << 4));
}

/* Blend colour c at coverage a (0..3) into pixel (x, y), with clipping. */
static inline void px_blend(hmi_renderer_t *r, const span_t *s, int x, int y, uint8_t c, int a)
{
    if (a <= 0 || x < s->x0 || x >= s->x1 || y < s->y0 || y >= s->y1) return;
    if (a >= 3) { px_set(r, x, y, c); return; }
    px_set(r, x, y, r->blend[c & 15][px_get(r, x, y)][a]);
}

/* ========================================================================= */
/* Primitives                                                                 */
/* ========================================================================= */

void hmi_draw_rect(hmi_renderer_t *r, int x, int y, int w, int h, uint8_t color)
{
    if (!r || w <= 0 || h <= 0) return;
    span_t s = vis(r);
    int x0 = x < s.x0 ? s.x0 : x, x1 = (x + w) > s.x1 ? s.x1 : x + w;
    int y0 = y < s.y0 ? s.y0 : y, y1 = (y + h) > s.y1 ? s.y1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    uint8_t c4 = (uint8_t)(color & 0x0F);
    uint8_t dbl = (uint8_t)((c4 << 4) | c4);
    for (int py = y0; py < y1; py++) {
        int px = x0;
        if (px & 1) { px_set(r, px, py, c4); px++; }
        int pairs = (x1 - px) / 2;
        if (pairs > 0) {
            memset(px_ptr(r, px, py), dbl, (size_t)pairs);
            px += pairs * 2;
        }
        if (px < x1) px_set(r, px, py, c4);
    }
}

/* Coverage (0..3) of a pixel whose centre is (dx2/2, dy2/2) pixels from a circle
 * centre, for a circle of radius rad. Integer only. */
static int disc_cov(int dx2, int dy2, int rad)
{
    long d = 9L * ((long)dx2 * dx2 + (long)dy2 * dy2);   /* (6 * dist)^2 */
    long r6 = 6L * rad;
    if (d <= (r6 - 2) * (r6 - 2)) return 3;
    if (d <= r6 * r6) return 2;
    if (d <= (r6 + 2) * (r6 + 2)) return 1;
    return 0;
}

void hmi_draw_round_rect(hmi_renderer_t *r, int x, int y, int w, int h, int rad, uint8_t color)
{
    if (!r || w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad <= 0) { hmi_draw_rect(r, x, y, w, h, color); return; }
    span_t s = vis(r);
    if (y + h <= s.y0 || y >= s.y1) return;

    /* Straight middle part */
    hmi_draw_rect(r, x, y + rad, w, h - 2 * rad, color);
    /* Top and bottom strips between corners */
    hmi_draw_rect(r, x + rad, y, w - 2 * rad, rad, color);
    hmi_draw_rect(r, x + rad, y + h - rad, w - 2 * rad, rad, color);

    /* Four anti-aliased corners */
    for (int py = 0; py < rad; py++) {
        int dy2 = 2 * py + 1 - 2 * rad;
        int yt = y + py, yb = y + h - 1 - py;
        bool top = yt >= s.y0 && yt < s.y1, bot = yb >= s.y0 && yb < s.y1;
        if (!top && !bot) continue;
        for (int px = 0; px < rad; px++) {
            int a = disc_cov(2 * px + 1 - 2 * rad, dy2, rad);
            if (!a) continue;
            if (top) { px_blend(r, &s, x + px, yt, color, a); px_blend(r, &s, x + w - 1 - px, yt, color, a); }
            if (bot) { px_blend(r, &s, x + px, yb, color, a); px_blend(r, &s, x + w - 1 - px, yb, color, a); }
        }
    }
}

void hmi_draw_round_frame(hmi_renderer_t *r, int x, int y, int w, int h, int rad, uint8_t fill, uint8_t border)
{
    hmi_draw_round_rect(r, x, y, w, h, rad, border);
    hmi_draw_round_rect(r, x + 1, y + 1, w - 2, h - 2, rad > 1 ? rad - 1 : 0, fill);
}

void hmi_draw_circle(hmi_renderer_t *r, int cx, int cy, int rad, uint8_t color)
{
    if (!r || rad <= 0) return;
    span_t s = vis(r);
    if (cy + rad < s.y0 || cy - rad - 1 >= s.y1) return;
    for (int py = cy - rad - 1; py <= cy + rad; py++) {
        if (py < s.y0 || py >= s.y1) continue;
        for (int px = cx - rad - 1; px <= cx + rad; px++) {
            /* circle centred on the corner between pixels (cx, cy) */
            int a = disc_cov(2 * (px - cx) + 1, 2 * (py - cy) + 1, rad);
            px_blend(r, &s, px, py, color, a);
        }
    }
}

void hmi_draw_line(hmi_renderer_t *r, int x0, int y0, int x1, int y1, int t, uint8_t color)
{
    if (!r || t <= 0) return;
    span_t s = vis(r);
    int pad = t / 2 + 2;
    int bx0 = (x0 < x1 ? x0 : x1) - pad, bx1 = (x0 > x1 ? x0 : x1) + pad;
    int by0 = (y0 < y1 ? y0 : y1) - pad, by1 = (y0 > y1 ? y0 : y1) + pad;
    if (by1 < s.y0 || by0 >= s.y1) return;
    if (by0 < s.y0) by0 = s.y0;
    if (by1 >= s.y1) by1 = s.y1 - 1;

    /* Doubled coordinates: endpoints on pixel centres */
    long vx = 2L * (x1 - x0), vy = 2L * (y1 - y0);
    long vv = vx * vx + vy * vy;
    long t3 = 3L * t;
    long lo = (t3 - 2) * (t3 - 2), mid = t3 * t3, hi = (t3 + 2) * (t3 + 2);

    for (int py = by0; py <= by1; py++) {
        for (int px = bx0; px <= bx1; px++) {
            long wx = 2L * (px - x0), wy = 2L * (py - y0);
            long dot = wx * vx + wy * vy;
            long long d;   /* (6 * distance)^2 * vv (or without vv at the caps) */
            int a;
            if (vv == 0 || dot <= 0) {
                d = 9LL * (wx * wx + wy * wy);
                a = d <= lo ? 3 : d <= mid ? 2 : d <= hi ? 1 : 0;
            } else if (dot >= vv) {
                long ex = wx - vx, ey = wy - vy;
                d = 9LL * (ex * ex + ey * ey);
                a = d <= lo ? 3 : d <= mid ? 2 : d <= hi ? 1 : 0;
            } else {
                long long cr = (long long)wx * vy - (long long)wy * vx;
                d = 9LL * cr * cr;
                a = d <= lo * (long long)vv ? 3 : d <= mid * (long long)vv ? 2 : d <= hi * (long long)vv ? 1 : 0;
            }
            px_blend(r, &s, px, py, color, a);
        }
    }
}

int hmi_text_width(const hmi_font_t *f, const char *str)
{
    if (!f || !str) return 0;
    int w = 0;
    for (; *str; str++) {
        const hmi_glyph_t *g = hmi_font_glyph(f, *str);
        w += g ? g->adv : f->line_h / 3;
    }
    return w;
}

int hmi_draw_text(hmi_renderer_t *r, const hmi_font_t *f, int x, int y, const char *str, uint8_t color)
{
    if (!r || !f || !str) return x;
    span_t s = vis(r);
    bool visible = (y + f->line_h > s.y0) && (y < s.y1);
    int pen = x;
    for (; *str; str++) {
        const hmi_glyph_t *g = hmi_font_glyph(f, *str);
        if (!g) { pen += f->line_h / 3; continue; }
        if (visible && g->w && g->h) {
            int gx = pen + g->xoff, gy = y + g->yoff;
            int stride = (g->w + 3) / 4;
            int r0 = gy < s.y0 ? s.y0 - gy : 0;
            int r1 = (gy + g->h) > s.y1 ? s.y1 - gy : g->h;
            for (int row = r0; row < r1; row++) {
                const uint8_t *bits = f->bits + g->off + (size_t)row * (size_t)stride;
                for (int col = 0; col < g->w; col++) {
                    int a = (bits[col >> 2] >> (6 - 2 * (col & 3))) & 3;
                    if (a) px_blend(r, &s, gx + col, gy + row, color, a);
                }
            }
        }
        pen += g->adv;
    }
    return pen;
}

/* ========================================================================= */
/* Text helpers                                                               */
/* ========================================================================= */

static void text_right(hmi_renderer_t *r, const hmi_font_t *f, int right, int y, const char *s, uint8_t c)
{
    hmi_draw_text(r, f, right - hmi_text_width(f, s), y, s, c);
}

static void text_center(hmi_renderer_t *r, const hmi_font_t *f, hmi_rect_t b, const char *s, uint8_t c)
{
    hmi_draw_text(r, f, b.x + (b.w - hmi_text_width(f, s)) / 2, b.y + (b.h - f->line_h) / 2, s, c);
}

/* Draw at most max_w pixels of text; long strings end in "..." */
static void text_fit(hmi_renderer_t *r, const hmi_font_t *f, int x, int y, int max_w, const char *s, uint8_t c)
{
    if (hmi_text_width(f, s) <= max_w) { hmi_draw_text(r, f, x, y, s, c); return; }
    char buf[96];
    int dots = hmi_text_width(f, "...");
    size_t n = 0;
    int w = 0;
    for (const char *p = s; *p != 0 && n + 4 < sizeof(buf); p++) {
        const hmi_glyph_t *g = hmi_font_glyph(f, *p);
        int adv = g ? g->adv : f->line_h / 3;
        if (w + adv + dots > max_w) break;
        buf[n++] = *p;
        w += adv;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = '\0';
    hmi_draw_text(r, f, x, y, buf, c);
}

/* Sentence-case an ALL CAPS string for display ("TANK 01 LEVEL" -> "Tank 01 level").
 * Tokens with digits/dashes or of <= 3 letters look like codes (V-101, PLC, HH) and are kept. */
static void nice_name(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    bool first_word = true;
    if (!in) in = "";
    for (const char *p = in; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {          /* already mixed case: keep as written */
            for (; in[n] && n + 1 < cap; n++) out[n] = in[n];
            out[n] = '\0';
            return;
        }
    }
    while (*in && n + 1 < cap) {
        const char *w = in;
        size_t len = 0;
        bool code = false;
        while (w[len] && w[len] != ' ') {
            if ((w[len] >= '0' && w[len] <= '9') || w[len] == '-') code = true;
            len++;
        }
        if (len <= 3) code = true;
        for (size_t i = 0; i < len && n + 1 < cap; i++) {
            char ch = w[i];
            bool keep = code || (first_word && i == 0);
            if (!keep && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            out[n++] = ch;
        }
        if (len) first_word = false;
        in = w + len;
        while (*in == ' ' && n + 1 < cap) { out[n++] = ' '; in++; }
    }
    out[n] = '\0';
}

/* ========================================================================= */
/* Data helpers                                                               */
/* ========================================================================= */

static bool q_ok(uint8_t q) { return q == QUALITY_GOOD || q == QUALITY_SIMULATED; }

static uint8_t sev_color(uint8_t sev)
{
    if (sev >= ALARM_SEV_CRITICAL) return C_RED;
    if (sev == ALARM_SEV_WARNING) return C_ORANGE;
    if (sev == ALARM_SEV_INFO) return C_BLUE;
    return C_GREEN;
}

static const char* sev_label(uint8_t sev)
{
    switch (sev) {
        case ALARM_SEV_ESTOP:    return "E-Stop";
        case ALARM_SEV_CRITICAL: return "Critical";
        case ALARM_SEV_WARNING:  return "Warning";
        case ALARM_SEV_INFO:     return "Info";
        default:                 return "Normal";
    }
}

static const char* state_label(uint8_t st)
{
    switch (st) {
        case ALARM_PT_UNACK:     return "Unacknowledged";
        case ALARM_PT_ACKED:     return "Acknowledged";
        case ALARM_PT_RTN_UNACK: return "Cleared, unack.";
        default:                 return "Normal";
    }
}

static uint8_t point_sev(const hmi_view_t *v, uint8_t tag)
{
    if (!v->alarms || tag >= HMI_MAX_TAGS) return ALARM_SEV_NONE;
    return v->alarms->points[tag].severity;
}

static bool tag_value(const hmi_view_t *v, uint8_t tag, int16_t *val, uint8_t *q)
{
    if (!v->tags || tag >= HMI_MAX_TAGS) { *val = 0; *q = QUALITY_COMM_FAULT; return false; }
    *val = v->tags->tags[tag].value;
    *q = v->tags->tags[tag].quality;
    return q_ok(*q);
}

static void fmt_value(char *out, size_t cap, const hmi_view_t *v, uint8_t tag)
{
    int16_t val; uint8_t q;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!tag_value(v, tag, &val, &q)) { snprintf(out, cap, "--"); return; }
    hmi_format_scaled(out, cap, val, m ? m->decimals : 0);
}

static void fmt_uptime(char *out, size_t cap, uint32_t ms)
{
    uint32_t s = ms / 1000u;
    snprintf(out, cap, "%02lu:%02lu:%02lu.%03lu", (unsigned long)(s / 3600u), (unsigned long)((s / 60u) % 60u),
             (unsigned long)(s % 60u), (unsigned long)(ms % 1000u));
}

static bool role_allows(const hmi_view_t *v, hmi_command_t cmd)
{
    return v->sec && v->sec->current_role >= hmi_security_required_role(cmd);
}

/* ========================================================================= */
/* Small components                                                           */
/* ========================================================================= */

static void chip(hmi_renderer_t *r, hmi_rect_t b, uint8_t dot, const char *text, uint8_t text_col)
{
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, b.h / 2, C_SURF, C_LINE);
    int cy = b.y + b.h / 2;
    if (b.w <= b.h + 4) {                     /* compact: status dot only */
        hmi_draw_circle(r, b.x + b.w / 2, cy, 5, dot);
        return;
    }
    hmi_draw_circle(r, b.x + 15, cy, 4, dot);
    text_fit(r, &hmi_font_body_b, b.x + 26, b.y + (b.h - hmi_font_body_b.line_h) / 2, b.w - 34, text, text_col);
}

typedef enum { BTN_PRIMARY, BTN_PLAIN, BTN_DANGER, BTN_DISABLED, BTN_FILLED_RED } btn_style_t;

static void button(hmi_renderer_t *r, hmi_rect_t b, const char *label, btn_style_t st)
{
    if (!band_hits(r, b.y, b.h)) return;
    uint8_t fill = C_SURF, border = C_LINE, text = C_BLUE;
    switch (st) {
        case BTN_PRIMARY:    fill = C_BLUE; border = C_BLUE; text = C_WHITE; break;
        case BTN_FILLED_RED: fill = C_RED;  border = C_RED;  text = C_WHITE; break;
        case BTN_DANGER:     text = C_RED; break;
        case BTN_DISABLED:   text = C_DIM; break;
        case BTN_PLAIN:      default: break;
    }
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 10, fill, border);
    /* Label: body font, or caption font when the button is narrow, else truncated */
    const hmi_font_t *f = &hmi_font_body_b;
    if (hmi_text_width(f, label) > b.w - 16) f = &hmi_font_caption;
    if (hmi_text_width(f, label) > b.w - 12) {
        text_fit(r, f, b.x + 6, b.y + (b.h - f->line_h) / 2, b.w - 12, label, text);
    } else {
        text_center(r, f, b, label, text);
    }
}

static void kv_row(hmi_renderer_t *r, int x, int y, int w, const char *k, const char *val, uint8_t vc)
{
    hmi_draw_text(r, &hmi_font_body, x, y, k, C_MUTED);
    int kw = hmi_text_width(&hmi_font_body, k) + 12;
    int vw = hmi_text_width(&hmi_font_body_b, val);
    if (vw > w - kw) { text_fit(r, &hmi_font_body_b, x + kw, y, w - kw, val, vc); return; }
    text_right(r, &hmi_font_body_b, x + w, y, val, vc);
}

static void card(hmi_renderer_t *r, hmi_rect_t b, const char *title)
{
    if (!band_hits(r, b.y, b.h)) return;
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 14, C_CARD, C_LINE);
    if (title) text_fit(r, &hmi_font_title, b.x + 16, b.y + 12, b.w - 32, title, C_TEXT);
}

static void separator(hmi_renderer_t *r, int x, int y, int w)
{
    hmi_draw_rect(r, x, y, w, 1, C_LINE);
}

/* 32-step sine table, Q8 (256 = 1.0) */
static const int16_t s_sin32[32] = {
    0, 50, 98, 142, 181, 213, 237, 251, 256, 251, 237, 213, 181, 142, 98, 50,
    0, -50, -98, -142, -181, -213, -237, -251, -256, -251, -237, -213, -181, -142, -98, -50
};
static int isin(int i) { return s_sin32[i & 31]; }
static int icos(int i) { return s_sin32[(i + 8) & 31]; }

/* ========================================================================= */
/* Manifest widgets                                                           */
/* ========================================================================= */

static void w_card(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    char name[48];
    nice_name(name, sizeof(name), m ? m->name : "Tag");
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 14, C_CARD, C_LINE);

    /* Status at the top right: dot + word */
    int16_t val; uint8_t q;
    const char *st = "Normal";
    uint8_t col = C_GREEN;
    if (v->data_stale) { st = "Stale"; col = C_DIM; }
    else if (!tag_value(v, tag, &val, &q)) { st = "No data"; col = C_RED; }
    else {
        uint8_t sev = point_sev(v, tag);
        if (sev >= ALARM_SEV_CRITICAL) { st = "High-high"; col = C_RED; }
        else if (sev == ALARM_SEV_WARNING) { st = "High"; col = C_ORANGE; }
        else if (q == QUALITY_SIMULATED) { st = "Simulated"; col = C_TEAL; }
    }
    /* Narrow card: the title wins, the status shrinks to its dot */
    int sw = hmi_text_width(&hmi_font_caption, st);
    int tw = hmi_text_width(&hmi_font_title, name);
    bool room = (16 + tw + 20 + 9 + sw + 16) <= b.w;
    int sx = room ? b.x + b.w - 16 - sw : b.x + b.w - 12;
    if (room) hmi_draw_text(r, &hmi_font_caption, sx, b.y + 15, st, C_MUTED);
    hmi_draw_circle(r, sx - 9, b.y + 15 + 7, room ? 3 : 4, col);
    text_fit(r, &hmi_font_title, b.x + 16, b.y + 12, sx - 20 - (b.x + 16), name, C_TEXT);
}

static const hmi_font_t * const s_hero_fonts[3] = { &hmi_font_hero_l, &hmi_font_hero_m, &hmi_font_hero_s };

/* Largest hero font (0 = large .. 2 = small) whose line fits the rect height and
 * whose value + unit fits the width. Width is checked against the widest digits
 * the tag can show, so the size does not jump as the value changes. */
static int hero_fit(hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    char val[16], widest[16];
    fmt_value(val, sizeof(val), v, tag);
    /* widest realistic reading: one count below full scale (tank 99.9 %, pump 3599 RPM) */
    hmi_format_scaled(widest, sizeof(widest), (int16_t)(m && m->max_val > 0 ? m->max_val - 1 : 0), m ? m->decimals : 0);
    const char *probe = (strlen(widest) > strlen(val)) ? widest : val;
    const char *unit = (m && m->unit) ? m->unit : "";
    int uw = unit[0] ? hmi_text_width(&hmi_font_title, unit) + 6 : 0;
    for (int i = 0; i < 3; i++) {
        if (s_hero_fonts[i]->line_h <= b.h + 8 && hmi_text_width(s_hero_fonts[i], probe) + uw <= b.w) return i;
    }
    return 2;
}

static void w_hero(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag, int font_idx)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    char val[16];
    fmt_value(val, sizeof(val), v, tag);
    const char *unit = (m && m->unit) ? m->unit : "";
    const hmi_font_t *f = s_hero_fonts[font_idx < 0 ? 0 : font_idx > 2 ? 2 : font_idx];

    int16_t raw; uint8_t q;
    uint8_t col = C_TEXT;
    if (v->data_stale || !tag_value(v, tag, &raw, &q)) col = C_DIM;
    else if (point_sev(v, tag) >= ALARM_SEV_CRITICAL) col = C_RED;
    else if (point_sev(v, tag) == ALARM_SEV_WARNING) col = C_ORANGE;

    int y = b.y + (b.h - f->line_h) / 2;
    int end = hmi_draw_text(r, f, b.x, y, val, col);
    if (unit[0]) hmi_draw_text(r, &hmi_font_title, end + 6, y + f->ascent - hmi_font_title.ascent, unit, C_MUTED);
}

static void w_bar(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!m) return;
    char name[48], val[24], txt[40];
    nice_name(name, sizeof(name), m->name);
    fmt_value(val, sizeof(val), v, tag);
    snprintf(txt, sizeof(txt), "%s %s", val, m->unit ? m->unit : "");

    int16_t raw; uint8_t q;
    bool ok = tag_value(v, tag, &raw, &q) && !v->data_stale;
    uint8_t sev = point_sev(v, tag);
    uint8_t vc = !ok ? C_DIM : sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_TEXT;

    hmi_draw_text(r, &hmi_font_body, b.x, b.y, name, C_MUTED);
    text_right(r, &hmi_font_body_b, b.x + b.w, b.y, txt, vc);

    int bh = 8;
    int by = b.y + hmi_font_body.line_h + 5;
    hmi_draw_round_rect(r, b.x, by, b.w, bh, bh / 2, C_TRACK);
    int span = m->max_val - m->min_val;
    if (span <= 0) return;
    if (ok) {
        long fw = ((long)(raw - m->min_val) * b.w) / span;
        if (fw < 0) fw = 0;
        if (fw > b.w) fw = b.w;
        if (fw >= 2) {
            uint8_t fc = sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_TEAL;
            hmi_draw_round_rect(r, b.x, by, (int)fw, bh, bh / 2, fc);
        }
    }
    if (m->warn_high > m->min_val && m->warn_high < m->max_val) {
        int tx = b.x + (int)(((long)(m->warn_high - m->min_val) * b.w) / span);
        hmi_draw_rect(r, tx - 1, by - 3, 2, bh + 6, C_ORANGE);
    }
    if (m->trip_high > m->min_val && m->trip_high < m->max_val) {
        int tx = b.x + (int)(((long)(m->trip_high - m->min_val) * b.w) / span);
        hmi_draw_rect(r, tx - 1, by - 3, 2, bh + 6, C_RED);
    }
}

static void w_pill(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    int16_t val; uint8_t q;
    const char *t; uint8_t dot;
    if (v->data_stale || !tag_value(v, tag, &val, &q)) { t = "Unknown"; dot = C_RED; }
    else if (val) { t = "Running"; dot = C_GREEN; }
    else { t = "Stopped"; dot = C_DIM; }
    int h = 30;
    hmi_rect_t p = { b.x, b.y + (b.h - h) / 2, hmi_text_width(&hmi_font_body_b, t) + 40, h };
    if (p.w > b.w) p.w = b.w;
    chip(r, p, dot, t, C_TEXT);
}

static void w_button(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    int16_t val; uint8_t q;
    bool ok = tag_value(v, tag, &val, &q) && !v->data_stale;
    char label[48];
    btn_style_t st = BTN_PLAIN;
    hmi_command_t cmd = HMI_CMD_PUMP_TOGGLE;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);

    if (tag == SCHNEIDER_TAG_PUMP_RUNNING) {
        bool estop = v->tags && v->tags->tags[SCHNEIDER_TAG_ESTOP_ACTIVE].value != 0;
        if (ok && val) { snprintf(label, sizeof(label), "Stop pump"); st = BTN_DANGER; }
        else { snprintf(label, sizeof(label), estop ? "Start blocked: E-stop" : "Start pump"); st = estop ? BTN_DISABLED : BTN_PRIMARY; }
    } else if (tag == SCHNEIDER_TAG_VALVE_POS) {
        cmd = HMI_CMD_VALVE_TOGGLE;
        snprintf(label, sizeof(label), "Move valve to %d %%", ok ? (int)hmi_valve_toggle_target(val) : 85);
    } else {
        nice_name(label, sizeof(label), m ? m->name : "Action");
    }
    if (!ok || !role_allows(v, cmd)) st = BTN_DISABLED;
    int h = b.h < HMI_LAYOUT_BUTTON_H ? b.h : HMI_LAYOUT_BUTTON_H;
    hmi_rect_t bb = { b.x, b.y + (b.h - h) / 2, b.w, h };
    button(r, bb, label, st);
}

static void w_trend(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!m || m->max_val <= m->min_val) return;
    hmi_draw_text(r, &hmi_font_caption, b.x, b.y, "LAST 60 SECONDS", C_MUTED);

    hmi_rect_t p = { b.x, b.y + 20, b.w, b.h - 20 };
    if (p.h < 12) return;
    for (int i = 0; i <= 2; i++) hmi_draw_rect(r, p.x, p.y + (p.h - 1) * i / 2, p.w, 1, C_LINE);

    int span = m->max_val - m->min_val;
    #define TREND_Y(val) (p.y + p.h - 2 - (int)(((long)((val) - m->min_val) * (p.h - 4)) / span))
    /* dashed limit lines */
    int16_t lim[2] = { m->warn_high, m->trip_high };
    uint8_t lc[2] = { C_ORANGE, C_RED };
    for (int k = 0; k < 2; k++) {
        if (lim[k] <= m->min_val || lim[k] >= m->max_val) continue;
        int ly = TREND_Y(lim[k]);
        for (int x = p.x; x < p.x + p.w; x += 8) hmi_draw_rect(r, x, ly, 4, 1, lc[k]);
    }

    int16_t samples[HMI_TREND_SAMPLES];
    uint8_t n = v->tags ? hmi_tags_copy_trend(v->tags, samples, HMI_TREND_SAMPLES) : 0;
    if (n < 2) {
        hmi_draw_text(r, &hmi_font_body, p.x + 4, p.y + p.h / 2 - 8, "Collecting data...", C_DIM);
        return;
    }
    int first = HMI_TREND_SAMPLES - n;     /* newest sample sits at the right edge */
    int px_prev = 0, py_prev = 0;
    bool have_prev = false;
    hmi_rect_t clip = { p.x - 2, p.y - 2, p.w + 4, p.h + 4 };
    set_clip(r, clip);
    for (int i = 0; i < n; i++) {
        int16_t sv = samples[i];
        if (sv == HMI_TREND_GAP) { have_prev = false; continue; }   /* no data: break the line */
        if (sv < m->min_val) sv = m->min_val;
        if (sv > m->max_val) sv = m->max_val;
        int x = p.x + ((first + i) * (p.w - 1)) / (HMI_TREND_SAMPLES - 1);
        int y = TREND_Y(sv);
        if (have_prev) hmi_draw_line(r, px_prev, py_prev, x, y, 2, v->data_stale ? C_DIM : C_TEAL);
        px_prev = x; py_prev = y;
        have_prev = true;
    }
    reset_clip(r);
    if (have_prev) hmi_draw_circle(r, px_prev, py_prev, 4, v->data_stale ? C_DIM : C_TEAL);
    #undef TREND_Y
}

static void w_tank(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!m || m->max_val <= m->min_val) return;
    int16_t val; uint8_t q;
    bool ok = tag_value(v, tag, &val, &q) && !v->data_stale;
    int span = m->max_val - m->min_val;
    int rad = b.w / 4;
    if (rad > 12) rad = 12;

    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, rad, C_SURF, C_LINE);
    hmi_rect_t in = hmi_rect_inset(b, 3, 3);
    if (ok) {
        long lv = val < m->min_val ? 0 : val > m->max_val ? span : val - m->min_val;
        int fh = (int)((lv * in.h) / span);
        uint8_t sev = point_sev(v, tag);
        uint8_t fc = sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_TEAL;
        hmi_rect_t clip = { in.x, in.y + in.h - fh, in.w, fh };
        set_clip(r, clip);
        hmi_draw_round_rect(r, in.x, in.y, in.w, in.h, rad - 3, fc);
        reset_clip(r);
    }
    int16_t lim[2] = { m->warn_high, m->trip_high };
    uint8_t lc[2] = { C_ORANGE, C_RED };
    for (int k = 0; k < 2; k++) {
        if (lim[k] <= m->min_val || lim[k] >= m->max_val) continue;
        int ly = in.y + in.h - (int)(((long)(lim[k] - m->min_val) * in.h) / span);
        hmi_draw_rect(r, b.x - 4, ly, b.w + 8, 1, lc[k]);
    }
}

static void w_pump(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, uint8_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    int16_t val; uint8_t q;
    bool ok = tag_value(v, tag, &val, &q) && !v->data_stale;
    bool run = ok && val != 0;
    int rad = (b.w < b.h ? b.w : b.h) / 2 - 2;
    if (rad < 8) return;
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    /* volute + outlet */
    hmi_draw_round_rect(r, cx, cy - rad, rad + 6, rad / 2, 3, C_LINE);
    hmi_draw_circle(r, cx, cy, rad, C_LINE);
    hmi_draw_circle(r, cx, cy, rad - 2, C_SURF);
    uint8_t col = !ok ? C_DIM : run ? C_GREEN : C_MUTED;
    int phase = run ? (int)((v->now_ms / 25u) & 31u) : 0;
    for (int k = 0; k < 3; k++) {
        int a = phase + k * 11;            /* ~120 degrees apart in 32 steps */
        int ex = cx + (icos(a) * (rad - 6)) / 256;
        int ey = cy + (isin(a) * (rad - 6)) / 256;
        hmi_draw_line(r, cx, cy, ex, ey, 4, col);
    }
    hmi_draw_circle(r, cx, cy, 5, col);
}

static void render_manifest(hmi_renderer_t *r, const hmi_view_t *v, const uint32_t *tok, uint8_t n, hmi_rect_t content)
{
    if (!tok) return;
    int hero_font = 0;                       /* one hero size per screen, set by the tightest widget */
    for (uint8_t i = 0; i < n; i++) {
        if (hmi_token_get_type(tok[i]) != WIDGET_HERO_DIGIT) continue;
        int f = hero_fit(hmi_layout_token_rect(tok[i], content), v, hmi_token_get_tag(tok[i]));
        if (f > hero_font) hero_font = f;
    }
    for (uint8_t i = 0; i < n; i++) {
        hmi_rect_t b = hmi_layout_token_rect(tok[i], content);
        if (!band_hits(r, b.y - 4, b.h + 8)) continue;    /* bounding-box culling per band */
        uint8_t tag = hmi_token_get_tag(tok[i]);
        if (tag >= HMI_MAX_TAGS) continue;
        switch (hmi_token_get_type(tok[i])) {
            case WIDGET_CONTAINER_CARD: w_card(r, b, v, tag);   break;
            case WIDGET_HERO_DIGIT:     w_hero(r, b, v, tag, hero_font); break;
            case WIDGET_ANALOG_BAR:     w_bar(r, b, v, tag);    break;
            case WIDGET_STATUS_PILL:    w_pill(r, b, v, tag);   break;
            case WIDGET_ACTION_BUTTON:  w_button(r, b, v, tag); break;
            case WIDGET_TREND_SPARK:    w_trend(r, b, v, tag);  break;
            case WIDGET_TANK:           w_tank(r, b, v, tag);   break;
            case WIDGET_PUMP:           w_pump(r, b, v, tag);   break;
            default: break;
        }
    }
}

/* ========================================================================= */
/* Chrome: header, banner, dock, toast                                        */
/* ========================================================================= */

static void render_header(hmi_renderer_t *r, const hmi_view_t *v)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_rect_t h = hmi_layout_header(sw, sh);
    if (!band_hits(r, h.y, h.h)) return;
    hmi_draw_rect(r, h.x, h.y, h.w, h.h, C_CARD);
    separator(r, 0, h.h - 1, sw);

    /* App mark */
    hmi_draw_round_rect(r, 16, 13, 30, 30, 9, C_BLUE);
    hmi_draw_rect(r, 23, 26, 4, 11, C_WHITE);
    hmi_draw_rect(r, 29, 21, 4, 16, C_WHITE);
    hmi_draw_rect(r, 35, 17, 4, 20, C_WHITE);

    hmi_rect_t theme = hmi_layout_theme_button(sw, sh);
    int avail = theme.x - 8 - 56;
    text_fit(r, &hmi_font_title, 56, 9, avail, "Prometheus HMI", C_TEXT);
    char sub[96];
    if (v->plc && v->plc->mode == HMI_DRV_MODE_MODBUS_TCP) snprintf(sub, sizeof(sub), "Modicon M580  -  %s", v->plc->endpoint);
    else snprintf(sub, sizeof(sub), "Modicon M580  -  Simulator");
    text_fit(r, &hmi_font_caption, 56, 30, avail, sub, C_MUTED);

    /* Theme toggle (moon) */
    int tcx = theme.x + theme.w / 2, tcy = theme.y + theme.h / 2;
    hmi_draw_circle(r, tcx, tcy, 16, C_LINE);
    hmi_draw_circle(r, tcx, tcy, 15, C_SURF);
    hmi_draw_circle(r, tcx, tcy, 7, C_MUTED);
    hmi_draw_circle(r, tcx + 4, tcy - 3, 6, C_SURF);

    /* System health */
    const char *ht = "System OK";
    uint8_t hc = C_GREEN;
    if (v->data_stale)                          { ht = "Data stale";     hc = C_RED; }
    else if (v->wd && !v->wd->fieldbus_alive)   { ht = "Watchdog trip";  hc = C_RED; }
    else if (!v->ui_wd_alive)                   { ht = "UI stalled";     hc = C_ORANGE; }
    else if (v->wd && v->wd->failover_active)   { ht = "Failover latch"; hc = C_ORANGE; }
    else if (v->plc && v->plc->link_state != HMI_LINK_UP) { ht = "PLC offline"; hc = C_RED; }
    chip(r, hmi_layout_health_pill(sw, sh), hc, ht, C_TEXT);

    /* Alarm summary */
    char at[32];
    uint8_t ac = C_GREEN;
    if (v->alarms && v->alarms->unacked_count) {
        snprintf(at, sizeof(at), "%u unack.", (unsigned)v->alarms->unacked_count);
        ac = sev_color(v->alarms->highest_severity);
    } else if (v->alarms && v->alarms->active_count) {
        snprintf(at, sizeof(at), "%u active", (unsigned)v->alarms->active_count);
        ac = sev_color(v->alarms->highest_severity);
    } else {
        snprintf(at, sizeof(at), "No alarms");
    }
    chip(r, hmi_layout_alarm_pill(sw, sh), ac, at, C_TEXT);

    /* Role */
    static const char * const roles[4] = { "Viewer", "Operator", "Maintenance", "Engineer" };
    int role = v->sec ? (int)v->sec->current_role : 0;
    if (role < 0 || role > 3) role = 0;
    chip(r, hmi_layout_role_pill(sw, sh), C_PURPLE, roles[role], C_TEXT);

    /* E-stop */
    hmi_rect_t e = hmi_layout_estop_button(sw, sh);
    hmi_draw_round_rect(r, e.x, e.y, e.w, e.h, e.h / 2, C_RED);
    text_center(r, &hmi_font_body_b, e, "E-STOP", C_WHITE);
}

static void render_banner(hmi_renderer_t *r, const hmi_view_t *v)
{
    if (!v->alarms || !v->alarms->current_action.active) return;
    const hmi_emergency_action_t *a = &v->alarms->current_action;
    hmi_rect_t b = hmi_layout_banner(r->screen_w, r->screen_h);
    if (!band_hits(r, b.y, b.h)) return;
    uint8_t sc = sev_color(a->severity);
    bool unacked = (a->state == ALARM_PT_UNACK || a->state == ALARM_PT_RTN_UNACK);

    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 14, C_CARD, unacked ? sc : C_LINE);

    int cx = b.x + 32, cy = b.y + b.h / 2;
    hmi_draw_circle(r, cx, cy, 14, sc);
    hmi_draw_rect(r, cx - 2 + 1, cy - 8, 3, 10, C_WHITE);
    hmi_draw_rect(r, cx - 2 + 1, cy + 5, 3, 3, C_WHITE);

    hmi_rect_t ack = hmi_layout_banner_ack(r->screen_w, r->screen_h);
    int tx = b.x + 58;
    int tw = ack.x - 16 - tx;
    char title[96], s1[64], s2[64], steps[136];
    nice_name(title, sizeof(title), a->title);
    nice_name(s1, sizeof(s1), a->action_step1);
    nice_name(s2, sizeof(s2), a->action_step2);
    text_fit(r, &hmi_font_title, tx, b.y + 10, tw, title, sc);
    snprintf(steps, sizeof(steps), "%s     %s", s1, s2);
    text_fit(r, &hmi_font_body, tx, b.y + 33, tw, steps, C_MUTED);

    if (unacked) {
        button(r, ack, "Acknowledge", role_allows(v, HMI_CMD_ACK_ALARMS) ? BTN_PRIMARY : BTN_DISABLED);
    } else {
        text_center(r, &hmi_font_body_b, ack, "Acknowledged", C_MUTED);
    }
}

typedef enum { ICON_PROCESS, ICON_DIAG, ICON_TELEMETRY, ICON_ALARM, ICON_SETTINGS } icon_t;

static void draw_icon(hmi_renderer_t *r, icon_t ic, int cx, int cy, uint8_t c, uint8_t bg)
{
    switch (ic) {
        case ICON_PROCESS:   /* tank with level */
            hmi_draw_round_frame(r, cx - 8, cy - 10, 16, 20, 5, bg, c);
            hmi_draw_round_frame(r, cx - 7, cy - 9, 14, 18, 4, bg, c);
            hmi_draw_round_rect(r, cx - 5, cy, 10, 7, 2, c);
            break;
        case ICON_DIAG:      /* pulse */
            hmi_draw_line(r, cx - 11, cy, cx - 5, cy, 2, c);
            hmi_draw_line(r, cx - 5, cy, cx - 2, cy - 8, 2, c);
            hmi_draw_line(r, cx - 2, cy - 8, cx + 2, cy + 8, 2, c);
            hmi_draw_line(r, cx + 2, cy + 8, cx + 5, cy, 2, c);
            hmi_draw_line(r, cx + 5, cy, cx + 11, cy, 2, c);
            break;
        case ICON_TELEMETRY: /* bars */
            hmi_draw_round_rect(r, cx - 10, cy - 1, 5, 10, 2, c);
            hmi_draw_round_rect(r, cx - 2, cy - 9, 5, 18, 2, c);
            hmi_draw_round_rect(r, cx + 6, cy - 5, 5, 14, 2, c);
            break;
        case ICON_ALARM:     /* bell */
            hmi_draw_round_rect(r, cx - 7, cy - 9, 14, 15, 6, c);
            hmi_draw_round_rect(r, cx - 9, cy + 3, 18, 4, 2, c);
            hmi_draw_circle(r, cx, cy + 8, 2, c);
            break;
        case ICON_SETTINGS:  /* gear */
            for (int k = 0; k < 8; k++) {
                int a = k * 4;
                hmi_draw_circle(r, cx + (icos(a) * 8) / 256, cy + (isin(a) * 8) / 256, 3, c);
            }
            hmi_draw_circle(r, cx, cy, 8, c);
            hmi_draw_circle(r, cx, cy, 3, bg);
            break;
    }
}

static void render_dock(hmi_renderer_t *r, const hmi_view_t *v)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_rect_t d = hmi_layout_dock(sw, sh);
    if (!band_hits(r, d.y, d.h)) return;
    hmi_draw_rect(r, d.x, d.y, d.w, d.h, C_CARD);
    separator(r, 0, d.y, sw);
    static const char * const labels[SCREEN_COUNT] = { "Process", "Diagnostics", "Telemetry", "Alarms", "Settings" };
    for (int i = 0; i < SCREEN_COUNT; i++) {
        hmi_rect_t t = hmi_layout_dock_tab(sw, sh, i);
        bool on = (i == v->active_screen);
        uint8_t c = on ? C_BLUE : C_MUTED;
        int cx = t.x + t.w / 2;
        draw_icon(r, (icon_t)i, cx, t.y + 20, c, C_CARD);
        int lw = hmi_text_width(&hmi_font_caption, labels[i]);
        hmi_draw_text(r, &hmi_font_caption, cx - lw / 2, t.y + 36, labels[i], c);
        if (i == SCREEN_ALARM_MATRIX && v->alarms && v->alarms->unacked_count) {
            char n[8];
            snprintf(n, sizeof(n), "%u", (unsigned)(v->alarms->unacked_count > 99 ? 99 : v->alarms->unacked_count));
            int bx = cx + 12, by = t.y + 10;
            hmi_draw_circle(r, bx, by, 8, C_RED);
            hmi_draw_text(r, &hmi_font_caption, bx - hmi_text_width(&hmi_font_caption, n) / 2, by - 7, n, C_WHITE);
        }
    }
}

static void render_toast(hmi_renderer_t *r, const hmi_view_t *v)
{
    if (!v->toast || !v->toast[0]) return;
    int w = hmi_text_width(&hmi_font_body_b, v->toast) + 40;
    if (w > r->screen_w - 32) w = r->screen_w - 32;
    hmi_rect_t t = { (r->screen_w - w) / 2, r->screen_h - HMI_LAYOUT_DOCK_H - 16 - 38, w, 38 };
    if (!band_hits(r, t.y, t.h)) return;
    hmi_draw_round_frame(r, t.x, t.y, t.w, t.h, 19, C_SURF, C_LINE);
    text_fit(r, &hmi_font_body_b, t.x + 20, t.y + (t.h - hmi_font_body_b.line_h) / 2, t.w - 40, v->toast, C_TEXT);
}

static void screen_title(hmi_renderer_t *r, const hmi_view_t *v, const char *title, const char *sub)
{
    hmi_rect_t c = hmi_layout_content(r->screen_w, r->screen_h, v->alarms && v->alarms->current_action.active);
    if (!band_hits(r, c.y, HMI_LAYOUT_TITLE_H)) return;
    int end = hmi_draw_text(r, &hmi_font_headline, c.x + 2, c.y + 2, title, C_TEXT);
    if (sub) hmi_draw_text(r, &hmi_font_body, end + 12, c.y + 9, sub, C_MUTED);
}

/* ========================================================================= */
/* Fixed screens                                                              */
/* ========================================================================= */

static void render_diagnostics(hmi_renderer_t *r, const hmi_view_t *v, bool banner)
{
    char b1[48], b2[48];
    screen_title(r, v, "Diagnostics", "live, measured");
    hmi_rect_t body = hmi_layout_screen_body(r->screen_w, r->screen_h, banner);
    int g = HMI_LAYOUT_GAP;
    int w1 = (body.w - 2 * g) * 30 / 100, w2 = w1, w3 = body.w - 2 * g - w1 - w2;
    hmi_rect_t c1 = { body.x, body.y, w1, (body.h - g) * 52 / 100 };
    hmi_rect_t c1b = { body.x, c1.y + c1.h + g, w1, body.h - c1.h - g };
    hmi_rect_t c2 = { body.x + w1 + g, body.y, w2, body.h };
    hmi_rect_t c3 = { c2.x + w2 + g, body.y, w3, body.h };
    const int rh = 20;

    /* Fieldbus loop */
    card(r, c1, "Fieldbus loop");
    if (band_hits(r, c1.y, c1.h)) {
        int x = c1.x + 16, w = c1.w - 32, y = c1.y + 42;
        const hmi_fieldbus_stats_t *fb = v->fb;
        int rows = (c1.h - 50) / rh;
        if (fb) {
            uint8_t rc = (fb->loop_hz >= 950 && fb->loop_hz <= 1050) ? C_GREEN : C_ORANGE;
            snprintf(b1, sizeof(b1), "%lu Hz", (unsigned long)fb->loop_hz);
            if (rows-- > 0) { kv_row(r, x, y, w, "Rate", b1, rc); y += rh; }
            snprintf(b1, sizeof(b1), "%lu us", (unsigned long)fb->period_avg_us);
            if (rows-- > 0) { kv_row(r, x, y, w, "Period", b1, C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu / %lu us", (unsigned long)fb->period_min_us, (unsigned long)fb->period_max_us);
            if (rows-- > 0) { kv_row(r, x, y, w, "Min / max", b1, C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu", (unsigned long)fb->late_count);
            if (rows-- > 0) { kv_row(r, x, y, w, "Late (> 2 ms)", b1, fb->late_count ? C_ORANGE : C_TEXT); y += rh; }
            if (rows-- > 0) { kv_row(r, x, y, w, "Timer", fb->high_res_timer ? "High-res" : "Sleep(1)", C_TEXT); }
        } else {
            kv_row(r, x, y, w, "Status", "Starting...", C_DIM);
        }
    }

    /* Integrity */
    card(r, c1b, "Integrity");
    if (band_hits(r, c1b.y, c1b.h)) {
        int x = c1b.x + 16, w = c1b.w - 32, y = c1b.y + 42;
        int rows = (c1b.h - 50) / rh;
        snprintf(b1, sizeof(b1), "%08lX", v->fb ? (unsigned long)v->fb->snapshot_crc : 0ul);
        if (rows-- > 0) { kv_row(r, x, y, w, "CRC32-C", b1, C_TEXT); y += rh; }
        snprintf(b1, sizeof(b1), "%lu", (unsigned long)v->crc_mismatches);
        if (rows-- > 0) { kv_row(r, x, y, w, "CRC errors", b1, v->crc_mismatches ? C_RED : C_GREEN); y += rh; }
        snprintf(b1, sizeof(b1), "%lu ms", (unsigned long)v->snapshot_age_ms);
        if (rows-- > 0) { kv_row(r, x, y, w, "Snapshot age", b1, v->data_stale ? C_RED : C_TEXT); y += rh; }
        snprintf(b1, sizeof(b1), "%lu", (unsigned long)v->seqlock_retries);
        if (rows-- > 0) { kv_row(r, x, y, w, "Seqlock retries", b1, C_TEXT); }
    }

    /* PLC link */
    card(r, c2, "PLC link");
    if (band_hits(r, c2.y, c2.h)) {
        int x = c2.x + 16, w = c2.w - 32, y = c2.y + 42;
        int rows = (c2.h - 50) / rh;
        const hmi_driver_stats_t *p = v->plc;
        if (p) {
            static const char * const ls[3] = { "Down", "Connecting", "Up" };
            uint8_t lc = p->link_state == HMI_LINK_UP ? C_GREEN : p->link_state == HMI_LINK_CONNECTING ? C_ORANGE : C_RED;
            if (rows-- > 0) { kv_row(r, x, y, w, "Mode", p->mode == HMI_DRV_MODE_MODBUS_TCP ? "Modbus TCP" : "Simulator", C_TEXT); y += rh; }
            if (rows-- > 0) { kv_row(r, x, y, w, "Endpoint", p->endpoint, C_TEXT); y += rh; }
            if (rows-- > 0) { kv_row(r, x, y, w, "Link", ls[p->link_state < 3 ? p->link_state : 0], lc); y += rh; }
            snprintf(b1, sizeof(b1), "%lu / %lu", (unsigned long)p->responses_ok, (unsigned long)p->requests_sent);
            if (rows-- > 0) { kv_row(r, x, y, w, "Replies / requests", b1, C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu", (unsigned long)p->timeouts);
            if (rows-- > 0) { kv_row(r, x, y, w, "Timeouts", b1, p->timeouts ? C_ORANGE : C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu / %lu", (unsigned long)p->exceptions, (unsigned long)p->protocol_errors);
            if (rows-- > 0) { kv_row(r, x, y, w, "Exceptions / errors", b1, (p->exceptions || p->protocol_errors) ? C_ORANGE : C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu", (unsigned long)p->reconnects);
            if (rows-- > 0) { kv_row(r, x, y, w, "Connections", b1, C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu / %lu us", (unsigned long)p->last_rtt_us, (unsigned long)p->max_rtt_us);
            if (rows-- > 0) { kv_row(r, x, y, w, "RTT last / max", b1, C_TEXT); y += rh; }
            snprintf(b1, sizeof(b1), "%lu / %lu", (unsigned long)p->writes_ok, (unsigned long)p->writes_failed);
            if (rows-- > 0) { kv_row(r, x, y, w, "Writes ok / failed", b1, p->writes_failed ? C_ORANGE : C_TEXT); }
        }
    }

    /* Register map */
    card(r, c3, "Register map");
    if (band_hits(r, c3.y, c3.h)) {
        int x = c3.x + 16, w = c3.w - 32, y = c3.y + 44;
        int cx1 = x, cx2 = x + w * 18 / 100, cx3 = x + w * 72 / 100, cx4 = x + w;
        hmi_draw_text(r, &hmi_font_caption, cx1, y, "ADDR", C_DIM);
        hmi_draw_text(r, &hmi_font_caption, cx2, y, "TAG", C_DIM);
        text_right(r, &hmi_font_caption, cx3 + w * 10 / 100, y, "VALUE", C_DIM);
        text_right(r, &hmi_font_caption, cx4, y, "RAW", C_DIM);
        y += 18;
        separator(r, x, y - 3, w);
        static const uint8_t ids[] = { 0, 1, 2, 3, 4, 5, 6, 7, 12 };
        static const char * const addr[] = { "%MW0", "%MW1", "%MW2", "%MW3", "%MW4", "%M0", "%M1", "%M2", "LINK" };
        for (unsigned i = 0; i < sizeof(ids) && y + 18 <= c3.y + c3.h - 8; i++) {
            const hmi_tag_meta_t *m = hmi_tag_get_meta(ids[i]);
            if (!m) continue;
            int16_t val; uint8_t q;
            bool ok = tag_value(v, ids[i], &val, &q);
            nice_name(b1, sizeof(b1), m->name);
            hmi_draw_text(r, &hmi_font_body, cx1, y, addr[i], C_MUTED);
            text_fit(r, &hmi_font_body, cx2, y, cx3 - cx2 - 40, b1, C_TEXT);
            fmt_value(b2, sizeof(b2), v, ids[i]);
            text_right(r, &hmi_font_body_b, cx3 + w * 10 / 100, y, b2, ok ? C_TEXT : C_RED);
            snprintf(b2, sizeof(b2), "%04X", (unsigned)(uint16_t)val);
            text_right(r, &hmi_font_body, cx4, y, b2, ok ? C_MUTED : C_RED);
            y += 20;
        }
    }
}

static void render_alarms(hmi_renderer_t *r, const hmi_view_t *v, bool banner)
{
    int sw = r->screen_w, sh = r->screen_h;
    char b1[64], b2[48];
    const hmi_alarm_supervisor_t *al = v->alarms;
    if (al) snprintf(b1, sizeof(b1), "%u active, %u unacknowledged", (unsigned)al->active_count, (unsigned)al->unacked_count);
    else snprintf(b1, sizeof(b1), "no data");
    screen_title(r, v, "Alarms", (sw >= 900) ? b1 : NULL);

    bool can_ack = role_allows(v, HMI_CMD_ACK_ALARMS);
    bool estop = v->tags && v->tags->tags[SCHNEIDER_TAG_ESTOP_ACTIVE].value != 0;
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_ALARM_ACK), "Acknowledge all",
           (can_ack && al && al->unacked_count) ? BTN_PRIMARY : BTN_DISABLED);
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_ALARM_SILENCE), "Silence horn",
           (can_ack && al && al->horn_sounding) ? BTN_PLAIN : BTN_DISABLED);
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_ESTOP_RESET), "Reset E-stop",
           (estop && role_allows(v, HMI_CMD_ESTOP_RESET)) ? BTN_DANGER : BTN_DISABLED);

    hmi_rect_t body = hmi_layout_screen_body(sw, sh, banner);
    int nact = 0;
    if (al) for (int i = 0; i < HMI_MAX_TAGS; i++) if (al->points[i].state != ALARM_PT_NORMAL) nact++;
    int act_rows = nact ? (nact > 4 ? 4 : nact) : 1;
    int ah = 46 + act_rows * 24;
    if (ah > body.h / 2) ah = body.h / 2;
    hmi_rect_t ca = { body.x, body.y, body.w, ah };
    hmi_rect_t cj = { body.x, body.y + ah + HMI_LAYOUT_GAP, body.w, body.h - ah - HMI_LAYOUT_GAP };

    int x = ca.x + 16, w = ca.w - 32;
    int col_name = x + 110, col_val = x + w * 58 / 100, col_state = x + w;
    card(r, ca, "Active");
    if (band_hits(r, ca.y, ca.h)) {
        int y = ca.y + 42;
        if (!nact) {
            hmi_draw_circle(r, x + 5, y + 8, 4, C_GREEN);
            hmi_draw_text(r, &hmi_font_body, x + 16, y, "No active or unacknowledged alarms", C_MUTED);
        } else {
            for (int i = 0; i < HMI_MAX_TAGS && y + 20 <= ca.y + ca.h - 4; i++) {
                const hmi_alarm_point_t *pt = &al->points[i];
                if (pt->state == ALARM_PT_NORMAL) continue;
                const hmi_tag_meta_t *m = hmi_tag_get_meta((uint8_t)i);
                uint8_t sc = sev_color(pt->peak_severity);
                hmi_draw_circle(r, x + 5, y + 8, 4, sc);
                hmi_draw_text(r, &hmi_font_body_b, x + 16, y, sev_label(pt->peak_severity), sc);
                nice_name(b1, sizeof(b1), m ? m->name : "Tag");
                text_fit(r, &hmi_font_body, col_name, y, col_val - col_name - 70, b1, C_TEXT);
                fmt_value(b2, sizeof(b2), v, (uint8_t)i);
                if (m && m->type == TAG_TYPE_ANALOG) {
                    char t[72];
                    snprintf(t, sizeof(t), "%s %s", b2, m->unit ? m->unit : "");
                    text_right(r, &hmi_font_body_b, col_val, y, t, C_TEXT);
                }
                text_right(r, &hmi_font_body, col_state, y,
                           state_label(pt->state), pt->state == ALARM_PT_ACKED ? C_MUTED : sc);
                y += 24;
            }
        }
    }

    card(r, cj, "Event journal");
    if (band_hits(r, cj.y, cj.h)) {
        int jx = cj.x + 16, jw = cj.w - 32, y = cj.y + 42;
        uint8_t n = al ? hmi_alarms_journal_count(al) : 0;
        if (!n) hmi_draw_text(r, &hmi_font_body, jx, y, "No events recorded", C_MUTED);
        for (uint8_t i = 0; i < n && y + 18 <= cj.y + cj.h - 6; i++) {
            const hmi_alarm_record_t *rec = hmi_alarms_journal_newest(al, i);
            if (!rec) break;
            fmt_uptime(b2, sizeof(b2), rec->timestamp_ms);
            hmi_draw_text(r, &hmi_font_body, jx, y, b2, C_MUTED);
            uint8_t sc = rec->severity >= ALARM_SEV_WARNING ? sev_color(rec->severity) : C_DIM;
            hmi_draw_circle(r, jx + 104, y + 8, 3, sc);
            int sx = jx + jw;
            const char *st = rec->tag_id == HMI_ALARM_TAG_SYSTEM ? "System" :
                             rec->state == ALARM_PT_NORMAL ? "Event" : state_label(rec->state);
            int stw = hmi_text_width(&hmi_font_caption, st);
            hmi_draw_text(r, &hmi_font_caption, sx - stw, y + 2, st, C_DIM);
            nice_name(b1, sizeof(b1), rec->message);
            text_fit(r, &hmi_font_body, jx + 116, y, sx - stw - 12 - (jx + 116), b1,
                     rec->severity >= ALARM_SEV_WARNING ? C_TEXT : C_MUTED);
            y += 21;
        }
    }
}

static void render_settings(hmi_renderer_t *r, const hmi_view_t *v, bool banner)
{
    int sw = r->screen_w, sh = r->screen_h;
    char b1[64];
    screen_title(r, v, "Settings", "IEC 62443 access control and system");
    hmi_rect_t ka = hmi_layout_settings_card(sw, sh, banner, 0);
    hmi_rect_t ks = hmi_layout_settings_card(sw, sh, banner, 1);
    const int rh = 21;

    /* Access control */
    card(r, ka, "Access control");
    if (band_hits(r, ka.y, ka.h)) {
        int x = ka.x + 16, w = ka.w - 32, y = ka.y + 44;
        int limit = ka.y + ka.h - 16 - HMI_LAYOUT_BUTTON_H - 6;
        static const char * const roles[4] = { "Viewer", "Operator", "Maintenance", "Engineer" };
        int role = v->sec ? (int)v->sec->current_role : 0;
        if (role < 0 || role > 3) role = 0;
        if (y + rh <= limit) { kv_row(r, x, y, w, "Current role", roles[role], C_PURPLE); y += rh; }
        if (v->sec && v->sec->default_pins && y + rh <= limit) {
            kv_row(r, x, y, w, "PINs", "Factory defaults - change!", C_ORANGE); y += rh;
        }
        y += 4;
        static const struct { hmi_command_t c; const char *n; } perms[] = {
            { HMI_CMD_ESTOP, "Emergency stop" }, { HMI_CMD_ACK_ALARMS, "Acknowledge alarms" },
            { HMI_CMD_PUMP_TOGGLE, "Operate pump / valve" }, { HMI_CMD_WATCHDOG_RESET, "Reset watchdog" },
            { HMI_CMD_INJECT_FAULT, "Fault injection" },
        };
        for (unsigned i = 0; i < sizeof(perms) / sizeof(perms[0]) && y + rh <= limit; i++) {
            bool ok = role_allows(v, perms[i].c);
            if (ok) snprintf(b1, sizeof(b1), "Allowed");
            else snprintf(b1, sizeof(b1), "Needs %s", roles[hmi_security_required_role(perms[i].c)]);
            kv_row(r, x, y, w, perms[i].n, b1, ok ? C_GREEN : C_DIM);
            y += rh;
        }
        if (v->sec && y + rh <= limit) {
            snprintf(b1, sizeof(b1), "%lu min", (unsigned long)(v->sec->auto_logout_timeout_ms / 60000u));
            kv_row(r, x, y, w, "Auto-logout", b1, C_TEXT); y += rh;
        }
        if (v->sec && y + rh <= limit) {
            snprintf(b1, sizeof(b1), "%lu", (unsigned long)v->sec->denied_count);
            kv_row(r, x, y, w, "Denied attempts", b1, v->sec->denied_count ? C_ORANGE : C_TEXT);
        }
    }
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_SET_ROLE),
           (v->sec && v->sec->current_role > ROLE_VIEWER) ? "Switch role / sign out" : "Sign in", BTN_PRIMARY);

    /* System */
    card(r, ks, "System");
    if (band_hits(r, ks.y, ks.h)) {
        int x = ks.x + 16, w = ks.w - 32, y = ks.y + 44;
        int limit = ks.y + ks.h - 16 - HMI_LAYOUT_BUTTON_H - 6;
        const hmi_watchdog_t *wd = v->wd;
        if (y + rh <= limit) { kv_row(r, x, y, w, "Fieldbus watchdog", (wd && wd->fieldbus_alive) ? "Healthy" : "Tripped",
                                      (wd && wd->fieldbus_alive) ? C_GREEN : C_RED); y += rh; }
        if (y + rh <= limit) { kv_row(r, x, y, w, "UI watchdog", v->ui_wd_alive ? "Healthy" : "Tripped",
                                      v->ui_wd_alive ? C_GREEN : C_RED); y += rh; }
        if (y + rh <= limit) { kv_row(r, x, y, w, "Failover latch", (wd && wd->failover_active) ? "Latched" : "Clear",
                                      (wd && wd->failover_active) ? C_ORANGE : C_TEXT); y += rh; }
        if (y + rh <= limit) {
            if (v->log && v->log->open) snprintf(b1, sizeof(b1), "%lu records", (unsigned long)v->log->records_written);
            else snprintf(b1, sizeof(b1), "Off");
            kv_row(r, x, y, w, "Alarm log (chained)", b1, (v->log && v->log->write_errors) ? C_RED : C_TEXT); y += rh;
        }
        if (y + rh <= limit) {
            snprintf(b1, sizeof(b1), "%lu KB", (unsigned long)((v->exe_size_bytes + 1023u) / 1024u));
            kv_row(r, x, y, w, "Executable", b1, C_TEXT); y += rh;
        }
        if (y + rh <= limit) {
            snprintf(b1, sizeof(b1), "%u bytes, %d bands", (unsigned)HMI_DISPLAY_MAX_BUF_SIZE, r->band_count);
            kv_row(r, x, y, w, "Frame buffer", b1, C_TEXT); y += rh;
        }
        if (y + rh <= limit) {
            snprintf(b1, sizeof(b1), "%lu fps", (unsigned long)v->ui_frame_hz);
            kv_row(r, x, y, w, "Display refresh", b1, C_TEXT);
        }
    }
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_SET_THEME),
           (v->sec && v->sec->high_contrast_theme) ? "Dark theme" : "Outdoor theme", BTN_PLAIN);
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_SET_WD_TEST), "Test watchdog",
           role_allows(v, HMI_CMD_INJECT_FAULT) ? BTN_DANGER : BTN_DISABLED);
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_SET_WD_RESET), "Reset failover",
           (role_allows(v, HMI_CMD_WATCHDOG_RESET) && v->wd && v->wd->failover_active) ? BTN_PLAIN : BTN_DISABLED);
}

/* ========================================================================= */
/* Sign-in dialogs                                                            */
/* ========================================================================= */

static const char * const s_role_names[HMI_ROLE_COUNT] = { "Viewer", "Operator", "Maintenance", "Engineer" };
static const char * const s_role_desc[HMI_ROLE_COUNT] = {
    "Monitor only - no PIN", "Alarms, pumps, valves", "Plus watchdog reset", "Full access incl. tests"
};

/* Dim everything already in the band (modal scrim): each pixel is blended 2/3 toward
 * the background through the palette LUT. No extra memory, one pass per band. */
static void scrim_band(hmi_renderer_t *r)
{
    uint8_t map[16];
    for (int c = 0; c < 16; c++) map[c] = r->blend[C_BG][c][2];
    size_t n = (size_t)r->band_h * (size_t)(r->screen_w / 2);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = r->buffer[i];
        r->buffer[i] = (uint8_t)((map[b >> 4] << 4) | map[b & 15]);
    }
}

static void render_modal(hmi_renderer_t *r, const hmi_view_t *v)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_rect_t m = hmi_layout_modal(sw, sh, v->modal);
    if (m.w == 0 || !band_hits(r, m.y - 6, m.h + 12)) return;

    /* soft shadow + sheet */
    hmi_draw_round_rect(r, m.x - 3, m.y - 1, m.w + 6, m.h + 7, 18, C_BG);
    hmi_draw_round_frame(r, m.x, m.y, m.w, m.h, 16, C_CARD, C_LINE);
    hmi_rect_t cancel = hmi_layout_modal_cancel(sw, sh, v->modal);
    text_center(r, &hmi_font_body_b, cancel, "Cancel", C_BLUE);

    int role_now = v->sec ? (int)v->sec->current_role : 0;
    if (v->modal == HMI_MODAL_ROLE) {
        hmi_draw_text(r, &hmi_font_title, m.x + 18, m.y + 16, "Sign in as", C_TEXT);
        for (int i = 0; i < HMI_ROLE_COUNT; i++) {
            hmi_rect_t row = hmi_layout_modal_role(sw, sh, i);
            if (!band_hits(r, row.y, row.h)) continue;
            bool cur = (i == role_now);
            hmi_draw_round_frame(r, row.x, row.y, row.w, row.h, 10, C_SURF, cur ? C_BLUE : C_LINE);
            hmi_draw_circle(r, row.x + 18, row.y + row.h / 2, 5, i == 0 ? C_DIM : C_PURPLE);
            hmi_draw_text(r, &hmi_font_body_b, row.x + 34, row.y + 5, s_role_names[i], C_TEXT);
            hmi_draw_text(r, &hmi_font_caption, row.x + 34, row.y + 23, s_role_desc[i], C_MUTED);
            if (cur) text_right(r, &hmi_font_caption, row.x + row.w - 14, row.y + 15, "CURRENT", C_BLUE);
        }
        return;
    }

    /* PIN keypad */
    char title[48];
    int role = v->modal_role < HMI_ROLE_COUNT ? v->modal_role : 1;
    snprintf(title, sizeof(title), "PIN for %s", s_role_names[role]);
    text_fit(r, &hmi_font_title, m.x + 18, m.y + 16, cancel.x - 8 - (m.x + 18), title, C_TEXT);

    int dots_y = m.y + 52 + 14;
    int n = HMI_PIN_MAX_LEN;
    int step = 22;
    int dx0 = m.x + (m.w - (n - 1) * step) / 2;
    for (int i = 0; i < n; i++) {
        if (i < v->pin_len) hmi_draw_circle(r, dx0 + i * step, dots_y, 6, C_TEXT);
        else { hmi_draw_circle(r, dx0 + i * step, dots_y, 6, C_LINE); hmi_draw_circle(r, dx0 + i * step, dots_y, 4, C_CARD); }
    }

    char lock[48];
    const char *msg = v->modal_msg ? v->modal_msg : "Enter 4 to 8 digits";
    bool err = v->modal_msg_error;
    if (v->lockout_ms) {
        snprintf(lock, sizeof(lock), "Too many attempts - wait %lu s", (unsigned long)((v->lockout_ms + 999u) / 1000u));
        msg = lock;
        err = true;
    }
    hmi_rect_t mr = { m.x + 8, m.y + 52 + 36, m.w - 16, 20 };
    text_center(r, &hmi_font_caption, mr, msg, err ? C_RED : C_MUTED);

    static const char * const labels[12] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Delete", "OK" };
    for (int k = 0; k < 12; k++) {
        hmi_rect_t kr = hmi_layout_modal_key(sw, sh, k);
        if (!band_hits(r, kr.y, kr.h)) continue;
        bool ok = (k == HMI_KEY_OK);
        bool can_ok = v->pin_len >= HMI_PIN_MIN_LEN && !v->lockout_ms;
        uint8_t fill = ok ? (can_ok ? C_BLUE : C_SURF) : C_SURF;
        hmi_draw_round_frame(r, kr.x, kr.y, kr.w, kr.h, 10, fill, ok && can_ok ? C_BLUE : C_LINE);
        const hmi_font_t *f = (k <= 9) ? &hmi_font_title : &hmi_font_body_b;
        uint8_t tc = ok ? (can_ok ? C_WHITE : C_DIM) : (v->lockout_ms ? C_DIM : C_TEXT);
        text_center(r, f, kr, labels[k], tc);
    }
}

/* ========================================================================= */
/* Frame dispatch                                                             */
/* ========================================================================= */

void hmi_renderer_render_band(hmi_renderer_t *r, int band_idx, const hmi_view_t *v)
{
    if (!r || !v || band_idx < 0 || band_idx >= r->band_count) return;
    r->current_band_y = band_idx * r->band_h;
    reset_clip(r);

    /* Clear band (rows beyond screen_h in the last band are harmless) */
    memset(r->buffer, (C_BG << 4) | C_BG, (size_t)r->band_h * (size_t)(r->screen_w / 2));

    bool banner = v->alarms && v->alarms->current_action.active;
    hmi_rect_t content = hmi_layout_content(r->screen_w, r->screen_h, banner);

    switch (v->active_screen) {
        case SCREEN_PROCESS_OVERVIEW: render_manifest(r, v, v->tokens_process, v->ntok_process, content); break;
        case SCREEN_HOST_TELEMETRY:   render_manifest(r, v, v->tokens_host, v->ntok_host, content); break;
        case SCREEN_FIELD_DIAGNOSTICS: render_diagnostics(r, v, banner); break;
        case SCREEN_ALARM_MATRIX:      render_alarms(r, v, banner); break;
        case SCREEN_SECURITY_SETTINGS: render_settings(r, v, banner); break;
        default: break;
    }

    render_header(r, v);
    render_banner(r, v);
    render_dock(r, v);
    if (v->modal != HMI_MODAL_NONE) {
        scrim_band(r);
        /* keep the E-STOP at full strength: it stays operable over dialogs */
        hmi_rect_t e = hmi_layout_estop_button(r->screen_w, r->screen_h);
        if (band_hits(r, e.y, e.h)) {
            hmi_draw_round_rect(r, e.x, e.y, e.w, e.h, e.h / 2, C_RED);
            text_center(r, &hmi_font_body_b, e, "E-STOP", C_WHITE);
        }
        render_modal(r, v);
    } else {
        render_toast(r, v);
    }
}
