/**
 * @file micro_renderer.c
 * @brief Band-buffer rasteriser, anti-aliased primitives and the five HMI screens
 * @standards ISA-101 (grey graphics, colour only for abnormal state) / ISO 9241 touch targets
 *
 * Portable C99. No OS calls, no heap, integer-only pixel maths.
 * Every primitive clips to the active band AND to the clip rect, so a frame
 * is produced band by band from one 19,200-byte buffer.
 */

#include "renderer/micro_renderer.h"
#include "renderer/clut16.h"
#include <string.h>
#include <stdio.h>

/* Short colour names */
#define C_BG      COLOR_OBSIDIAN_BG
#define C_CARD    COLOR_TITANIUM_CARD
#define C_LINE    COLOR_CARD_BORDER
#define C_TEXT    COLOR_CHALK_WHITE
#define C_MUTED   COLOR_SILVER_MUTED
#define C_TRACK   COLOR_DARK_GRAY
#define C_TEAL    COLOR_CYAN_ACCENT    /* simulated-data marker only */
#define C_INFO    COLOR_BLUE_ACCENT    /* the INFO alarm priority only: blue is not used for controls */
#define C_NEUTRAL COLOR_SILVER_MUTED   /* analog fills, trends: normal values are grey (ISA-101) */
#define C_OK      COLOR_EMERALD_OK   /* normal / running: green, or grey in the high-performance scheme */
#define C_ORANGE  COLOR_AMBER_WARN
#define C_RED     COLOR_CRIMSON_TRIP
#define C_YELLOW  COLOR_VIBRANT_ORANGE
#define C_SURF    COLOR_SURFACE_HOVER
#define C_DIM     COLOR_DIM_TEXT
#define C_WHITE   COLOR_WHITE_PURE

/* ========================================================================= */
/* Lifecycle                                                                  */
/* ========================================================================= */

/* Status scheme greens (IEC 60073 "normal"): dark theme, outdoor theme */
#define STATUS_GREEN_DARK     0x30D158u
#define STATUS_GREEN_OUTDOOR  0x1E7E34u

static const uint32_t* palette_of(const hmi_renderer_t *r)
{
    return r ? r->palette : g_hmi_palette_dark;
}

static void load_palette(hmi_renderer_t *r)
{
    const uint32_t *base = r->high_contrast ? g_hmi_palette_contrast : g_hmi_palette_dark;
    for (int i = 0; i < 16; i++) r->palette[i] = base[i];
    if (!r->high_performance)
        r->palette[COLOR_EMERALD_OK] = r->high_contrast ? STATUS_GREEN_OUTDOOR : STATUS_GREEN_DARK;
}

