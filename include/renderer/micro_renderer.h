/**
 * @file micro_renderer.h
 * @brief Band-buffer rasteriser and the five HMI screens
 * @standards ISA-101 situational awareness
 *
 * The frame is rendered one horizontal band at a time into ONE static
 * 19,200-byte 4-bpp buffer; the platform layer streams each band to the
 * display (Win32 StretchDIBits, Linux fbdev, or an SPI/parallel LCD on a
 * microcontroller). Everything here is portable C99: no OS calls, no heap,
 * no floating point in the pixel paths.
 *
 * Anti-aliasing works through the palette: a 16 x 16 x 4 lookup table maps
 * (foreground, background, coverage) to the nearest palette entry, so text
 * and curves blend through real intermediate tones without any extra colours.
 */

#ifndef INDUSTRIAL_HMI_MICRO_RENDERER_H
#define INDUSTRIAL_HMI_MICRO_RENDERER_H

#include "../config.h"
#include "../core/tags.h"
#include "../core/alarms.h"
#include "../core/alarm_log.h"
#include "../core/security.h"
#include "../core/watchdog.h"
#include "../core/runtime_stats.h"
#include "../drivers/plc_driver.h"
#include "../drivers/device_info.h"
#include "../drivers/discovery.h"
#include "bytecode.h"
#include "layout.h"
#include "font.h"

typedef struct {
    int  screen_w;        /* render width: rounded UP to a multiple of 8, clamped to HMI_DISPLAY_MAX_WIDTH */
    int  screen_h;        /* clamped to 1..HMI_DISPLAY_MAX_HEIGHT */
    int  band_h;          /* rows per band: HMI_DISPLAY_MAX_BUF_SIZE / (screen_w/2), >= 1, <= screen_h */
    int  band_count;
    int  current_band_y;
    bool high_contrast;
    int  clip_x0, clip_y0, clip_x1, clip_y1;   /* active clip rect (screen coords, x1/y1 exclusive) */
    uint8_t blend[16][16][4];                /* [fg][bg][coverage 0..3] -> palette index */
    uint8_t buffer[HMI_DISPLAY_MAX_BUF_SIZE];
} hmi_renderer_t;

/* Everything one frame needs. All pointers may be NULL (rendered as "--"). */
typedef struct {
    const hmi_tag_table_t        *tags;
    const hmi_alarm_supervisor_t *alarms;
    const hmi_security_ctx_t     *sec;
    const hmi_watchdog_t         *wd;          /* UI-side watchdog: fieldbus liveness + failover latch */
    const hmi_fieldbus_stats_t   *fb;          /* measured loop timing */
    const hmi_alarm_log_stats_t  *log;         /* persistent log stats */
    const hmi_device_info_t      *devices;     /* present devices, display order (process screen) */
    uint8_t                       ndev;
    const hmi_discovery_stats_t  *disc;        /* discovery statistics (may be NULL) */
    const uint32_t *tokens_host;     uint8_t ntok_host;      /* SCREEN_HOST_TELEMETRY manifest */
    uint8_t  active_screen;
    bool     ui_wd_alive;          /* fieldbus-side view of the UI heartbeat (from the snapshot) */
    bool     data_stale;           /* snapshot older than HMI_SNAPSHOT_STALE_MS */
    uint32_t snapshot_age_ms;
    uint32_t crc_mismatches;       /* UI-side end-to-end CRC check failures */
    uint32_t seqlock_retries;
    uint32_t ui_frame_hz;          /* measured */
    uint32_t exe_size_bytes;       /* measured at startup (0 = unknown) */
    uint32_t now_ms;
    const char *toast;             /* transient one-line message (NULL = none) */
    /* Sign-in dialog */
    uint8_t  modal;                /* hmi_modal_t */
    uint8_t  modal_role;           /* role being signed in to (PIN dialog) */
    uint8_t  pin_len;              /* digits typed so far (digits are never passed to the renderer) */
    uint32_t lockout_ms;           /* remaining PIN lockout */
    const char *modal_msg;         /* hint / error line in the PIN dialog and the keypad */
    bool     modal_msg_error;
    /* Device controls */
    uint8_t     fp_slot;           /* faceplate: device slot */
    const char *np_title;          /* numeric keypad: what is being set */
    const char *np_entry;          /* characters typed so far ("" = none) */
    const char *np_unit;
    const char *np_info;           /* "Now 50.0 Hz  -  allowed 0.0 to 60.0" */
    const char *cf_title;          /* confirmation dialog */
    const char *cf_detail;
    const char *cf_ok;             /* confirm button label */
    bool        cf_danger;         /* red confirm button */
} hmi_view_t;

/* --- Lifecycle --- */
void hmi_renderer_init(hmi_renderer_t *rend, int screen_w, int screen_h);
void hmi_renderer_resize(hmi_renderer_t *rend, int screen_w, int screen_h);
void hmi_renderer_set_theme(hmi_renderer_t *rend, bool high_contrast);

/* --- Primitives (clipped to the active band and to the buffer) --- */
void hmi_draw_rect(hmi_renderer_t *r, int x, int y, int w, int h, uint8_t color);
void hmi_draw_round_rect(hmi_renderer_t *r, int x, int y, int w, int h, int radius, uint8_t color);
void hmi_draw_round_frame(hmi_renderer_t *r, int x, int y, int w, int h, int radius,
                          uint8_t fill, uint8_t border);
void hmi_draw_circle(hmi_renderer_t *r, int cx, int cy, int radius, uint8_t color);
void hmi_draw_line(hmi_renderer_t *r, int x0, int y0, int x1, int y1, int thickness, uint8_t color);
/* Text: y is the TOP of the font's line box. Returns the pen x after the string. */
int  hmi_draw_text(hmi_renderer_t *r, const hmi_font_t *f, int x, int y, const char *str, uint8_t color);
int  hmi_text_width(const hmi_font_t *f, const char *str);

/* --- Frame --- */
void hmi_renderer_render_band(hmi_renderer_t *r, int band_idx, const hmi_view_t *view);

const uint8_t*  hmi_renderer_get_band_buffer(const hmi_renderer_t *r);
const uint32_t* hmi_renderer_get_palette(const hmi_renderer_t *r);

#endif /* INDUSTRIAL_HMI_MICRO_RENDERER_H */