static void build_blend_lut(hmi_renderer_t *r)
{
    load_palette(r);
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

void hmi_renderer_set_scheme(hmi_renderer_t *rend, bool high_performance)
{
    if (!rend) return;
    rend->high_performance = high_performance;
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

/* The EMERGENCY button label, with some air on narrow (compact-header) buttons */
static void emergency_label(hmi_renderer_t *r, hmi_rect_t b, uint8_t c)
{
    const hmi_font_t *f = hmi_text_width(&hmi_font_title, "EMERGENCY") + 16 <= b.w ? &hmi_font_title : &hmi_font_body_b;
    text_center(r, f, b, "EMERGENCY", c);
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
    if (sev == ALARM_SEV_INFO) return C_INFO;
    return C_OK;
}

static const char* sev_label(uint8_t sev)
{
    switch (sev) {
        case ALARM_SEV_ESTOP:    return "Stop";
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

static uint8_t point_sev(const hmi_view_t *v, hmi_tag_id_t tag)
{
    if (!v->alarms || tag >= HMI_MAX_TAGS) return ALARM_SEV_NONE;
    return v->alarms->points[tag].severity;
}

static bool tag_value(const hmi_view_t *v, hmi_tag_id_t tag, int16_t *val, uint8_t *q)
{
    if (!v->tags || tag >= HMI_MAX_TAGS) { *val = 0; *q = QUALITY_COMM_FAULT; return false; }
    *val = v->tags->tags[tag].value;
    *q = v->tags->tags[tag].quality;
    return q_ok(*q);
}

static void fmt_value(char *out, size_t cap, const hmi_view_t *v, hmi_tag_id_t tag)
{
    int16_t val; uint8_t q;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!tag_value(v, tag, &val, &q)) { snprintf(out, cap, "--"); return; }
    hmi_format_scaled(out, cap, val, m ? m->decimals : 0);
}


/* Wall-clock time of an event that happened at uptime ts_ms. Same day: "14:03:27"; another day:
 * "11-01 14:03"; no clock on this panel: uptime "+00:00:31". */
static void fmt_clock(char *out, size_t cap, const hmi_view_t *v, uint32_t ts_ms)
{
    if (!v->utc_ms) {
        uint32_t sec = ts_ms / 1000u;
        snprintf(out, cap, "+%02lu:%02lu:%02lu", (unsigned long)(sec / 3600u), (unsigned long)((sec / 60u) % 60u),
                 (unsigned long)(sec % 60u));
        return;
    }
    /* signed: an event stamped by the fieldbus thread a moment after the UI read its clock must
     * not wrap to "49 days ago" */
    int32_t age = (int32_t)(v->now_ms - ts_ms);
    if (age < 0) age = 0;
    int64_t local_now = (int64_t)v->utc_ms + (int64_t)v->tz_offset_s * 1000;
    int64_t t = local_now - (int64_t)age;
    if (t < 0) t = 0;
    int64_t day = t / 86400000, today = local_now / 86400000;
    uint32_t sod = (uint32_t)((t % 86400000) / 1000);
    if (day == today) {
        snprintf(out, cap, "%02u:%02u:%02u", (unsigned)(sod / 3600), (unsigned)(sod / 60 % 60), (unsigned)(sod % 60));
    } else {                                                /* civil date from the day count */
        int64_t z = day + 719468, era = z / 146097;
        unsigned doe = (unsigned)(z - era * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
        snprintf(out, cap, "%02u-%02u %02u:%02u", m, d, (unsigned)(sod / 3600), (unsigned)(sod / 60 % 60));
    }
}

/* Priority symbols (ISA-101 / ISA-18.2: priority is never shown by colour alone):
 *   critical and stop  red diamond     warning  amber triangle     info  blue circle */
static void fill_diamond(hmi_renderer_t *r, int cx, int cy, int s, uint8_t c)
{
    for (int dy = -s; dy <= s; dy++) {
        int half = s - (dy < 0 ? -dy : dy);
        hmi_draw_rect(r, cx - half, cy + dy, 2 * half + 1, 1, c);
    }
}

static void fill_triangle(hmi_renderer_t *r, int cx, int cy, int s, uint8_t c)
{
    int h = (s * 2 * 87) / 100 + 1;                        /* equilateral: height = side * 0.87 */
    int top = cy - (2 * h) / 3;
    for (int i = 0; i < h; i++) {
        int half = (i * s) / h;
        hmi_draw_rect(r, cx - half, top + i, 2 * half + 1, 1, c);
    }
}

static void sev_icon(hmi_renderer_t *r, int cx, int cy, int s, uint8_t sev, uint8_t fg)
{
    if (sev >= ALARM_SEV_CRITICAL) {
        fill_diamond(r, cx, cy, s, sev_color(sev));
        if (s >= 9) { hmi_draw_rect(r, cx - 1, cy - s / 2, 3, s * 2 / 3, fg); hmi_draw_rect(r, cx - 1, cy + s / 3, 3, 3, fg); }
    } else if (sev == ALARM_SEV_WARNING) {
        fill_triangle(r, cx, cy, s + 1, sev_color(sev));
        if (s >= 9) { hmi_draw_rect(r, cx - 1, cy - s / 3, 3, s / 2, C_BG); hmi_draw_rect(r, cx - 1, cy + s / 3 - 1, 3, 3, C_BG); }
    } else if (sev == ALARM_SEV_INFO) {
        hmi_draw_circle(r, cx, cy, s * 3 / 4, sev_color(sev));
    } else {
        hmi_draw_circle(r, cx, cy, s / 2 > 3 ? s / 2 : 3, C_DIM);
    }
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
    /* ISA-101: controls are neutral. The one primary action is shown by contrast (an inverted,
     * light fill), not by colour; red is kept for stop actions and alarms are the only other colour. */
    uint8_t fill = C_SURF, border = C_LINE, text = C_TEXT;
    switch (st) {
        case BTN_PRIMARY:    fill = C_TEXT; border = C_TEXT; text = C_BG; break;
        case BTN_FILLED_RED: fill = C_RED;  border = C_RED;  text = C_WHITE; break;
        case BTN_DANGER:     text = C_RED; break;
        case BTN_DISABLED:   text = C_DIM; break;
        case BTN_PLAIN:      default: break;
    }
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 10, fill, border);
    /* Label: body font, or caption font when the button is narrow, else truncated */
    const hmi_font_t *f = &hmi_font_body_b;
    if (hmi_text_width(f, label) > b.w - 16) f = &hmi_font_caption;
    const char *sp = strchr(label, ' ');
    if (hmi_text_width(f, label) > b.w - 12 && sp && !strchr(sp + 1, ' ') && 2 * f->line_h <= b.h - 4 &&
        (size_t)(sp - label) < 32) {                            /* two words: one per line */
        char first[32];
        memcpy(first, label, (size_t)(sp - label));
        first[sp - label] = '\0';
        if (hmi_text_width(f, first) <= b.w - 12 && hmi_text_width(f, sp + 1) <= b.w - 12) {
            int y0 = b.y + (b.h - 2 * f->line_h) / 2;
            hmi_rect_t l1 = { b.x, y0, b.w, f->line_h }, l2 = { b.x, y0 + f->line_h, b.w, f->line_h };
            text_center(r, f, l1, first, text);
            text_center(r, f, l2, sp + 1, text);
            return;
        }
    }
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

static void w_card(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    char name[48];
    nice_name(name, sizeof(name), m ? m->name : "Tag");
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 14, C_CARD, C_LINE);

    /* Status at the top right: dot + word */
    int16_t val; uint8_t q;
    const char *st = "Normal";
    uint8_t col = C_OK;
    if (v->data_stale) { st = "Stale"; col = C_DIM; }
    else if (!tag_value(v, tag, &val, &q)) { st = "No data"; col = C_RED; }
    else {
        uint8_t sev = point_sev(v, tag);
        const hmi_tag_meta_t *mm = hmi_tag_get_meta(tag);
        bool dig = mm && mm->type == TAG_TYPE_DIGITAL;
        if (sev >= ALARM_SEV_CRITICAL) { st = dig ? "Alarm" : "High-high"; col = C_RED; }
        else if (sev == ALARM_SEV_WARNING) { st = dig ? "Alarm" : "High"; col = C_ORANGE; }
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
static int hero_fit(hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag)
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

static void w_hero(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag, int font_idx)
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

static void w_bar(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag)
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

    int vw = hmi_text_width(&hmi_font_body_b, txt);
    int nw = hmi_text_width(&hmi_font_body, name);
    int bh = 8;
    int by = b.y + hmi_font_body.line_h + 5;
    int bw = b.w;
    if (nw + 8 + vw <= b.w) {                     /* label and value on one line, bar below */
        text_fit(r, &hmi_font_body, b.x, b.y, b.w - vw - 8, name, C_MUTED);
        text_right(r, &hmi_font_body_b, b.x + b.w, b.y, txt, vc);
    } else {                                      /* narrow: never cut the label - it gets the line */
        text_fit(r, &hmi_font_body, b.x, b.y, b.w, name, C_MUTED);
        bw = b.w - vw - 8;
        text_right(r, &hmi_font_body_b, b.x + b.w, by + bh / 2 - hmi_font_body_b.line_h / 2, txt, vc);
    }
    b.w = bw;
    hmi_draw_round_rect(r, b.x, by, b.w, bh, bh / 2, C_TRACK);
    int span = m->max_val - m->min_val;
    if (span <= 0) return;
    if (ok) {
        long fw = ((long)(raw - m->min_val) * b.w) / span;
        if (fw < 0) fw = 0;
        if (fw > b.w) fw = b.w;
        if (fw >= 2) {
            uint8_t fc = sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_NEUTRAL;
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

/* Status chip. "Running" / "Stopped" only for a motor (a tag the device starts); any other
 * signal is named ("Network control: On", "Flame: Off"), so a stopped drive never shows "Running". */
static void w_pill(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag,
                   const hmi_device_info_t *dev, uint8_t rel)
{
    if (!band_hits(r, b.y, b.h)) return;
    int16_t val; uint8_t q;
    char t[48];
    uint8_t dot;
    bool motor = false;
    if (dev && dev->profile)
        for (uint8_t i = 0; i < dev->profile->n_actions; i++)
            if (dev->profile->actions[i].rel_tag == rel && hmi_act_is_start(dev->profile->actions[i].kind)) motor = true;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (v->data_stale || !tag_value(v, tag, &val, &q)) { snprintf(t, sizeof(t), "Unknown"); dot = C_RED; }
    else if (motor || !m || !m->name) { snprintf(t, sizeof(t), "%s", val ? "Running" : "Stopped"); dot = val ? C_OK : C_DIM; }
    else {
        char nm[32];
        nice_name(nm, sizeof(nm), m->name);
        snprintf(t, sizeof(t), "%s: %s", nm, val ? "On" : "Off");
        dot = val ? C_OK : C_DIM;
    }
    int h = b.h < 30 ? (b.h > 22 ? b.h : 22) : 30;          /* shrink in small panels, never overlap the next row */
    hmi_rect_t p = { b.x, b.y + (b.h - h) / 2, hmi_text_width(&hmi_font_body_b, t) + 40, h };
    if (p.w > b.w) p.w = b.w;
    chip(r, p, dot, t, C_TEXT);
}

/* Label and style of one control button of device dev for action a, given the tag state */
static btn_style_t control_look(const hmi_view_t *v, const hmi_device_info_t *dev, const hmi_device_action_t *a,
                                uint8_t option, bool ok, int16_t val, char *label, size_t cap)
{
    const hmi_tag_meta_t *m = hmi_tag_get_meta(HMI_DEV_TAG(dev->slot, a->rel_tag));
    bool blocked = a->block_tag != HMI_REL_NONE && v->tags &&
                   v->tags->tags[HMI_DEV_TAG(dev->slot, a->block_tag)].value != 0;
    btn_style_t st = BTN_PLAIN;
    switch (a->kind) {
        case HMI_ACT_TOGGLE_COIL: {
            bool on = ok && val != 0;
            if (!on && blocked) {
                const hmi_tag_meta_t *bm = hmi_tag_get_meta(HMI_DEV_TAG(dev->slot, a->block_tag));
                snprintf(label, cap, "Blocked: %s", bm ? bm->name : "interlock");
                return BTN_DISABLED;
            }
            snprintf(label, cap, "%s", on ? a->label_on : a->label);
            st = on ? BTN_DANGER : BTN_PRIMARY;
            break;
        }
        case HMI_ACT_TOGGLE_REG: {
            char num[16], tv[24];
            int16_t target = hmi_action_reg_target(a, ok ? val : a->threshold);
            hmi_format_scaled(num, sizeof(num), target, m ? m->decimals : 0);
            snprintf(tv, sizeof(tv), "%s %s", num, (m && m->unit) ? m->unit : "");
            snprintf(label, cap, (ok && val > a->threshold) ? a->label_on : a->label, tv);
            break;
        }
        case HMI_ACT_COIL_ON:
        case HMI_ACT_REG_ON:
            if (blocked) {
                const hmi_tag_meta_t *bm = hmi_tag_get_meta(HMI_DEV_TAG(dev->slot, a->block_tag));
                snprintf(label, cap, "Blocked: %s", bm ? bm->name : "interlock");
                return BTN_DISABLED;
            }
            snprintf(label, cap, "%s", a->label);
            st = (ok && val != 0) ? BTN_PLAIN : BTN_PRIMARY;
            break;
        case HMI_ACT_COIL_OFF:
        case HMI_ACT_REG_OFF:
            snprintf(label, cap, "%s", a->label);
            st = (ok && val != 0) ? BTN_DANGER : BTN_PLAIN;
            break;
        case HMI_ACT_PULSE_COIL:
        case HMI_ACT_PULSE_REG:
            snprintf(label, cap, "%s", a->label);
            st = (ok && val != 0) ? BTN_PRIMARY : BTN_PLAIN;
            break;
        case HMI_ACT_SET_REG:
            snprintf(label, cap, "%s...", a->label);
            break;
        case HMI_ACT_SET_ENUM:
            if (option == HMI_FP_NO_OPTION) {                 /* dashboard: "Mode: Auto" */
                char opt[24];
                hmi_action_option_label(a, ok && val >= 0 ? (uint8_t)val : 0xFFu, opt, sizeof(opt));
                snprintf(label, cap, "%s: %s", a->label, opt[0] ? opt : "--");
            } else {
                hmi_action_option_label(a, option, label, cap);
                st = (ok && val == option) ? BTN_PRIMARY : BTN_PLAIN;
            }
            break;
        default:
            snprintf(label, cap, "%s", a->label ? a->label : "Action");
            break;
    }
    return st;
}

static void w_button(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag,
                     const hmi_device_info_t *dev, uint8_t rel)
{
    if (!band_hits(r, b.y, b.h)) return;
    int16_t val; uint8_t q;
    bool ok = tag_value(v, tag, &val, &q) && !v->data_stale && dev && dev->state == HMI_DEV_ONLINE;
    char label[56];
    btn_style_t st;
    int idx = dev ? hmi_profile_quick_action(dev->profile, rel, ok ? val : 0) : -1;
    if (idx < 0) {
        const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
        nice_name(label, sizeof(label), m ? m->name : "Action");
        st = BTN_DISABLED;
    } else {
        st = control_look(v, dev, &dev->profile->actions[idx], HMI_FP_NO_OPTION, ok, val, label, sizeof(label));
    }
    if (!ok) st = BTN_DISABLED;       /* a role that is too low does not disable: tapping asks for a PIN */
    int h = b.h < HMI_LAYOUT_BUTTON_H ? b.h : HMI_LAYOUT_BUTTON_H;
    hmi_rect_t bb = { b.x, b.y + (b.h - h) / 2, b.w, h };
    button(r, bb, label, st);
}

/* Range of a zoomed trend: the last 60 s with a margin, at least 5 % of the full scale (so
 * noise is not blown up), inside the full scale. false = no samples: use the full scale. */
static bool trend_range(const hmi_tag_meta_t *m, const int16_t *s, uint8_t n, int *lo, int *hi)
{
    int a = m->max_val, z = m->min_val;
    for (int i = 0; i < n; i++) {
        if (s[i] == HMI_TREND_GAP) continue;
        int sv = s[i] < m->min_val ? m->min_val : s[i] > m->max_val ? m->max_val : s[i];
        if (sv < a) a = sv;
        if (sv > z) z = sv;
    }
    if (z < a) return false;
    int full = m->max_val - m->min_val, min_span = full / 20 > 2 ? full / 20 : 2;
    int pad = (z - a) / 5;
    if (z - a + 2 * pad < min_span) pad = (min_span - (z - a) + 1) / 2;
    *lo = a - pad; *hi = z + pad;
    if (*lo < m->min_val) { *hi += m->min_val - *lo; *lo = m->min_val; }
    if (*hi > m->max_val) { *lo -= *hi - m->max_val; *hi = m->max_val; }
    if (*lo < m->min_val) *lo = m->min_val;
    if (*hi <= *lo) *hi = *lo + 1;
    return true;
}

/* A 60 s trend. zoom: scaled to the recent values (tiles), with the scale written at the top right;
 * otherwise the full scale of the reading (device widgets). */
static void trend_draw(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag, uint8_t slot, bool zoom)
{
    if (!band_hits(r, b.y, b.h)) return;
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!m || m->max_val <= m->min_val) return;
    int16_t samples[HMI_TREND_SAMPLES];
    uint8_t n = v->tags ? hmi_tags_copy_trend(v->tags, slot, samples, HMI_TREND_SAMPLES) : 0;
    int lo = m->min_val, hi = m->max_val;
    bool zoomed = zoom && trend_range(m, samples, n, &lo, &hi) && (lo != m->min_val || hi != m->max_val);
    hmi_draw_text(r, &hmi_font_caption, b.x, b.y, "LAST 60 SECONDS", C_MUTED);
    if (zoomed) {                                            /* the scale shown, never a bare zoom */
        char a[16], z[16], txt[48];
        hmi_format_scaled(a, sizeof(a), (int16_t)lo, m->decimals);
        hmi_format_scaled(z, sizeof(z), (int16_t)hi, m->decimals);
        snprintf(txt, sizeof(txt), "%s to %s %s", a, z, m->unit ? m->unit : "");
        int cw = hmi_text_width(&hmi_font_caption, "LAST 60 SECONDS") + 12;
        if (hmi_text_width(&hmi_font_caption, txt) <= b.w - cw)
            text_right(r, &hmi_font_caption, b.x + b.w, b.y, txt, C_MUTED);
    }

    hmi_rect_t p = { b.x, b.y + 20, b.w, b.h - 20 };
    if (p.h < 12) return;
    for (int i = 0; i <= 2; i++) hmi_draw_rect(r, p.x, p.y + (p.h - 1) * i / 2, p.w, 1, C_LINE);

    int span = hi - lo;
    #define TREND_Y(val) (p.y + p.h - 2 - (int)(((long)((val) - lo) * (p.h - 4)) / span))
    /* dashed limit lines, where they fall inside the scale */
    int16_t lim[2] = { m->warn_high, m->trip_high };
    uint8_t lc[2] = { C_ORANGE, C_RED };
    for (int k = 0; k < 2; k++) {
        if (lim[k] <= lo || lim[k] >= hi || lim[k] <= m->min_val) continue;
        int ly = TREND_Y(lim[k]);
        for (int x = p.x; x < p.x + p.w; x += 8) hmi_draw_rect(r, x, ly, 4, 1, lc[k]);
    }

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
        if (sv < lo) sv = (int16_t)lo;
        if (sv > hi) sv = (int16_t)hi;
        int x = p.x + ((first + i) * (p.w - 1)) / (HMI_TREND_SAMPLES - 1);
        int y = TREND_Y(sv);
        if (have_prev) hmi_draw_line(r, px_prev, py_prev, x, y, 2, v->data_stale ? C_DIM : C_TEXT);
        px_prev = x; py_prev = y;
        have_prev = true;
    }
    reset_clip(r);
    if (have_prev) hmi_draw_circle(r, px_prev, py_prev, 4, v->data_stale ? C_DIM : C_TEXT);
    #undef TREND_Y
}

static void w_trend(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag, uint8_t slot)
{
    trend_draw(r, b, v, tag, slot, false);
}

static void w_tank(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag)
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
        uint8_t fc = sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_NEUTRAL;
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

static void w_pump(hmi_renderer_t *r, hmi_rect_t b, const hmi_view_t *v, hmi_tag_id_t tag)
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
    uint8_t col = !ok ? C_DIM : run ? C_OK : C_MUTED;
    int phase = run ? (int)((v->now_ms / 25u) & 31u) : 0;
    for (int k = 0; k < 3; k++) {
        int a = phase + k * 11;            /* ~120 degrees apart in 32 steps */
        int ex = cx + (icos(a) * (rad - 6)) / 256;
        int ey = cy + (isin(a) * (rad - 6)) / 256;
        hmi_draw_line(r, cx, cy, ex, ey, 4, col);
    }
    hmi_draw_circle(r, cx, cy, 5, col);
}

/* Card that stands for the whole device (manifest tag HMI_REL_DEVICE) */
static void w_device_card(hmi_renderer_t *r, hmi_rect_t b, const hmi_device_info_t *dev)
{
    if (!band_hits(r, b.y, b.h)) return;
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 14, C_CARD, C_LINE);
    char t[64];
    if (dev->product[0]) snprintf(t, sizeof(t), "%s", dev->product);
    else snprintf(t, sizeof(t), "%s registers", dev->profile ? dev->profile->name : "Device");
    text_fit(r, &hmi_font_title, b.x + 16, b.y + 12, b.w - 32, t, C_TEXT);
}

/* Draw a manifest. dev == NULL: token tags are absolute (fixed screens).
 * dev != NULL: token tags are relative to the device's tag block. */
static void render_manifest(hmi_renderer_t *r, const hmi_view_t *v, const uint32_t *tok, uint8_t n,
                            hmi_rect_t content, const hmi_device_info_t *dev)
{
    if (!tok) return;
    int hero_font = 0;                       /* one hero size per manifest, set by the tightest widget */
    for (uint8_t i = 0; i < n; i++) {
        if (hmi_token_get_type(tok[i]) != WIDGET_HERO_DIGIT) continue;
        hmi_tag_id_t t = hmi_token_get_tag(tok[i]);
        if (dev) t = HMI_DEV_TAG(dev->slot, t);
        int f = hero_fit(hmi_layout_token_rect(tok[i], content), v, t);
        if (f > hero_font) hero_font = f;
    }
    for (uint8_t i = 0; i < n; i++) {
        hmi_rect_t b = hmi_layout_token_rect(tok[i], content);
        if (!band_hits(r, b.y - 4, b.h + 8)) continue;    /* bounding-box culling per band */
        uint8_t rel = hmi_token_get_tag(tok[i]);
        uint8_t type = hmi_token_get_type(tok[i]);
        if (dev && rel == HMI_REL_DEVICE) {
            if (type == WIDGET_CONTAINER_CARD) w_device_card(r, b, dev);
            continue;
        }
        if (dev && rel >= HMI_DEVICE_TAG_SPAN) continue;
        hmi_tag_id_t tag = dev ? HMI_DEV_TAG(dev->slot, rel) : rel;
        if (tag >= HMI_MAX_TAGS) continue;
        switch (type) {
            case WIDGET_CONTAINER_CARD: w_card(r, b, v, tag);   break;
            case WIDGET_HERO_DIGIT:     w_hero(r, b, v, tag, hero_font); break;
            case WIDGET_ANALOG_BAR:     w_bar(r, b, v, tag);    break;
            case WIDGET_STATUS_PILL:    w_pill(r, b, v, tag, dev, rel);   break;
            case WIDGET_ACTION_BUTTON:  w_button(r, b, v, tag, dev, rel); break;
            case WIDGET_TREND_SPARK:    w_trend(r, b, v, tag, dev ? dev->slot : 0); break;
            case WIDGET_TANK:           w_tank(r, b, v, tag);   break;
            case WIDGET_PUMP:           w_pump(r, b, v, tag);   break;
            default: break;
        }
    }
}

/* ========================================================================= */
/* Plug-and-play dashboard                                                    */
/* ========================================================================= */

static void device_status(const hmi_device_info_t *d, char *out, size_t cap, uint8_t *col)
{
    switch (d->state) {
        case HMI_DEV_ONLINE:     snprintf(out, cap, d->transport == HMI_TRANSPORT_LOOPBACK ? "Loopback" : "Online"); *col = C_OK; break;
        case HMI_DEV_CONNECTING: snprintf(out, cap, "Connecting"); *col = C_ORANGE; break;
        case HMI_DEV_OFFLINE:
            if (d->ident == HMI_IDENT_MISMATCH) snprintf(out, cap, "Other device - blocked");
            else if (d->remove_in_ms) snprintf(out, cap, "Offline - removing in %lu s", (unsigned long)((d->remove_in_ms + 999u) / 1000u));
            else snprintf(out, cap, "Offline - reconnecting");
            *col = C_RED;
            break;
        default: snprintf(out, cap, "Empty"); *col = C_DIM; break;
    }
}

static void device_header(hmi_renderer_t *r, hmi_rect_t p, const hmi_device_info_t *d)
{
    if (!band_hits(r, p.y, HMI_LAYOUT_DEVICE_HEADER_H)) return;
    char st[40], sub[80];
    uint8_t col;
    device_status(d, st, sizeof(st), &col);
    const char *open = "Details >";
    int ow = hmi_text_width(&hmi_font_body_b, open);
    text_right(r, &hmi_font_body_b, p.x + p.w - 4, p.y + 7, open, C_TEXT);   /* tap the header: faceplate */
    int stw = hmi_text_width(&hmi_font_body_b, st);
    int right = p.x + p.w - 4 - ow - 14;
    text_right(r, &hmi_font_body_b, right, p.y + 7, st, col);

    hmi_draw_circle(r, p.x + 7, p.y + 15, 5, col);
    int x = hmi_draw_text(r, &hmi_font_title, p.x + 20, p.y + 5, d->label, C_TEXT);
    if (d->product[0]) snprintf(sub, sizeof(sub), "%s  -  %s", d->endpoint, d->product);
    else snprintf(sub, sizeof(sub), "%s", d->endpoint);
    text_fit(r, &hmi_font_caption, x + 10, p.y + 9, right - stw - 16 - (x + 10), sub, C_MUTED);
}

static void render_empty_dashboard(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t c)
{
    if (!band_hits(r, c.y, c.h)) return;
    int cx = c.x + c.w / 2, cy = c.y + c.h / 2 - 30;
    /* radar: rings + a sweep that turns with time */
    for (int k = 3; k >= 1; k--) {
        hmi_draw_circle(r, cx, cy, 14 * k + 1, C_LINE);
        hmi_draw_circle(r, cx, cy, 14 * k - 1, C_BG);
    }
    int a = (int)((v->now_ms / 60u) & 31u);
    hmi_draw_line(r, cx, cy, cx + (icos(a) * 40) / 256, cy + (isin(a) * 40) / 256, 3, C_MUTED);
    hmi_draw_circle(r, cx, cy, 4, C_MUTED);

    hmi_rect_t t1 = { c.x, cy + 54, c.w, 28 };
    text_center(r, &hmi_font_headline, t1, "Searching for PLCs", C_TEXT);
    char line[96];
    if (v->disc && v->disc->sweeps)
        snprintf(line, sizeof(line), "Checking %lu address(es) every %u s - plug a PLC into the network",
                 (unsigned long)v->disc->targets, (unsigned)(HMI_DISCOVERY_PERIOD_MS / 1000u));
    else
        snprintf(line, sizeof(line), "Plug a PLC into the network; it appears here automatically");
    hmi_rect_t t2 = { c.x, cy + 84, c.w, 18 };
    text_center(r, &hmi_font_body, t2, line, C_MUTED);
    hmi_rect_t t3 = { c.x, cy + 104, c.w, 16 };
    char drv[96];
    size_t n = (size_t)snprintf(drv, sizeof(drv), "Driver modules:");
    for (uint8_t i = 0; i < hmi_profile_count() && n < sizeof(drv); i++) {
        const hmi_device_profile_t *p = hmi_profile_at(i);
        if (!p->auto_attach) continue;                      /* only modules that attach on their own */
        int k = snprintf(drv + n, sizeof(drv) - n, "%s %s", n > 16 ? "," : "", p->name);
        if (k > 0) n += (size_t)k;
    }
    text_center(r, &hmi_font_caption, t3, drv, C_DIM);
}

/* The device's main value: the tag its module trends, else its first analog reading. */
static hmi_tag_id_t main_tag(const hmi_device_info_t *dev)
{
    const hmi_device_profile_t *pf = dev->profile;
    uint8_t rel = pf->trend_tag;
    if (rel == HMI_REL_NONE || rel >= HMI_DEVICE_TAG_SPAN) {
        rel = 0;
        for (uint8_t i = 0; i < pf->meta_count; i++) if (pf->meta[i].type == TAG_TYPE_ANALOG) { rel = (uint8_t)pf->meta[i].tag_id; break; }
    }
    return HMI_DEV_TAG(dev->slot, rel);
}

/* Run state for a summary: the stop latch, else the first start/stop control. NULL = none. */
static const char *run_state(const hmi_view_t *v, const hmi_device_info_t *dev, uint8_t *col)
{
    const hmi_device_profile_t *pf = dev->profile;
    *col = C_MUTED;
    if (pf->estop_tag != HMI_REL_NONE && hmi_tag_get_meta(HMI_DEV_TAG(dev->slot, pf->estop_tag)) &&
        v->tags && v->tags->tags[HMI_DEV_TAG(dev->slot, pf->estop_tag)].value) { *col = C_RED; return "Stop latch"; }
    for (uint8_t k = 0; k < pf->n_actions; k++) {
        if (!hmi_act_is_stop(pf->actions[k].kind)) continue;
        int16_t rv; uint8_t rq;
        if (tag_value(v, HMI_DEV_TAG(dev->slot, pf->actions[k].rel_tag), &rv, &rq) && !v->data_stale) {
            *col = rv ? C_OK : C_MUTED;
            return rv ? "Running" : "Stopped";
        }
    }
    return NULL;
}

/* Alarms of one device: how many points are not NORMAL, and the highest priority among them. */
static int device_alarms(const hmi_view_t *v, uint8_t slot, uint8_t *worst)
{
    *worst = ALARM_SEV_NONE;
    if (!v->alarms) return 0;
    int n = 0;
    for (int rel = 0; rel < HMI_DEVICE_TAG_SPAN; rel++) {
        const hmi_alarm_point_t *pt = &v->alarms->points[HMI_DEV_TAG(slot, rel)];
        if (pt->state == ALARM_PT_NORMAL) continue;
        n++;
        if (pt->peak_severity > *worst) *worst = pt->peak_severity;
    }
    return n;
}

/* A small 60 s trend of one tag (overview tiles): grey line, newest sample at the right */
static void sparkline(hmi_renderer_t *r, hmi_rect_t p, const hmi_view_t *v, hmi_tag_id_t tag, uint8_t slot)
{
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    if (!m || m->max_val <= m->min_val || p.w < 20 || p.h < 8 || !v->tags) return;
    int16_t samples[HMI_TREND_SAMPLES];
    uint8_t n = hmi_tags_copy_trend(v->tags, slot, samples, HMI_TREND_SAMPLES);
    hmi_draw_rect(r, p.x, p.y + p.h - 1, p.w, 1, C_LINE);
    if (n < 2) return;
    int lo = m->min_val, hi = m->max_val;
    trend_range(m, samples, n, &lo, &hi);                  /* the shape of the last 60 s */
    int span = hi - lo, first = HMI_TREND_SAMPLES - n;
    int px = 0, py = 0;
    bool have = false;
    for (int i = 0; i < n; i++) {
        int16_t sv = samples[i];
        if (sv == HMI_TREND_GAP) { have = false; continue; }
        if (sv < lo) sv = (int16_t)lo;
        if (sv > hi) sv = (int16_t)hi;
        int x = p.x + ((first + i) * (p.w - 1)) / (HMI_TREND_SAMPLES - 1);
        int y = p.y + p.h - 2 - (int)(((long)(sv - lo) * (p.h - 3)) / span);
        if (have) hmi_draw_line(r, px, py, x, y, 1, v->data_stale ? C_DIM : C_NEUTRAL);
        px = x; py = y; have = true;
    }
    if (have) hmi_draw_circle(r, px, py, 2, v->data_stale ? C_DIM : C_TEXT);
}

/* Overview tile (many devices): status dot and name, alarm count with its priority symbol, the
 * main value with its 60 s trend, and the run state. The whole tile opens the device view. */
static void render_device_tile(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t p, const hmi_device_info_t *dev)
{
    if (!dev->profile) return;
    hmi_draw_round_frame(r, p.x, p.y, p.w, p.h, 12, C_CARD, C_LINE);
    char st[40], name[48], val[24], line[48];
    uint8_t col;
    device_status(dev, st, sizeof(st), &col);
    int x = p.x + 12, right = p.x + p.w - 12;

    uint8_t worst;
    int nal = device_alarms(v, dev->slot, &worst);
    int rw = 0;
    if (nal) {
        snprintf(line, sizeof(line), "%d", nal);
        int nw = hmi_text_width(&hmi_font_body_b, line);
        text_right(r, &hmi_font_body_b, right, p.y + 10, line, C_TEXT);
        sev_icon(r, right - nw - 12, p.y + 19, 8, worst, C_WHITE);
        rw = nw + 26;
    }
    hmi_draw_circle(r, x + 4, p.y + 19, 4, col);
    text_fit(r, &hmi_font_body_b, x + 14, p.y + 10, right - rw - (x + 14), dev->label, C_TEXT);

    hmi_tag_id_t tag = main_tag(dev);
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    nice_name(name, sizeof(name), m ? m->name : "Value");
    fmt_value(val, sizeof(val), v, tag);
    int16_t raw; uint8_t q;
    bool ok = dev->state == HMI_DEV_ONLINE && tag_value(v, tag, &raw, &q) && !v->data_stale;
    uint8_t sev = point_sev(v, tag);
    uint8_t vc = !ok ? C_DIM : sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_TEXT;
    const hmi_font_t *vf = p.h >= 120 ? &hmi_font_headline : &hmi_font_title;
    int vy = p.y + p.h - 12 - vf->line_h;

    /* middle row: value name, and on the right the run state or why there is no data */
    const char *rs = NULL;
    uint8_t rc = C_MUTED;
    if (dev->state != HMI_DEV_ONLINE) {
        rs = dev->ident == HMI_IDENT_MISMATCH ? "Other device" : dev->state == HMI_DEV_OFFLINE ? "Offline" : "Connecting";
        rc = col;
    }
    else rs = run_state(v, dev, &rc);
    int my = vy - hmi_font_body_b.line_h - 2;
    int sw_ = 0;
    if (rs) {
        sw_ = hmi_text_width(&hmi_font_body_b, rs) + 14;
        hmi_draw_circle(r, right - sw_ + 4, my + 9, 4, rc);
        hmi_draw_text(r, &hmi_font_body_b, right - sw_ + 14, my, rs, C_TEXT);
        sw_ += 8;
    }
    text_fit(r, &hmi_font_caption, x, my + 2, right - sw_ - x, name, C_MUTED);

    /* a tall tile uses its middle: the 60 s trend of the main value, and more readings as bars */
    hmi_rect_t mid = { x, p.y + 40, right - x, my - 10 - (p.y + 40) };
    bool big_trend = mid.h >= 70 && mid.w >= 120;
    if (big_trend) {
        const hmi_device_profile_t *pf = dev->profile;
        int nb = mid.h >= 120 ? (mid.h - 80) / 40 : 0;     /* the chart keeps at least 80 px */
        if (nb > 3) nb = 3;
        int used = 0;
        for (uint8_t i = 0; i < pf->meta_count && used < nb; i++) {
            if (pf->meta[i].type != TAG_TYPE_ANALOG) continue;
            hmi_tag_id_t bt = HMI_DEV_TAG(dev->slot, pf->meta[i].tag_id);
            if (bt == tag || !hmi_tag_get_meta(bt)) continue;
            used++;
            hmi_rect_t bb = { mid.x, mid.y + mid.h - used * 40 + 4, mid.w, 32 };
            w_bar(r, bb, v, bt);
        }
        hmi_rect_t tr = { mid.x, mid.y, mid.w, mid.h - used * 40 };
        trend_draw(r, tr, v, tag, dev->slot, true);
    }
    /* bottom row: the value, and its last 60 s on the right when the tile is wide enough */
    snprintf(line, sizeof(line), "%s %s", val, (m && m->unit) ? m->unit : "");
    int spark_w = big_trend ? 0 : p.w >= 260 ? p.w / 3 : p.w >= 170 ? 48 : 0;
    text_fit(r, vf, x, vy, right - x - (spark_w ? spark_w + 8 : 0), line, vc);
    if (spark_w) {
        hmi_rect_t sp = { right - spark_w, vy + 2, spark_w, vf->line_h - 4 };
        sparkline(r, sp, v, tag, dev->slot);
    }
}

/* Summary card for a panel too small for the device's widgets: the main value, its state and a
 * clear way in. Nothing is drawn over anything else, whatever the number of devices. */
static void render_device_summary(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t p, const hmi_device_info_t *dev)
{
    hmi_rect_t b = hmi_layout_device_body(p);
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h - 4, 14, C_CARD, C_LINE);
    const hmi_device_profile_t *pf = dev->profile;
    if (!pf) return;
    hmi_tag_id_t tag = main_tag(dev);
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    char name[48], val[24], line[48];
    nice_name(name, sizeof(name), m ? m->name : "Value");
    fmt_value(val, sizeof(val), v, tag);
    int16_t raw; uint8_t q;
    bool ok = tag_value(v, tag, &raw, &q) && !v->data_stale;
    uint8_t sev = point_sev(v, tag);
    uint8_t vc = !ok ? C_DIM : sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_TEXT;
    int x = b.x + 16, y = b.y + 8, right = b.x + b.w - 16;
    int inner_h = b.h - 4;
    const hmi_font_t *vf = inner_h >= 70 ? &hmi_font_headline : &hmi_font_title;

    /* right: what to do next */
    const char *go = "Details >";
    text_right(r, &hmi_font_body_b, right, b.y + (inner_h - hmi_font_body_b.line_h) / 2, go, C_TEXT);
    int rw = hmi_text_width(&hmi_font_body_b, go) + 24;

    /* run state of the first start/stop control, if the device has one */
    uint8_t sc;
    const char *st = run_state(v, dev, &sc);
    if (st) {
        int sw_ = hmi_text_width(&hmi_font_body_b, st);
        int sx = right - rw - sw_;
        hmi_draw_circle(r, sx - 10, b.y + inner_h / 2, 4, sc);
        hmi_draw_text(r, &hmi_font_body_b, sx, b.y + (inner_h - hmi_font_body_b.line_h) / 2, st, C_TEXT);
        rw += sw_ + 26;
    }
    /* left: the main value, never covered */
    text_fit(r, &hmi_font_caption, x, y, right - rw - x, name, C_MUTED);
    snprintf(line, sizeof(line), "%s %s", val, (m && m->unit) ? m->unit : "");
    text_fit(r, vf, x, y + hmi_font_caption.line_h + 2, right - rw - x, line, vc);
}

static void render_devices(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t c)
{
    if (!v->devices || v->ndev == 0) { render_empty_dashboard(r, v, c); return; }
    if (v->focus_dev) {                          /* device view: one device with all its graphics */
        const hmi_device_info_t *dev = v->focus_dev;
        hmi_rect_t p = hmi_layout_focus_panel(c);
        if (band_hits(r, p.y, p.h)) {
            device_header(r, p, dev);
            hmi_rect_t body = hmi_layout_device_body(p);
            uint8_t nt = 0;
            const uint32_t *tk = hmi_layout_device_manifest(dev->profile, body, &nt);
            render_manifest(r, v, tk, nt, body, dev);
        }
        button(r, hmi_layout_focus_button(c, 0), "< All devices", BTN_PLAIN);
        button(r, hmi_layout_focus_button(c, 1), "Controls >", BTN_PRIMARY);
        return;
    }
    int per = hmi_layout_devices_per_page(c, v->ndev);
    int pages = per > 0 ? (v->ndev + per - 1) / per : 1;
    int page = v->device_page < pages ? v->device_page : pages - 1;
    bool tiles = hmi_layout_device_tiles(c, v->ndev);
    if (pages > 1) {                         /* more devices than one screen holds: Previous / Next */
        hmi_rect_t pv = hmi_layout_device_pager(c, 0), nx = hmi_layout_device_pager(c, 1);
        if (band_hits(r, pv.y, pv.h)) {
            /* the page buttons say when a device on another page is in alarm (never hidden) */
            int before = 0, after = 0;
            for (int k = 0; k < v->ndev; k++) {
                uint8_t w;
                if (k / per == page || !device_alarms(v, v->devices[k].slot, &w)) continue;
                if (k / per < page) before++; else after++;
            }
            char t[96];
            snprintf(t, sizeof(t), before ? "< Previous (%d in alarm)" : "< Previous", before);
            button(r, pv, t, page > 0 ? BTN_PLAIN : BTN_DISABLED);
            snprintf(t, sizeof(t), after ? "Next (%d in alarm) >" : "Next >", after);
            button(r, nx, t, page + 1 < pages ? BTN_PLAIN : BTN_DISABLED);
            int last = (page + 1) * per < v->ndev ? (page + 1) * per : v->ndev;
            snprintf(t, sizeof(t), "Devices %d-%d of %u  -  page %d of %d", page * per + 1, last,
                     (unsigned)v->ndev, page + 1, pages);
            hmi_rect_t lab = { pv.x + pv.w, pv.y, nx.x - (pv.x + pv.w), pv.h };
            text_center(r, &hmi_font_body, lab, t, C_MUTED);
        }
    }
    for (int k = 0; k < per && page * per + k < v->ndev; k++) {
        const hmi_device_info_t *dev = &v->devices[page * per + k];
        hmi_rect_t p = hmi_layout_device_panel(c, v->ndev, k);
        if (!band_hits(r, p.y, p.h)) continue;
        if (tiles) { render_device_tile(r, v, p, dev); continue; }
        device_header(r, p, dev);
        if (hmi_layout_device_summary(p)) { render_device_summary(r, v, p, dev); continue; }
        hmi_rect_t body = hmi_layout_device_body(p);
        uint8_t nt = 0;
        const uint32_t *tk = hmi_layout_device_manifest(dev->profile, body, &nt);
        render_manifest(r, v, tk, nt, body, dev);
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
    hmi_draw_round_rect(r, 16, 13, 30, 30, 9, C_SURF);
    hmi_draw_rect(r, 23, 26, 4, 11, C_TEXT);
    hmi_draw_rect(r, 29, 21, 4, 16, C_TEXT);
    hmi_draw_rect(r, 35, 17, 4, 20, C_TEXT);

    hmi_rect_t theme = hmi_layout_theme_button(sw, sh);
    int avail = theme.x - 8 - 56;
    text_fit(r, &hmi_font_title, 56, 9, avail, "Prometheus HMI", C_TEXT);
    char sub[96];
    unsigned online = 0, offline = 0;
    for (uint8_t i = 0; v->devices && i < v->ndev; i++) {
        if (v->devices[i].state == HMI_DEV_ONLINE) online++;
        else if (v->devices[i].state == HMI_DEV_OFFLINE) offline++;
    }
    if (!v->ndev) snprintf(sub, sizeof(sub), "Searching for PLCs...");
    else if (offline) snprintf(sub, sizeof(sub), "%u online, %u offline", online, offline);
    else snprintf(sub, sizeof(sub), "%u device%s online", online, online == 1 ? "" : "s");
    text_fit(r, &hmi_font_caption, 56, 30, avail, sub, C_MUTED);

    /* Theme toggle (moon) */
    int tcx = theme.x + theme.w / 2, tcy = theme.y + theme.h / 2;
    hmi_draw_circle(r, tcx, tcy, 16, C_LINE);
    hmi_draw_circle(r, tcx, tcy, 15, C_SURF);
    hmi_draw_circle(r, tcx, tcy, 7, C_MUTED);
    hmi_draw_circle(r, tcx + 4, tcy - 3, 6, C_SURF);

    /* System health */
    const char *ht = "HMI OK";                 /* the HMI's own health - not the plant's */
    uint8_t hc = C_OK;
    if (v->data_stale)                          { ht = "Data stale";     hc = C_RED; }
    else if (v->wd && !v->wd->fieldbus_alive)   { ht = "Watchdog trip";  hc = C_RED; }
    else if (!v->ui_wd_alive)                   { ht = "UI stalled";     hc = C_ORANGE; }
    else if (v->wd && v->wd->failover_active)   { ht = "Failover latch"; hc = C_ORANGE; }
    else {
        for (uint8_t i = 0; v->devices && i < v->ndev; i++)
            if (v->devices[i].state == HMI_DEV_OFFLINE) { ht = "PLC offline"; hc = C_RED; }
    }
    chip(r, hmi_layout_health_pill(sw, sh), hc, ht, C_TEXT);

    /* Alarm summary */
    char at[32];
    uint8_t ac = C_OK;
    if (v->alarms && v->alarms->unacked_count) {
        snprintf(at, sizeof(at), "%u unack.", (unsigned)v->alarms->unacked_count);
        ac = sev_color(v->alarms->highest_severity);
    } else if (v->alarms && v->alarms->active_count) {
        snprintf(at, sizeof(at), "%u active", (unsigned)v->alarms->active_count);
        ac = sev_color(v->alarms->highest_severity);
    } else {
        snprintf(at, sizeof(at), "No alarms");
    }
    {
        hmi_rect_t ap = hmi_layout_alarm_pill(sw, sh);
        uint8_t hs = (v->alarms && (v->alarms->unacked_count || v->alarms->active_count)) ? v->alarms->highest_severity : 0;
        if (hs) {                                           /* the priority symbol, not just a dot */
            hmi_draw_round_frame(r, ap.x, ap.y, ap.w, ap.h, ap.h / 2, C_SURF, ac);
            sev_icon(r, ap.x + 17, ap.y + ap.h / 2, 7, hs, C_WHITE);
            text_fit(r, &hmi_font_body_b, ap.x + 30, ap.y + (ap.h - hmi_font_body_b.line_h) / 2, ap.w - 38, at, C_TEXT);
        } else {
            chip(r, ap, ac, at, C_TEXT);
        }
    }

    /* Role */
    static const char * const roles[4] = { "Viewer", "Operator", "Maintenance", "Engineer" };
    int role = v->sec ? (int)v->sec->current_role : 0;
    if (role < 0 || role > 3) role = 0;
    chip(r, hmi_layout_role_pill(sw, sh), role ? C_TEXT : C_DIM, roles[role], C_TEXT);

    /* EMERGENCY: anyone may press it; it sends a software stop to every device and locks the HMI
     * on the red EMERGENCY screen. Deliberately not labelled E-STOP and not red-on-yellow: those
     * belong to the hardwired emergency stop at each machine (ISO 13850, IEC 60204-1). */
    hmi_rect_t e = hmi_layout_estop_button(sw, sh);
    hmi_draw_round_rect(r, e.x, e.y, e.w, e.h, 10, C_RED);
    emergency_label(r, e, C_WHITE);
}

static void render_banner(hmi_renderer_t *r, const hmi_view_t *v)
{
    if (!v->alarms || !hmi_layout_banner_shown(v->active_screen, v->alarms->current_action.active)) return;
    const hmi_emergency_action_t *a = &v->alarms->current_action;
    hmi_rect_t b = hmi_layout_banner(r->screen_w, r->screen_h);
    if (!band_hits(r, b.y, b.h)) return;
    uint8_t sc = sev_color(a->severity);
    bool unacked = (a->state == ALARM_PT_UNACK || a->state == ALARM_PT_RTN_UNACK);

    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 14, C_CARD, unacked ? sc : C_LINE);

    int cx = b.x + 32, cy = b.y + b.h / 2;
    sev_icon(r, cx, cy, 16, a->severity, C_WHITE);

    hmi_rect_t ack = hmi_layout_banner_ack(r->screen_w, r->screen_h);
    int tx = b.x + 58;
    int tw = ack.x - 16 - tx;
    char title[96], s1[64], s2[64], steps[136];
    nice_name(title, sizeof(title), a->title);
    nice_name(s1, sizeof(s1), a->action_step1);
    nice_name(s2, sizeof(s2), a->action_step2);
    text_fit(r, &hmi_font_title, tx, b.y + 9, tw, title, sc);
    (void)steps;
    text_fit(r, &hmi_font_body, tx, b.y + 35, tw, s1, C_TEXT);      /* each step on its own line, in full */
    text_fit(r, &hmi_font_body, tx, b.y + 56, tw, s2, C_TEXT);

    if (unacked) {
        button(r, ack, "Acknowledge", role_allows(v, HMI_CMD_ACK_ALARMS) ? BTN_PRIMARY : BTN_DISABLED);
    } else {
        text_center(r, &hmi_font_body_b, ack, "Acknowledged", C_MUTED);
    }
}

typedef enum { ICON_PROCESS, ICON_READINGS, ICON_DIAG, ICON_TELEMETRY, ICON_ALARM, ICON_SETTINGS } icon_t;

static void draw_icon(hmi_renderer_t *r, icon_t ic, int cx, int cy, uint8_t c, uint8_t bg)
{
    switch (ic) {
        case ICON_PROCESS:   /* tank with level */
            hmi_draw_round_frame(r, cx - 8, cy - 10, 16, 20, 5, bg, c);
            hmi_draw_round_frame(r, cx - 7, cy - 9, 14, 18, 4, bg, c);
            hmi_draw_round_rect(r, cx - 5, cy, 10, 7, 2, c);
            break;
        case ICON_READINGS:  /* list */
            for (int k = -1; k <= 1; k++) {
                hmi_draw_circle(r, cx - 8, cy + k * 7, 2, c);
                hmi_draw_round_rect(r, cx - 4, cy + k * 7 - 1, 15, 3, 1, c);
            }
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
    static const char * const labels[SCREEN_COUNT] = {
        [SCREEN_PROCESS_OVERVIEW] = "Process",     [SCREEN_READINGS] = "Readings",
        [SCREEN_ALARM_MATRIX] = "Alarms",          [SCREEN_FIELD_DIAGNOSTICS] = "Diagnostics",
        [SCREEN_HOST_TELEMETRY] = "Telemetry",     [SCREEN_SECURITY_SETTINGS] = "Settings" };
    static const icon_t icons[SCREEN_COUNT] = {
        [SCREEN_PROCESS_OVERVIEW] = ICON_PROCESS,  [SCREEN_READINGS] = ICON_READINGS,
        [SCREEN_ALARM_MATRIX] = ICON_ALARM,        [SCREEN_FIELD_DIAGNOSTICS] = ICON_DIAG,
        [SCREEN_HOST_TELEMETRY] = ICON_TELEMETRY,  [SCREEN_SECURITY_SETTINGS] = ICON_SETTINGS };
    for (int i = 0; i < SCREEN_COUNT; i++) {
        hmi_rect_t t = hmi_layout_dock_tab(sw, sh, i);
        bool on = (i == v->active_screen);
        uint8_t c = on ? C_TEXT : C_DIM;
        int cx = t.x + t.w / 2;
        if (on) hmi_draw_rect(r, t.x + t.w / 4, t.y + 1, t.w / 2, 3, C_TEXT);   /* where am I: a bar, not a colour */
        draw_icon(r, icons[i], cx, t.y + 20, c, C_CARD);
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
    char b1[64], b2[80];   /* sized for the longest formatted line (endpoint + three u32 counters) */
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
            uint8_t rc = (fb->loop_hz >= 950 && fb->loop_hz <= 1050) ? C_OK : C_ORANGE;
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
        if (rows-- > 0) { kv_row(r, x, y, w, "CRC errors", b1, v->crc_mismatches ? C_RED : C_OK); y += rh; }
        snprintf(b1, sizeof(b1), "%lu ms", (unsigned long)v->snapshot_age_ms);
        if (rows-- > 0) { kv_row(r, x, y, w, "Snapshot age", b1, v->data_stale ? C_RED : C_TEXT); y += rh; }
        if (v->latency_max_us)
            snprintf(b1, sizeof(b1), "%lu / %lu ms", (unsigned long)((v->latency_avg_us + 500u) / 1000u),
                     (unsigned long)((v->latency_max_us + 500u) / 1000u));
        else
            snprintf(b1, sizeof(b1), "-");
        if (rows-- > 0) {
            kv_row(r, x, y, w, "Latency avg/max", b1,
                   v->latency_max_us > HMI_LATENCY_TARGET_MS * 1000u ? C_ORANGE : C_TEXT);
            y += rh;
        }
        snprintf(b1, sizeof(b1), "%lu", (unsigned long)v->seqlock_retries);
        if (rows-- > 0) { kv_row(r, x, y, w, "Seqlock retries", b1, C_TEXT); }
    }

    /* Devices */
    card(r, c2, "Devices");
    if (band_hits(r, c2.y, c2.h)) {
        int x = c2.x + 16, w = c2.w - 32, y = c2.y + 42;
        if (!v->ndev) hmi_draw_text(r, &hmi_font_body, x, y, "No PLC connected", C_MUTED);
        uint8_t d = 0;
        for (; v->devices && d < v->ndev && y + 38 <= c2.y + c2.h - 24 - (d + 1 < v->ndev ? 20 : 0); d++) {
            const hmi_device_info_t *dv = &v->devices[d];
            char st[40];
            uint8_t col;
            device_status(dv, st, sizeof(st), &col);
            hmi_draw_circle(r, x + 4, y + 8, 4, col);
            text_fit(r, &hmi_font_body_b, x + 14, y, w - 14 - 70, dv->label, C_TEXT);
            if (dv->transport == HMI_TRANSPORT_TCP) {
                snprintf(b1, sizeof(b1), "%lu.%lu ms", (unsigned long)(dv->rtt_us / 1000u), (unsigned long)((dv->rtt_us / 100u) % 10u));
                text_right(r, &hmi_font_body_b, x + w, y, b1, C_TEXT);
                snprintf(b2, sizeof(b2), "%s  ok %lu/%lu  to %lu", dv->endpoint, (unsigned long)dv->responses,
                         (unsigned long)dv->requests, (unsigned long)dv->timeouts);
            } else {
                snprintf(b2, sizeof(b2), "%s", st);
            }
            text_fit(r, &hmi_font_caption, x + 14, y + 18, w - 14, b2, C_MUTED);
            y += 40;
        }
        if (v->devices && d < v->ndev) {         /* not all fit: say so rather than hide them */
            snprintf(b1, sizeof(b1), "+ %u more (all on the Process screen)", (unsigned)(v->ndev - d));
            text_fit(r, &hmi_font_caption, x, y, w, b1, C_MUTED);
        }
        if (v->disc && band_hits(r, c2.y + c2.h - 26, 20)) {
            snprintf(b1, sizeof(b1), "Discovery: %lu targets, sweep %lu ms",
                     (unsigned long)v->disc->targets, (unsigned long)v->disc->last_sweep_ms);
            text_fit(r, &hmi_font_caption, x, c2.y + c2.h - 24, w, b1, C_DIM);
        }
    }

    /* Register map of the first device: straight from its driver profile */
    const hmi_device_info_t *d0 = (v->devices && v->ndev) ? &v->devices[0] : NULL;
    card(r, c3, "Register map");
    if (band_hits(r, c3.y, c3.h)) {
        int x = c3.x + 16, w = c3.w - 32, y = c3.y + 44;
        int cx1 = x, cx2 = x + w * 20 / 100, cx3 = x + w * 74 / 100, cx4 = x + w;
        if (!d0 || !d0->profile) {
            hmi_draw_text(r, &hmi_font_body, x, y, "No PLC connected", C_MUTED);
        } else {
            hmi_draw_text(r, &hmi_font_caption, cx1, y, "ADDR", C_DIM);
            text_right(r, &hmi_font_caption, cx3 + w * 8 / 100, y, "VALUE", C_DIM);
            text_right(r, &hmi_font_caption, cx4, y, "RAW", C_DIM);
            y += 18;
            separator(r, x, y - 3, w);
            const hmi_device_profile_t *p = d0->profile;
            /* The name column starts after the widest address (%MW3201 is wider than %MW12) */
            int addr_w = 0;
            for (int blk = 0; blk < 2; blk++) {
                const hmi_block_map_t *bm = blk == 0 ? &p->regs : &p->coils;
                if (!bm->count) continue;
                snprintf(b1, sizeof(b1), blk == 0 ? "%%MW%u" : "%%M%u", (unsigned)(bm->start + bm->count - 1));
                int tw = hmi_text_width(&hmi_font_body, b1);
                if (tw > addr_w) addr_w = tw;
            }
            if (cx1 + addr_w + 12 > cx2) cx2 = cx1 + addr_w + 12;
            text_fit(r, &hmi_font_caption, cx2, y - 18, cx3 - cx2 - 36, d0->label, C_DIM);
            for (int blk = 0; blk < 2; blk++) {
                const hmi_block_map_t *bm = blk == 0 ? &p->regs : &p->coils;
                for (uint8_t i = 0; i < bm->count && y + 18 <= c3.y + c3.h - 8; i++) {
                    uint8_t rel = bm->rel_tags[i];
                    if (rel == HMI_REL_NONE) continue;
                    hmi_tag_id_t id = HMI_DEV_TAG(d0->slot, rel);
                    const hmi_tag_meta_t *m = hmi_tag_get_meta(id);
                    int16_t val; uint8_t q;
                    bool ok = tag_value(v, id, &val, &q);
                    snprintf(b1, sizeof(b1), blk == 0 ? "%%MW%u" : "%%M%u", (unsigned)(bm->start + i));
                    hmi_draw_text(r, &hmi_font_body, cx1, y, b1, C_MUTED);
                    text_fit(r, &hmi_font_body, cx2, y, cx3 - cx2 - 36, m ? m->name : "Raw word", m ? C_TEXT : C_MUTED);
                    fmt_value(b2, sizeof(b2), v, id);
                    text_right(r, &hmi_font_body_b, cx3 + w * 8 / 100, y, b2, ok ? C_TEXT : C_RED);
                    snprintf(b2, sizeof(b2), "%04X", (unsigned)(uint16_t)val);
                    text_right(r, &hmi_font_body, cx4, y, b2, ok ? C_MUTED : C_RED);
                    y += 20;
                }
            }
        }
    }
}

static void tab_button(hmi_renderer_t *r, hmi_rect_t b, const char *label, bool on)
{
    if (!band_hits(r, b.y, b.h)) return;
    hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 10, on ? C_SURF : C_CARD, on ? C_TEXT : C_LINE);
    text_center(r, &hmi_font_body_b, b, label, on ? C_TEXT : C_MUTED);
}

/* Prev / "Page n of m" / Next in the card footer; nothing when everything fits on one page. */
static void pager(hmi_renderer_t *r, hmi_rect_t card, int page, int pages)
{
    if (pages <= 1) return;
    hmi_rect_t pv = hmi_layout_alarm_page(card, 0), nx = hmi_layout_alarm_page(card, 1);
    if (!band_hits(r, pv.y, pv.h)) return;
    button(r, pv, "< Previous", page > 0 ? BTN_PLAIN : BTN_DISABLED);
    button(r, nx, "Next >", page + 1 < pages ? BTN_PLAIN : BTN_DISABLED);
    char t[32];
    snprintf(t, sizeof(t), "Page %d of %d", page + 1, pages);
    hmi_rect_t lab = { pv.x + pv.w, pv.y, nx.x - (pv.x + pv.w), pv.h };
    text_center(r, &hmi_font_body, lab, t, C_MUTED);
}

static void render_alarms(hmi_renderer_t *r, const hmi_view_t *v, bool banner)
{
    int sw = r->screen_w, sh = r->screen_h;
    const hmi_alarm_supervisor_t *al = v->alarms;
    char b1[96], b2[48], tm[24];

    /* Title row: the two views, then the actions; the most used action right-most */
    hmi_rect_t c = hmi_layout_content(sw, sh, banner);
    if (band_hits(r, c.y, HMI_LAYOUT_TITLE_H))
        hmi_draw_text(r, &hmi_font_headline, c.x + 2, c.y + 4, "Alarms", C_TEXT);
    /* The app sorts once per frame; this runs once per band, so only sort here without it */
    static hmi_tag_id_t own[HMI_MAX_TAGS];         /* only used when the app did not sort */
    const hmi_tag_id_t *order = v->alarm_order;
    uint16_t nact = v->alarm_order_n;
    if (!order) {
        nact = al ? hmi_alarms_sorted(al, own, HMI_MAX_TAGS) : 0;
        order = own;
    }
    tab_button(r, hmi_layout_alarm_tab(sw, sh, HMI_ALARM_VIEW_ACTIVE), "Active", v->alarm_view == HMI_ALARM_VIEW_ACTIVE);
    if (al && al->shelved_count) snprintf(b2, sizeof(b2), "Shelved %u", (unsigned)al->shelved_count);
    else snprintf(b2, sizeof(b2), "Shelved");
    tab_button(r, hmi_layout_alarm_tab(sw, sh, HMI_ALARM_VIEW_SHELVED), b2, v->alarm_view == HMI_ALARM_VIEW_SHELVED);
    tab_button(r, hmi_layout_alarm_tab(sw, sh, HMI_ALARM_VIEW_HISTORY), "History", v->alarm_view == HMI_ALARM_VIEW_HISTORY);

    bool can_ack = role_allows(v, HMI_CMD_ACK_ALARMS);
    bool estop = false;
    for (uint8_t d = 0; v->devices && v->tags && d < v->ndev; d++) {
        const hmi_device_profile_t *p = v->devices[d].profile;
        if (p && p->estop_tag != HMI_REL_NONE && v->tags->tags[HMI_DEV_TAG(v->devices[d].slot, p->estop_tag)].value) estop = true;
    }
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_ALARM_ACK), "Acknowledge all",
           (can_ack && al && al->unacked_count) ? BTN_PRIMARY : BTN_DISABLED);
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_ALARM_SILENCE), "Silence horn",
           (can_ack && al && al->horn_sounding) ? BTN_PLAIN : BTN_DISABLED);
    /* Resetting a latch is not a stop: neutral, never red (red means stop) */
    button(r, hmi_layout_screen_button(sw, sh, banner, HMI_HIT_ESTOP_RESET), "Reset stop",
           (estop && role_allows(v, HMI_CMD_ESTOP_RESET)) ? BTN_PLAIN : BTN_DISABLED);

    hmi_rect_t ca = hmi_layout_alarm_active_card(sw, sh, banner, nact);
    if (v->alarm_view == HMI_ALARM_VIEW_ACTIVE) {
        /* ISA-18.2: in a flood the operator is told so, and works from the top of the list */
        if (al && al->flood)
            snprintf(b1, sizeof(b1), "ALARM FLOOD - %u new in 10 min - work from the top", (unsigned)al->rate_10min);
        else
            snprintf(b1, sizeof(b1), "%u active - %u unacknowledged - %u new in 10 min", (unsigned)nact,
                     al ? (unsigned)al->unacked_count : 0u, al ? (unsigned)al->rate_10min : 0u);
        card(r, ca, NULL);
        if (band_hits(r, ca.y + 12, 20))
            text_fit(r, &hmi_font_title, ca.x + 16, ca.y + 12, ca.w - 32, b1, (al && al->flood) ? C_ORANGE : C_TEXT);
        int per = hmi_layout_alarm_rows(ca);
        int pages = nact ? (nact + per - 1) / per : 1;
        int page = v->alarm_page < pages ? v->alarm_page : pages - 1;
        if (band_hits(r, ca.y, ca.h)) {
            int x = ca.x + 16, w = ca.w - 32;
            int ack_x = hmi_layout_alarm_row_ack(ca, 0).x;
            int col_pri = x + 28, col_name = x + 128, col_time = ack_x - 14;
            int col_state = col_time - 84, col_val = col_state - 150;
            if (!nact) {
                int y = hmi_layout_alarm_row_y(ca, 0);
                hmi_draw_circle(r, x + 6, y + 9, 4, C_DIM);       /* ISA-101: normal is not coloured */
                hmi_draw_text(r, &hmi_font_body_b, x + 20, y, "No active alarms", C_MUTED);
            }
            for (int row = 0; row < per; row++) {
                int k = page * per + row;
                if (k >= nact) break;
                hmi_tag_id_t i = order[k];
                const hmi_alarm_point_t *pt = &al->points[i];
                int y = hmi_layout_alarm_row_y(ca, row);
                int ry = ca.y + 42 + row * HMI_LAYOUT_ALARM_ROW_H;
                bool unacked = pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_RTN_UNACK;
                if (row) hmi_draw_rect(r, x, ry, w, 1, C_LINE);
                const hmi_tag_meta_t *m = hmi_tag_get_meta(i);
                uint8_t sc = sev_color(pt->peak_severity);
                sev_icon(r, x + 9, y + 10, 9, pt->peak_severity, C_WHITE);
                hmi_draw_text(r, &hmi_font_body_b, col_pri, y, sev_label(pt->peak_severity), sc);
                if (m && m->group) snprintf(b1, sizeof(b1), "%s: %s", m->group, m->name);
                else nice_name(b1, sizeof(b1), m ? m->name : "Tag");
                bool analog = m && m->type == TAG_TYPE_ANALOG;
                text_fit(r, unacked ? &hmi_font_body_b : &hmi_font_body, col_name, y,
                         (analog ? col_val - 90 : col_state - 10) - col_name, b1, unacked ? C_TEXT : C_MUTED);
                if (analog) {
                    fmt_value(b2, sizeof(b2), v, i);
                    char t[72];
                    snprintf(t, sizeof(t), "%s %s", b2, m->unit ? m->unit : "");
                    text_right(r, &hmi_font_body_b, col_val, y, t, C_TEXT);
                }
                const char *stl = pt->state == ALARM_PT_RTN_UNACK ? "Cleared" : unacked ? "New" : "Acknowledged";
                if (!unacked && (pt->flags & HMI_ALARM_F_CHATTER)) stl = "Chattering";
                text_right(r, &hmi_font_body_b, col_state, y, stl, unacked ? sc : C_MUTED);
                fmt_clock(tm, sizeof(tm), v, pt->since_ms);
                text_right(r, &hmi_font_body, col_time, y, tm, C_MUTED);
                if (unacked) button(r, hmi_layout_alarm_row_ack(ca, row), "Ack", can_ack ? BTN_PLAIN : BTN_DISABLED);
            }
        }
        pager(r, ca, page, pages);
    } else if (v->alarm_view == HMI_ALARM_VIEW_SHELVED) {
        card(r, ca, "Shelved and out of service - not sounded, not in the list");
        uint16_t n = v->hidden ? v->hidden_n : 0;
        int per = hmi_layout_alarm_rows(ca);
        int pages = n ? (n + per - 1) / per : 1;
        int page = v->alarm_page < pages ? v->alarm_page : pages - 1;
        if (band_hits(r, ca.y, ca.h)) {
            int x = ca.x + 16, w = ca.w - 32;
            int ack_x = hmi_layout_alarm_row_ack(ca, 0).x;
            if (!n) {
                int y = hmi_layout_alarm_row_y(ca, 0);
                hmi_draw_text(r, &hmi_font_body_b, x, y, "No alarm is shelved or out of service", C_MUTED);
            }
            for (int row = 0; row < per; row++) {
                int k = page * per + row;
                if (k >= n) break;
                hmi_tag_id_t i = v->hidden[k];
                const hmi_alarm_point_t *pt = &al->points[i];
                int y = hmi_layout_alarm_row_y(ca, row);
                if (row) hmi_draw_rect(r, x, ca.y + 42 + row * HMI_LAYOUT_ALARM_ROW_H, w, 1, C_LINE);
                const hmi_tag_meta_t *m = hmi_tag_get_meta(i);
                if (pt->state != ALARM_PT_NORMAL) sev_icon(r, x + 9, y + 10, 9, pt->peak_severity, C_WHITE);
                else hmi_draw_circle(r, x + 9, y + 10, 4, C_DIM);
                if (m && m->group) snprintf(b1, sizeof(b1), "%s: %s", m->group, m->name);
                else nice_name(b1, sizeof(b1), m ? m->name : "Tag");
                bool shelved = (pt->flags & HMI_ALARM_F_SHELVED) != 0;
                char st[40];
                if (shelved) {
                    int32_t left = (int32_t)(pt->shelve_until_ms - v->now_ms);
                    snprintf(st, sizeof(st), "Shelved, %ld min left", (long)(left > 0 ? (left + 59999) / 60000 : 0));
                } else {
                    snprintf(st, sizeof(st), "Out of service");
                }
                int stw = hmi_text_width(&hmi_font_body_b, st);
                text_right(r, &hmi_font_body_b, ack_x - 14, y, st, C_MUTED);
                text_fit(r, &hmi_font_body, x + 28, y, ack_x - 14 - stw - 16 - (x + 28), b1, C_TEXT);
                bool may = role_allows(v, shelved ? HMI_CMD_SHELVE_ALARM : HMI_CMD_ALARM_SERVICE);
                button(r, hmi_layout_alarm_row_ack(ca, row), shelved ? "Unshelve" : "Return", may ? BTN_PLAIN : BTN_DISABLED);
            }
        }
        pager(r, ca, page, pages);
    } else {
        card(r, ca, "Event history - newest first");
        uint16_t n = al ? hmi_alarms_journal_count(al) : 0;
        int per = hmi_layout_history_rows(ca);
        int pages = n ? (n + per - 1) / per : 1;
        int page = v->alarm_page < pages ? v->alarm_page : pages - 1;
        if (band_hits(r, ca.y, ca.h)) {
            int jx = ca.x + 16, jw = ca.w - 32, y = ca.y + 44;
            if (!n) hmi_draw_text(r, &hmi_font_body, jx, y, "No events recorded", C_MUTED);
            for (int row = 0; row < per; row++) {
                int k = page * per + row;
                if (k >= n) break;
                const hmi_alarm_record_t *rec = hmi_alarms_journal_newest(al, (uint16_t)k);
                if (!rec) break;
                fmt_clock(tm, sizeof(tm), v, rec->timestamp_ms);
                hmi_draw_text(r, &hmi_font_body, jx, y, tm, C_MUTED);
                if (rec->severity >= ALARM_SEV_WARNING) sev_icon(r, jx + 112, y + 9, 6, rec->severity, C_WHITE);
                else hmi_draw_circle(r, jx + 112, y + 9, 3, C_DIM);
                int sx = jx + jw;
                const char *st = rec->tag_id == HMI_ALARM_TAG_SYSTEM ? "System" :
                                 rec->state == ALARM_PT_NORMAL ? "Event" : state_label(rec->state);
                int stw = hmi_text_width(&hmi_font_caption, st);
                hmi_draw_text(r, &hmi_font_caption, sx - stw, y + 2, st, C_DIM);
                nice_name(b1, sizeof(b1), rec->message);
                text_fit(r, &hmi_font_body, jx + 128, y, sx - stw - 12 - (jx + 128), b1,
                         rec->severity >= ALARM_SEV_WARNING ? C_TEXT : C_MUTED);
                y += HMI_LAYOUT_HISTORY_ROW_H;
            }
        }
        pager(r, ca, page, pages);
    }
}

/* ---- Readings ---- */

static const hmi_device_info_t *find_device(const hmi_view_t *v, uint8_t slot);

/* Where a value sits in its range (ISA-101 analog indicator): a grey track, the high warning and
 * trip limits as ticks, and a marker at the value. */
static void range_bar(hmi_renderer_t *r, int x, int cy, int w, const hmi_tag_meta_t *m, int16_t val, bool ok)
{
    if (!m || m->type != TAG_TYPE_ANALOG || m->max_val <= m->min_val || w < 24) return;
    int span = m->max_val - m->min_val;
    hmi_draw_round_rect(r, x, cy - 3, w, 6, 3, C_TRACK);
    if (m->warn_high > m->min_val && m->warn_high < m->max_val)
        hmi_draw_rect(r, x + (int)((int32_t)(m->warn_high - m->min_val) * w / span), cy - 6, 2, 12, C_ORANGE);
    if (m->trip_high > m->min_val && m->trip_high < m->max_val)
        hmi_draw_rect(r, x + (int)((int32_t)(m->trip_high - m->min_val) * w / span), cy - 6, 2, 12, C_RED);
    if (!ok) return;
    int32_t pos = (int32_t)(val - m->min_val) * w / span;
    if (pos < 0) pos = 0;
    if (pos > w - 3) pos = w - 3;
    hmi_draw_rect(r, x + (int)pos, cy - 7, 3, 14, C_TEXT);
}

static void read_heading(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t row, const hmi_read_row_t *it)
{
    hmi_draw_round_rect(r, row.x, row.y + 3, row.w, row.h - 6, 8, C_SURF);
    int ty = row.y + (row.h - hmi_font_title.line_h) / 2;
    int right = row.x + row.w - 12;
    char sub[96];
    if (it->slot == HMI_READ_SLOT_HMI) {
        hmi_draw_text(r, &hmi_font_title, row.x + 12, ty, "This HMI", C_TEXT);
        return;
    }
    const hmi_device_info_t *d = find_device(v, it->slot);
    if (!d) return;
    char st[40];
    uint8_t col;
    device_status(d, st, sizeof(st), &col);
    const char *open = "Open >";
    text_right(r, &hmi_font_body_b, right, row.y + (row.h - hmi_font_body_b.line_h) / 2, open, C_TEXT);
    int sx = right - hmi_text_width(&hmi_font_body_b, open) - 16;
    int stw = hmi_text_width(&hmi_font_body_b, st);
    text_right(r, &hmi_font_body_b, sx, row.y + (row.h - hmi_font_body_b.line_h) / 2, st, col);
    hmi_draw_circle(r, row.x + 14, row.y + row.h / 2, 4, col);
    int x = hmi_draw_text(r, &hmi_font_title, row.x + 26, ty, d->label, C_TEXT);
    if (it->kind == HMI_READ_ROW_CONTINUED) snprintf(sub, sizeof(sub), "(continued)  %s", d->endpoint);
    else if (d->product[0]) snprintf(sub, sizeof(sub), "%s  -  %s", d->endpoint, d->product);
    else snprintf(sub, sizeof(sub), "%s", d->endpoint);
    text_fit(r, &hmi_font_caption, x + 10, row.y + (row.h - hmi_font_caption.line_h) / 2,
             sx - stw - 16 - (x + 10), sub, C_MUTED);
}

static void read_tag_row(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t row, hmi_tag_id_t tag,
                         int col_val, int col_bar, int bar_w, int col_state)
{
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    char name[64], val[24], line[48];
    int ty = row.y + (row.h - hmi_font_body.line_h) / 2;
    hmi_draw_rect(r, row.x + 12, row.y + row.h - 1, row.w - 12, 1, C_LINE);
    const hmi_alarm_point_t *pt = v->alarms ? &v->alarms->points[tag] : NULL;
    bool in_alarm = pt && pt->state != ALARM_PT_NORMAL;
    if (in_alarm) sev_icon(r, row.x + 22, row.y + row.h / 2, 8, pt->peak_severity, C_WHITE);
    if (v->read_flat && m && m->group) {                        /* no headings: name the device */
        char nn[48];
        nice_name(nn, sizeof(nn), m->name);
        snprintf(name, sizeof(name), "%s: %s", m->group, nn);
    } else {
        nice_name(name, sizeof(name), m ? m->name : "Reading");
    }
    text_fit(r, in_alarm ? &hmi_font_body_b : &hmi_font_body, row.x + 40, ty, col_val - 110 - (row.x + 40), name,
             in_alarm ? C_TEXT : C_MUTED);

    int16_t raw; uint8_t q;
    bool ok = tag_value(v, tag, &raw, &q) && !v->data_stale;
    if (m && m->type == TAG_TYPE_DIGITAL) snprintf(val, sizeof(val), ok ? (raw ? "On" : "Off") : "--");
    else fmt_value(val, sizeof(val), v, tag);
    snprintf(line, sizeof(line), "%s %s", val, (m && m->unit && ok) ? m->unit : "");
    uint8_t sev = point_sev(v, tag);
    uint8_t vc = !ok ? C_DIM : sev >= ALARM_SEV_CRITICAL ? C_RED : sev == ALARM_SEV_WARNING ? C_ORANGE : C_TEXT;
    text_right(r, &hmi_font_body_b, col_val, ty, line, vc);
    range_bar(r, col_bar, row.y + row.h / 2, bar_w, m, raw, ok);

    const char *st = NULL;
    uint8_t sc = C_MUTED;
    if (!ok) { st = v->data_stale ? "Stale" : q == QUALITY_COMM_FAULT ? "No data" : "Bad data"; sc = C_ORANGE; }
    else if (in_alarm) {
        st = pt->state == ALARM_PT_RTN_UNACK ? "Cleared, unack." : pt->state == ALARM_PT_UNACK ? "New alarm" : "Acknowledged";
        sc = pt->state == ALARM_PT_ACKED ? C_MUTED : sev_color(pt->peak_severity);
    }
    if (st) text_right(r, &hmi_font_body_b, col_state, ty, st, sc);
}

static void render_readings(hmi_renderer_t *r, const hmi_view_t *v, bool banner)
{
    int sw = r->screen_w, sh = r->screen_h;
    char t[96];
    hmi_rect_t c = hmi_layout_content(sw, sh, banner);
    if (band_hits(r, c.y, HMI_LAYOUT_TITLE_H)) {
        hmi_draw_text(r, &hmi_font_headline, c.x + 2, c.y + 4, "Readings", C_TEXT);
        static const char * const fl[HMI_READ_FILTERS] = { "All", "In alarm", "Bad data" };
        for (int f = 0; f < HMI_READ_FILTERS; f++)
            tab_button(r, hmi_layout_read_filter(sw, sh, banner, f), fl[f], v->read_filter == f);
        const char *q = v->read_query ? v->read_query : "";
        if (q[0]) snprintf(t, sizeof(t), "Search: %s", q);
        else snprintf(t, sizeof(t), "Search...");
        button(r, hmi_layout_read_search(sw, sh, banner), t, q[0] ? BTN_PRIMARY : BTN_PLAIN);
    }

    hmi_rect_t ca = hmi_layout_read_card(sw, sh, banner);
    card(r, ca, NULL);
    if (!band_hits(r, ca.y, ca.h)) return;
    hmi_rect_t r0 = hmi_layout_read_row(ca, 0);
    int col_state = r0.x + r0.w - 12;
    int state_w = 128;
    int bar_w = r0.w >= 900 ? 160 : r0.w >= 700 ? 100 : 0;
    int col_val = col_state - state_w - 16 - bar_w - 16;
    if (col_val > r0.x + 640) col_val = r0.x + 640;              /* wide screens: value near its name */
    int col_bar = col_val + 16;
    if (band_hits(r, ca.y, HMI_LAYOUT_READ_HEAD_H)) {
        int hy = ca.y + (HMI_LAYOUT_READ_HEAD_H - hmi_font_caption.line_h) / 2 + 2;
        hmi_draw_text(r, &hmi_font_caption, r0.x + 40, hy, "READING", C_DIM);
        text_right(r, &hmi_font_caption, col_val, hy, "VALUE", C_DIM);
        if (bar_w) hmi_draw_text(r, &hmi_font_caption, col_bar, hy, "RANGE", C_DIM);
        text_right(r, &hmi_font_caption, col_state, hy, "STATUS", C_DIM);
    }
    if (!v->read_nrows) {
        hmi_rect_t e = { ca.x, ca.y + HMI_LAYOUT_READ_HEAD_H + 20, ca.w, 24 };
        text_center(r, &hmi_font_body_b, e,
                    v->read_total ? "No reading matches this filter or search" : "No readings yet: no device connected",
                    C_MUTED);
    }
    for (int k = 0; v->read_rows && k < v->read_nrows; k++) {
        hmi_rect_t row = hmi_layout_read_row(ca, k);
        if (!band_hits(r, row.y, row.h)) continue;
        const hmi_read_row_t *it = &v->read_rows[k];
        if (it->kind == HMI_READ_ROW_TAG) read_tag_row(r, v, row, it->tag, col_val, col_bar + 0, bar_w, col_state);
        else if (it->kind != HMI_READ_ROW_BLANK) read_heading(r, v, row, it);
    }

    /* footer: where am I, and the pages */
    hmi_rect_t pv = hmi_layout_read_page(ca, 0), nx = hmi_layout_read_page(ca, 1);
    if (band_hits(r, pv.y, pv.h)) {
        if (v->read_matches == v->read_total)
            snprintf(t, sizeof(t), "%u readings", (unsigned)v->read_total);
        else
            snprintf(t, sizeof(t), "%u of %u readings shown", (unsigned)v->read_matches, (unsigned)v->read_total);
        hmi_draw_text(r, &hmi_font_body, ca.x + 16, pv.y + (pv.h - hmi_font_body.line_h) / 2, t, C_MUTED);
        if (v->read_pages > 1) {
            button(r, pv, "< Previous", v->read_page > 0 ? BTN_PLAIN : BTN_DISABLED);
            button(r, nx, "Next >", v->read_page + 1 < v->read_pages ? BTN_PLAIN : BTN_DISABLED);
            snprintf(t, sizeof(t), "Page %u of %u", (unsigned)(v->read_page + 1), (unsigned)v->read_pages);
            hmi_rect_t lab = { pv.x + pv.w, pv.y, nx.x - (pv.x + pv.w), pv.h };
            text_center(r, &hmi_font_body, lab, t, C_MUTED);
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
        if (y + rh <= limit) { kv_row(r, x, y, w, "Current role", roles[role], C_TEXT); y += rh; }
        if (v->sec && v->sec->default_pins && y + rh <= limit) {
            kv_row(r, x, y, w, "PINs", v->sec->allow_factory_pins ? "Factory PINs - BENCH MODE"
                                                                   : "Factory PINs: roles locked", C_ORANGE); y += rh;
        }
        y += 4;
        static const struct { hmi_command_t c; const char *n; } perms[] = {
            { HMI_CMD_ESTOP, "EMERGENCY (stop all)" }, { HMI_CMD_EMERGENCY_RESET, "Reset EMERGENCY" }, { HMI_CMD_ACK_ALARMS, "Acknowledge alarms" },
            { HMI_CMD_PUMP_TOGGLE, "Operate pump / valve" }, { HMI_CMD_WATCHDOG_RESET, "Reset watchdog" },
            { HMI_CMD_INJECT_FAULT, "Fault injection" },
        };
        for (unsigned i = 0; i < sizeof(perms) / sizeof(perms[0]) && y + rh <= limit; i++) {
            bool ok = role_allows(v, perms[i].c);
            if (ok) snprintf(b1, sizeof(b1), "Allowed");
            else snprintf(b1, sizeof(b1), "Needs %s", roles[hmi_security_required_role(perms[i].c)]);
            kv_row(r, x, y, w, perms[i].n, b1, ok ? C_OK : C_DIM);
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
                                      (wd && wd->fieldbus_alive) ? C_OK : C_RED); y += rh; }
        if (y + rh <= limit) { kv_row(r, x, y, w, "UI watchdog", v->ui_wd_alive ? "Healthy" : "Tripped",
                                      v->ui_wd_alive ? C_OK : C_RED); y += rh; }
        if (y + rh <= limit) { kv_row(r, x, y, w, "Failover latch", (wd && wd->failover_active) ? "Latched" : "Clear",
                                      (wd && wd->failover_active) ? C_ORANGE : C_TEXT); y += rh; }
        if (y + rh <= limit) {
            kv_row(r, x, y, w, "Colour scheme", r->high_performance ? "High-performance" : "Status (green normal)", C_TEXT);
            y += rh;
        }
        if (y + rh <= limit) {
            bool signed_log = v->log && v->log->open && v->log->authenticated;
            if (v->log && v->log->open)
                snprintf(b1, sizeof(b1), "%lu records, %s", (unsigned long)v->log->records_written,
                         signed_log ? "signed" : "NOT signed");
            else snprintf(b1, sizeof(b1), "Off");
            kv_row(r, x, y, w, "Audit log", b1,
                   (v->log && (v->log->write_errors || (v->log->open && !signed_log))) ? C_RED : C_TEXT); y += rh;
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

static const hmi_device_info_t *find_device(const hmi_view_t *v, uint8_t slot)
{
    for (uint8_t d = 0; v->devices && d < v->ndev; d++) if (v->devices[d].slot == slot) return &v->devices[d];
    return NULL;
}

/* Current value of a device tag as operator text: "50.0 Hz", "Auto", "Running", "ACTIVE" */
static uint8_t device_value_text(const hmi_view_t *v, const hmi_device_info_t *d, uint8_t rel, char *out, size_t cap)
{
    hmi_tag_id_t tag = HMI_DEV_TAG(d->slot, rel);
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag);
    int16_t val; uint8_t q;
    if (!m || v->data_stale || !tag_value(v, tag, &val, &q)) { snprintf(out, cap, "--"); return C_DIM; }
    const hmi_device_action_t *e = hmi_profile_enum_action(d->profile, rel);
    if (e) {
        hmi_action_option_label(e, val >= 0 ? (uint8_t)val : 0xFFu, out, cap);
        if (!out[0]) snprintf(out, cap, "%d", (int)val);
        return C_TEXT;
    }
    if (m->type == TAG_TYPE_DIGITAL) {
        if (m->digital_alarm_sev) { snprintf(out, cap, "%s", val ? "ACTIVE" : "OK"); return val ? sev_color(m->digital_alarm_sev) : C_MUTED; }
        bool motor = false;
        for (uint8_t i = 0; i < d->profile->n_actions; i++)
            if (d->profile->actions[i].rel_tag == rel && hmi_act_is_start(d->profile->actions[i].kind)) motor = true;
        snprintf(out, cap, "%s", val ? (motor ? "Running" : "On") : (motor ? "Stopped" : "Off"));
        return val ? C_OK : C_MUTED;
    }
    char num[16];
    hmi_format_scaled(num, sizeof(num), val, m->decimals);
    snprintf(out, cap, "%s%s%s", num, (m->unit && m->unit[0]) ? " " : "", m->unit ? m->unit : "");
    uint8_t sev = point_sev(v, tag);
    return sev ? sev_color(sev) : C_TEXT;
}

static void render_faceplate(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t m)
{
    int sw = r->screen_w, sh = r->screen_h;
    const hmi_device_info_t *d = find_device(v, v->fp_slot);
    if (!d || !d->profile) return;
    const hmi_device_profile_t *p = d->profile;

    char st[40], sub[96];
    uint8_t col;
    device_status(d, st, sizeof(st), &col);
    hmi_rect_t close = hmi_layout_modal_cancel(sw, sh, HMI_MODAL_FACEPLATE);
    if (band_hits(r, m.y, 60)) {
        button(r, close, "Close", BTN_PLAIN);
        hmi_draw_circle(r, m.x + 22, m.y + 22, 6, col);
        int x = hmi_draw_text(r, &hmi_font_title, m.x + 36, m.y + 11, d->label, C_TEXT);
        text_fit(r, &hmi_font_body_b, x + 12, m.y + 13, close.x - 16 - (x + 12), st, col);
        snprintf(sub, sizeof(sub), "%s  -  %s", d->product[0] ? d->product : p->name, d->endpoint);
        text_fit(r, &hmi_font_caption, m.x + 18, m.y + 38, close.x - 8 - (m.x + 18), sub, C_MUTED);
    }

    /* values */
    hmi_rect_t vc = hmi_layout_fp_values(sw, sh);
    if (band_hits(r, vc.y, vc.h)) {
        hmi_draw_round_frame(r, vc.x, vc.y, vc.w, vc.h, 12, C_SURF, C_LINE);
        hmi_draw_text(r, &hmi_font_caption, vc.x + 12, vc.y + 8, "VALUES", C_MUTED);
        int n = 0;
        for (uint8_t i = 0; i < p->meta_count; i++) if (p->meta[i].tag_id != p->comm_tag) n++;
        int rh = n ? (vc.h - 34) / n : 24;
        if (rh > 26) rh = 26;
        int k = 0;
        for (uint8_t i = 0; i < p->meta_count; i++) {
            uint8_t rel = (uint8_t)p->meta[i].tag_id;
            if (rel == p->comm_tag) continue;
            int y = vc.y + 28 + k * rh;
            k++;
            if (!band_hits(r, y, rh)) continue;
            char name[40], val[32];
            nice_name(name, sizeof(name), p->meta[i].name);
            uint8_t c = device_value_text(v, d, rel, val, sizeof(val));
            int vw = hmi_text_width(&hmi_font_body_b, val);
            text_fit(r, &hmi_font_body, vc.x + 12, y + (rh - hmi_font_body.line_h) / 2, vc.w - 36 - vw, name, C_MUTED);
            text_right(r, &hmi_font_body_b, vc.x + vc.w - 12, y + (rh - hmi_font_body_b.line_h) / 2, val, c);
        }
    }

    /* controls */
    uint8_t rels[HMI_FP_MAX_ROWS];
    uint8_t nrows = hmi_profile_fp_rows(p, rels);
    bool online = d->state == HMI_DEV_ONLINE && !v->data_stale;
    for (uint8_t row = 0; row < nrows; row++) {
        hmi_rect_t rr = hmi_layout_fp_row(sw, sh, row);
        if (!band_hits(r, rr.y - 4, rr.h + 8)) continue;
        if (row) hmi_draw_rect(r, rr.x, rr.y - 3, rr.w, 1, C_LINE);
        const hmi_tag_meta_t *m0 = hmi_tag_get_meta(HMI_DEV_TAG(d->slot, rels[row]));
        char name[40], val[32];
        nice_name(name, sizeof(name), m0 ? m0->name : "Control");
        uint8_t c = device_value_text(v, d, rels[row], val, sizeof(val));
        int tw = rr.w * 36 / 100 - 8;
        text_fit(r, &hmi_font_body_b, rr.x, rr.y + rr.h / 2 - hmi_font_body_b.line_h + 1, tw, name, C_TEXT);
        text_fit(r, &hmi_font_caption, rr.x, rr.y + rr.h / 2 + 2, tw, val, c);
    }
    hmi_fp_ctrl_t cs[HMI_FP_MAX_CTRLS];
    uint8_t nc = hmi_profile_fp_controls(p, cs, HMI_FP_MAX_CTRLS);
    for (uint8_t k = 0; k < nc; k++) {
        hmi_rect_t br = hmi_layout_fp_ctrl(sw, sh, p, k);
        if (!band_hits(r, br.y, br.h)) continue;
        const hmi_device_action_t *a = &p->actions[cs[k].action];
        int16_t val; uint8_t q;
        bool ok = tag_value(v, HMI_DEV_TAG(d->slot, a->rel_tag), &val, &q) && online;
        char label[40];
        btn_style_t bs = control_look(v, d, a, cs[k].option, ok, val, label, sizeof(label));
        button(r, br, label, ok ? bs : BTN_DISABLED);
    }

    /* footer: who may operate */
    int fy = m.y + m.h - 22;
    if (band_hits(r, fy, 18)) {
        char foot[96];
        int role = v->sec ? (int)v->sec->current_role : 0;
        if (!online) snprintf(foot, sizeof(foot), "Device is not online: controls are disabled");
        else if (role == ROLE_VIEWER) snprintf(foot, sizeof(foot), "Signed in as Viewer: a control asks for the PIN of the role it needs");
        else snprintf(foot, sizeof(foot), "Signed in as %s  -  commands go to the PLC at once and are logged", s_role_names[role]);
        hmi_rect_t fr = { m.x + 16, fy, m.w - 32, 18 };
        text_center(r, &hmi_font_caption, fr, foot, C_MUTED);
    }
}

static void render_numpad(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t m)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_rect_t cancel = hmi_layout_modal_cancel(sw, sh, HMI_MODAL_NUMPAD);
    button(r, cancel, "Cancel", BTN_PLAIN);
    text_fit(r, &hmi_font_title, m.x + 18, m.y + 16, cancel.x - 8 - (m.x + 18), v->np_title ? v->np_title : "Enter value", C_TEXT);

    hmi_rect_t dsp = hmi_layout_numpad_display(sw, sh);
    if (band_hits(r, dsp.y, dsp.h)) {
        hmi_draw_round_frame(r, dsp.x, dsp.y, dsp.w, dsp.h, 10, C_SURF, C_TEXT);
        int ux = dsp.x + dsp.w - 12;
        if (v->np_unit && v->np_unit[0]) {
            text_right(r, &hmi_font_body_b, ux, dsp.y + (dsp.h - hmi_font_body_b.line_h) / 2, v->np_unit, C_MUTED);
            ux -= hmi_text_width(&hmi_font_body_b, v->np_unit) + 8;
        }
        bool empty = !v->np_entry || !v->np_entry[0];
        text_right(r, &hmi_font_headline, ux, dsp.y + (dsp.h - hmi_font_headline.line_h) / 2,
                   empty ? "0" : v->np_entry, empty ? C_DIM : C_TEXT);
    }
    hmi_rect_t info = { m.x + 8, m.y + 108, m.w - 16, 20 };
    const char *msg = v->modal_msg ? v->modal_msg : v->np_info;
    if (msg) text_center(r, &hmi_font_caption, info, msg, v->modal_msg && v->modal_msg_error ? C_RED : C_MUTED);

    static const char * const labels[HMI_NP_KEYS] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Delete", "Set", ".", "-" };
    for (int k = 0; k < HMI_NP_KEYS; k++) {
        hmi_rect_t kr = hmi_layout_numpad_key(sw, sh, k);
        if (kr.w == 0 || !band_hits(r, kr.y, kr.h)) continue;
        bool okk = (k == HMI_NP_OK);
        hmi_draw_round_frame(r, kr.x, kr.y, kr.w, kr.h, 10, okk ? C_TEXT : C_SURF, okk ? C_TEXT : C_LINE);
        const hmi_font_t *f = (k <= 9 || k >= HMI_NP_DOT) ? &hmi_font_title : &hmi_font_body_b;
        text_center(r, f, kr, labels[k], okk ? C_BG : C_TEXT);
    }
}

static void render_confirm(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t m)
{
    int sw = r->screen_w, sh = r->screen_h;
    text_fit(r, &hmi_font_title, m.x + 20, m.y + 20, m.w - 40, v->cf_title ? v->cf_title : "Confirm", C_TEXT);
    if (v->cf_detail) text_fit(r, &hmi_font_body, m.x + 20, m.y + 58, m.w - 40, v->cf_detail, C_MUTED);
    text_fit(r, &hmi_font_caption, m.x + 20, m.y + 86, m.w - 40,
             v->emergency ? "Nothing is sent to the machines. The reset is logged."
                          : "The command is sent to the PLC immediately and logged.", C_DIM);
    button(r, hmi_layout_confirm_button(sw, sh, false), "Cancel", BTN_PLAIN);
    button(r, hmi_layout_confirm_button(sw, sh, true), v->cf_ok ? v->cf_ok : "Confirm", v->cf_danger ? BTN_FILLED_RED : BTN_PRIMARY);
}

/* One alarm: what, how bad, since when, what to do, and the four things the operator can do */
static void render_alarm_detail(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t m)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_rect_t cancel = hmi_layout_modal_cancel(sw, sh, HMI_MODAL_ALARM);
    button(r, cancel, "Close", BTN_PLAIN);
    hmi_tag_id_t tag = v->almd_tag;
    const hmi_tag_meta_t *meta = hmi_tag_get_meta(tag);
    if (!v->alarms || tag >= HMI_MAX_TAGS || !meta) return;
    const hmi_alarm_point_t *pt = &v->alarms->points[tag];
    char b[112], val[24], tm[24];
    int x = m.x + 20, w = m.w - 40, y = m.y + 18;

    uint8_t sev = pt->state != ALARM_PT_NORMAL ? pt->peak_severity : ALARM_SEV_NONE;
    sev_icon(r, x + 9, y + 10, 9, sev, C_WHITE);
    snprintf(b, sizeof(b), "%s%s%s", meta->group ? meta->group : "", meta->group ? ": " : "", meta->name);
    text_fit(r, &hmi_font_title, x + 28, y, cancel.x - 12 - (x + 28), b, C_TEXT);
    y += 34;

    bool needs_ack = pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_RTN_UNACK;
    fmt_clock(tm, sizeof(tm), v, pt->since_ms);
    snprintf(b, sizeof(b), "%s  -  %s  -  since %s", sev_label(sev),
             pt->state == ALARM_PT_NORMAL ? "Normal" : state_label(pt->state), tm);
    text_fit(r, &hmi_font_body_b, x, y, w, b, sev ? sev_color(sev) : C_MUTED);
    y += 24;

    fmt_value(val, sizeof(val), v, tag);
    if (meta->type == TAG_TYPE_ANALOG) {
        char wl[16] = "-", tl[16] = "-";
        if (meta->warn_high) hmi_format_scaled(wl, sizeof(wl), meta->warn_high, meta->decimals);
        if (meta->trip_high) hmi_format_scaled(tl, sizeof(tl), meta->trip_high, meta->decimals);
        snprintf(b, sizeof(b), "Now %s %s   High %s   High-high %s", val, meta->unit ? meta->unit : "", wl, tl);
    } else {
        int16_t raw; uint8_t q;
        bool ok = tag_value(v, tag, &raw, &q);
        snprintf(b, sizeof(b), "Now %s", ok ? (raw ? "On" : "Off") : "-- (no data)");
    }
    text_fit(r, &hmi_font_body, x, y, w, b, C_TEXT);
    y += 24;

    /* what to do */
    snprintf(b, sizeof(b), "1. %s", meta->action1 ? meta->action1 : "Check the field device and its wiring");
    text_fit(r, &hmi_font_body, x, y, w, b, C_TEXT);
    y += 20;
    snprintf(b, sizeof(b), "2. %s", meta->action2 ? meta->action2 : "Acknowledge once the cause is understood");
    text_fit(r, &hmi_font_body, x, y, w, b, C_TEXT);
    y += 26;

    /* handling */
    b[0] = '\0';
    if (pt->flags & HMI_ALARM_F_SHELVED) {
        int32_t left = (int32_t)(pt->shelve_until_ms - v->now_ms);
        snprintf(b, sizeof(b), "Shelved: comes back by itself in %ld min.", (long)(left > 0 ? (left + 59999) / 60000 : 0));
    } else if (pt->flags & HMI_ALARM_F_OOS) {
        snprintf(b, sizeof(b), "Out of service: not sounded until Maintenance returns it.");
    } else if (pt->flags & HMI_ALARM_F_CHATTER) {
        snprintf(b, sizeof(b), "Chattering: came in %u times within a minute. Consider shelving it.", (unsigned)pt->ann_count);
    } else if (!hmi_alarms_can_hide(tag)) {
        snprintf(b, sizeof(b), "A stop alarm: it cannot be shelved or taken out of service.");
    }
    if (b[0]) text_fit(r, &hmi_font_caption, x, y, w, b, C_MUTED);

    /* actions */
    bool can_hide = hmi_alarms_can_hide(tag);
    bool shelved = (pt->flags & HMI_ALARM_F_SHELVED) != 0, oos = (pt->flags & HMI_ALARM_F_OOS) != 0;
    button(r, hmi_layout_almd_button(sw, sh, HMI_ALMD_ACK), "Acknowledge",
           needs_ack ? BTN_PRIMARY : BTN_DISABLED);
    snprintf(b, sizeof(b), shelved ? "Unshelve" : "Shelve for %lu min", (unsigned long)(HMI_ALARM_SHELVE_MS / 60000u));
    button(r, hmi_layout_almd_button(sw, sh, HMI_ALMD_SHELVE), b, (can_hide && !oos) ? BTN_PLAIN : BTN_DISABLED);
    button(r, hmi_layout_almd_button(sw, sh, HMI_ALMD_SERVICE), oos ? "Return to service" : "Out of service",
           can_hide ? BTN_PLAIN : BTN_DISABLED);
    button(r, hmi_layout_almd_button(sw, sh, HMI_ALMD_DEVICE), "Open device",
           (meta->group && tag >= HMI_DEVICE_TAG_BASE) ? BTN_PLAIN : BTN_DISABLED);
    hmi_rect_t foot = { m.x + 16, hmi_layout_almd_button(sw, sh, 0).y - 22, m.w - 32, 16 };
    if (band_hits(r, foot.y, foot.h) && v->sec && v->sec->current_role == ROLE_VIEWER)
        text_center(r, &hmi_font_caption, foot, "Signed in as Viewer: each action asks for the PIN of the role it needs", C_MUTED);
}

/* Search keyboard: the search text, then 4 rows of keys. The list filters as the operator types. */
static void render_search_keyboard(hmi_renderer_t *r, const hmi_view_t *v, hmi_rect_t m)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_rect_t cancel = hmi_layout_modal_cancel(sw, sh, HMI_MODAL_SEARCH);
    button(r, cancel, "Close", BTN_PLAIN);
    char t[64];
    snprintf(t, sizeof(t), "Search readings  -  %u match", (unsigned)v->read_matches);
    text_fit(r, &hmi_font_title, m.x + 18, m.y + 16, cancel.x - 8 - (m.x + 18), t, C_TEXT);
    hmi_rect_t dsp = hmi_layout_kbd_display(sw, sh);
    if (band_hits(r, dsp.y, dsp.h)) {
        hmi_draw_round_frame(r, dsp.x, dsp.y, dsp.w, dsp.h, 10, C_SURF, C_TEXT);
        const char *q = v->read_query ? v->read_query : "";
        bool empty = !q[0];
        text_fit(r, &hmi_font_headline, dsp.x + 14, dsp.y + (dsp.h - hmi_font_headline.line_h) / 2, dsp.w - 28,
                 empty ? "Device or reading name" : q, empty ? C_DIM : C_TEXT);
    }
    hmi_rect_t hint = { m.x + 16, dsp.y + dsp.h + 2, m.w - 32, 14 };
    if (band_hits(r, hint.y, hint.h))
        text_center(r, &hmi_font_caption, hint, "A keyboard types here too. EMERGENCY: the red button at the top.", C_MUTED);
    for (int k = 0; k < HMI_KBD_KEYS; k++) {
        hmi_rect_t kr = hmi_layout_kbd_key(sw, sh, k);
        if (kr.w == 0 || !band_hits(r, kr.y, kr.h)) continue;
        bool done = hmi_layout_kbd_char(k) == HMI_KBD_DONE;
        hmi_draw_round_frame(r, kr.x, kr.y, kr.w, kr.h, 8, done ? C_TEXT : C_SURF, done ? C_TEXT : C_LINE);
        const char *lab = hmi_layout_kbd_label(k);
        const hmi_font_t *f = lab[1] ? &hmi_font_caption : &hmi_font_title;
        text_center(r, f, kr, lab, done ? C_BG : C_TEXT);
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
    if (v->modal == HMI_MODAL_FACEPLATE) { render_faceplate(r, v, m); return; }
    if (v->modal == HMI_MODAL_NUMPAD)    { render_numpad(r, v, m); return; }
    if (v->modal == HMI_MODAL_CONFIRM)   { render_confirm(r, v, m); return; }
    if (v->modal == HMI_MODAL_SEARCH)    { render_search_keyboard(r, v, m); return; }
    if (v->modal == HMI_MODAL_ALARM)     { render_alarm_detail(r, v, m); return; }
    hmi_rect_t cancel = hmi_layout_modal_cancel(sw, sh, v->modal);
    button(r, cancel, "Cancel", BTN_PLAIN);

    int role_now = v->sec ? (int)v->sec->current_role : 0;
    if (v->modal == HMI_MODAL_ROLE) {
        hmi_draw_text(r, &hmi_font_title, m.x + 18, m.y + 16, "Sign in as", C_TEXT);
        for (int i = 0; i < HMI_ROLE_COUNT; i++) {
            hmi_rect_t row = hmi_layout_modal_role(sw, sh, i);
            if (!band_hits(r, row.y, row.h)) continue;
            bool cur = (i == role_now);
            hmi_draw_round_frame(r, row.x, row.y, row.w, row.h, 10, C_SURF, cur ? C_TEXT : C_LINE);
            hmi_draw_circle(r, row.x + 18, row.y + row.h / 2, 5, i == 0 ? C_DIM : C_TEXT);
            hmi_draw_text(r, &hmi_font_body_b, row.x + 34, row.y + 5, s_role_names[i], C_TEXT);
            hmi_draw_text(r, &hmi_font_caption, row.x + 34, row.y + 23, s_role_desc[i], C_MUTED);
            if (cur) text_right(r, &hmi_font_caption, row.x + row.w - 14, row.y + 15, "CURRENT", C_TEXT);
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
    const char *msg = v->modal_msg ? v->modal_msg : "Enter your PIN";
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
        uint8_t fill = ok ? (can_ok ? C_TEXT : C_SURF) : C_SURF;
        hmi_draw_round_frame(r, kr.x, kr.y, kr.w, kr.h, 10, fill, ok && can_ok ? C_TEXT : C_LINE);
        const hmi_font_t *f = (k <= 9) ? &hmi_font_title : &hmi_font_body_b;
        uint8_t tc = ok ? (can_ok ? C_BG : C_DIM) : (v->lockout_ms ? C_DIM : C_TEXT);
        text_center(r, f, kr, labels[k], tc);
    }
}

/* ========================================================================= */
/* Frame dispatch                                                             */
/* ========================================================================= */

/* Below the supported display size the full layout cannot be drawn without overlap, and an
 * unreadable HMI is worse than a plain one: show what this panel is, the alarm state and EMERGENCY. */
static void render_too_small(hmi_renderer_t *r, const hmi_view_t *v)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_draw_rect(r, 0, 0, sw, sh, C_BG);
    hmi_rect_t e = hmi_layout_estop_button(sw, sh);
    hmi_draw_round_rect(r, e.x, e.y, e.w, e.h, 10, C_RED);
    emergency_label(r, e, C_WHITE);
    text_fit(r, &hmi_font_title, 12, 14, e.x - 24, "Prometheus HMI", C_TEXT);
    int y = HMI_LAYOUT_HEADER_H + 8;
    char t[96];
    if (v->alarms && (v->alarms->unacked_count || v->alarms->active_count)) {
        sev_icon(r, 22, y + 12, 10, v->alarms->highest_severity, C_WHITE);
        snprintf(t, sizeof(t), "%u alarm(s), %u unacknowledged", (unsigned)v->alarms->active_count,
                 (unsigned)v->alarms->unacked_count);
        text_fit(r, &hmi_font_body_b, 40, y + 2, sw - 52, t, sev_color(v->alarms->highest_severity));
    } else {
        text_fit(r, &hmi_font_body_b, 12, y + 2, sw - 24, "No alarms", C_MUTED);
    }
    y += 34;
    snprintf(t, sizeof(t), "This display is %dx%d. The HMI needs %dx%d or larger.", sw, sh, HMI_LAYOUT_MIN_W, HMI_LAYOUT_MIN_H);
    text_fit(r, &hmi_font_body, 12, y, sw - 24, t, C_MUTED);
    text_fit(r, &hmi_font_body, 12, y + 22, sw - 24, "Operate the plant from a supported panel.", C_MUTED);
}

/* The EMERGENCY screen: the whole display red, what happened, and machine by machine whether the
 * software stop reached it. Shown at every display size, over every other screen. */
static const char *stop_text(uint8_t res, bool *missed)
{
    *missed = false;
    switch (res) {
        case HMI_STOP_SENT:       return "Stop sent";
        case HMI_STOP_NOTHING:    return "Nothing to stop";
        case HMI_STOP_OFFLINE:    *missed = true; return "NOT STOPPED - offline: stop it at the machine";
        case HMI_STOP_FAILED:     *missed = true; return "NOT STOPPED - stop failed: stop it at the machine";
        case HMI_STOP_NO_CONTROL: *missed = true; return "NOT STOPPED - no stop control: stop it at the machine";
        default:                  return "Stopping...";
    }
}

static void emergency_row(hmi_renderer_t *r, hmi_rect_t row, const char *name, const char *status, bool missed)
{
    if (!band_hits(r, row.y, row.h)) return;
    if (missed) hmi_draw_round_rect(r, row.x, row.y, row.w, row.h, 8, C_BG);   /* NOT stopped: stands out */
    uint8_t tc = missed ? C_TEXT : C_WHITE;
    int ty = row.y + (row.h - hmi_font_body_b.line_h) / 2;
    int name_w = row.w * 2 / 5;
    text_fit(r, &hmi_font_body_b, row.x + 10, ty, name_w - 16, name, tc);
    text_fit(r, &hmi_font_body_b, row.x + name_w, ty, row.w - name_w - 10, status, tc);
}

static void render_emergency(hmi_renderer_t *r, const hmi_view_t *v)
{
    int sw = r->screen_w, sh = r->screen_h;
    hmi_draw_rect(r, 0, 0, sw, sh, C_RED);

    /* header: symbol, title, the EMERGENCY button in its usual place (pressing it again re-sends) */
    if (band_hits(r, 0, HMI_LAYOUT_HEADER_H)) {
        int cy = HMI_LAYOUT_HEADER_H / 2;
        hmi_draw_circle(r, HMI_LAYOUT_MARGIN + 14, cy, 14, C_WHITE);
        hmi_rect_t mark = { HMI_LAYOUT_MARGIN, cy - 14, 28, 28 };
        text_center(r, &hmi_font_headline, mark, "!", C_RED);
        hmi_rect_t e = hmi_layout_estop_button(sw, sh);
        text_fit(r, &hmi_font_headline, HMI_LAYOUT_MARGIN + 38, cy - hmi_font_headline.line_h / 2,
                 e.x - HMI_LAYOUT_MARGIN - 50, "EMERGENCY", C_WHITE);
        hmi_draw_round_rect(r, e.x, e.y, e.w, e.h, 10, C_WHITE);
        emergency_label(r, e, C_RED);
        hmi_draw_rect(r, 0, HMI_LAYOUT_HEADER_H - 2, sw, 2, C_WHITE);
    }

    /* what happened */
    int missed = v->emergency_unreached, pending = 0;
    for (uint8_t i = 0; v->devices && i < v->ndev; i++) {
        bool m;
        stop_text(v->devices[i].stop_result, &m);
        if (m) missed++;
        if (v->devices[i].stop_result == HMI_STOP_NONE) pending++;
    }
    hmi_rect_t t = hmi_layout_emergency_title(sw, sh);
    if (band_hits(r, t.y, t.h)) {
        const char *head = missed ? "NOT EVERY MACHINE COULD BE STOPPED"
                         : pending ? "STOPPING ALL MACHINES..." : "ALL MACHINES STOPPED BY SOFTWARE";
        text_fit(r, &hmi_font_headline, t.x, t.y, t.w, head, C_WHITE);
        char when[24], line[112];
        fmt_clock(when, sizeof(when), v, v->emergency_at_ms);
        snprintf(line, sizeof(line), "Pressed at %s. This is a software stop over the network.", when);
        text_fit(r, &hmi_font_body_b, t.x, t.y + 40, t.w, line, C_WHITE);
        text_fit(r, &hmi_font_body_b, t.x, t.y + 64, t.w,
                 "Also press the hardwired E-stop at each machine. Machines stay stopped after Reset.", C_WHITE);
    }

    /* machine by machine. Items in order of what the worker must act on: machines NOT stopped
     * first, then those still being stopped, then the stopped ones. Item index v->ndev stands for
     * the machines on the network that this HMI does not hold. */
    static int order[HMI_MAX_DEVICES + 1];
    int items = 0;
    if (v->emergency_unreached) order[items++] = v->ndev;
    for (int pass = 0; pass < 3; pass++) {
        for (int k = 0; v->devices && k < v->ndev; k++) {
            bool m;
            stop_text(v->devices[k].stop_result, &m);
            int rank = m ? 0 : v->devices[k].stop_result == HMI_STOP_NONE ? 1 : 2;
            if (rank == pass) order[items++] = k;
        }
    }
    int cap = hmi_layout_emergency_rows(sw, sh) * hmi_layout_emergency_cols(sw, sh);
    int shown = items <= cap ? items : cap - 1;           /* the last place says how many more */
    for (int k = 0; k < shown; k++) {
        hmi_rect_t row = hmi_layout_emergency_row(sw, sh, k);
        if (row.w == 0) break;
        int i = order[k];
        if (i < v->ndev) {
            bool m;
            const char *st = stop_text(v->devices[i].stop_result, &m);
            emergency_row(r, row, v->devices[i].label, st, m);
        } else {
            char name[48];
            snprintf(name, sizeof(name), "%u machine(s) not on this HMI", (unsigned)v->emergency_unreached);
            emergency_row(r, row, name, "NOT STOPPED - stop them at the machine", true);
        }
    }
    if (shown >= 0 && shown < items) {
        hmi_rect_t row = hmi_layout_emergency_row(sw, sh, shown);
        int more_missed = 0;
        for (int k = shown; k < items; k++) {
            bool m = order[k] >= v->ndev;
            if (!m) stop_text(v->devices[order[k]].stop_result, &m);
            if (m) more_missed++;
        }
        char name[48], st[48];
        snprintf(name, sizeof(name), "+ %d more", items - shown);
        if (more_missed) snprintf(st, sizeof(st), "%d of them NOT STOPPED", more_missed);
        else snprintf(st, sizeof(st), "all stopped");
        if (row.w) emergency_row(r, row, name, st, more_missed != 0);
    }

    /* Reset: Maintenance, after a confirmation. It only leaves this screen. */
    hmi_rect_t b = hmi_layout_emergency_reset(sw, sh);
    if (b.w > 0 && band_hits(r, b.y, b.h)) {
        hmi_draw_round_frame(r, b.x, b.y, b.w, b.h, 12, C_BG, C_WHITE);
        text_center(r, &hmi_font_title, b, "Reset EMERGENCY (Maintenance)", C_TEXT);
    }
}

void hmi_renderer_render_band(hmi_renderer_t *r, int band_idx, const hmi_view_t *v)
{
    if (!r || !v || band_idx < 0 || band_idx >= r->band_count) return;
    r->current_band_y = band_idx * r->band_h;
    reset_clip(r);

    /* Clear band (rows beyond screen_h in the last band are harmless) */
    memset(r->buffer, (C_BG << 4) | C_BG, (size_t)r->band_h * (size_t)(r->screen_w / 2));

    if (v->emergency) {
        render_emergency(r, v);
        if (v->modal != HMI_MODAL_NONE && !HMI_LAYOUT_TOO_SMALL(r->screen_w, r->screen_h)) {
            scrim_band(r);
            render_modal(r, v);                          /* sign-in / confirm for the Reset */
        } else {
            render_toast(r, v);
        }
        return;
    }
    if (HMI_LAYOUT_TOO_SMALL(r->screen_w, r->screen_h)) { render_too_small(r, v); return; }
    bool banner = v->alarms && hmi_layout_banner_shown(v->active_screen, v->alarms->current_action.active);
    hmi_rect_t content = hmi_layout_content(r->screen_w, r->screen_h, banner);

    switch (v->active_screen) {
        case SCREEN_PROCESS_OVERVIEW: render_devices(r, v, content); break;
        case SCREEN_READINGS:         render_readings(r, v, banner); break;
        case SCREEN_HOST_TELEMETRY:
            if (v->ntok_host) render_manifest(r, v, v->tokens_host, v->ntok_host, content, NULL);
            else if (band_hits(r, content.y, 60)) {           /* never a blank screen: say why */
                hmi_draw_text(r, &hmi_font_headline, content.x + 2, content.y + 4, "Telemetry", C_TEXT);
                hmi_draw_text(r, &hmi_font_body, content.x + 2, content.y + 40,
                              "No host telemetry on this platform.", C_MUTED);
            }
            break;
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
        /* keep EMERGENCY at full strength: it stays operable over dialogs */
        hmi_rect_t e = hmi_layout_estop_button(r->screen_w, r->screen_h);
        if (band_hits(r, e.y, e.h)) {
            hmi_draw_round_rect(r, e.x, e.y, e.w, e.h, 10, C_RED);
            emergency_label(r, e, C_WHITE);
        }
        render_modal(r, v);
    } else {
        render_toast(r, v);
    }
}
