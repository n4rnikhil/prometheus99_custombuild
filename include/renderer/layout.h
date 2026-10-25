/**
 * @file layout.h
 * @brief Single source of truth for screen geometry, shared by the renderer and hit-testing
 *
 * Every interactive element is drawn at, and hit-tested against, the rect these
 * functions return, so a tap always lands on what the operator sees.
 * All functions are pure (no globals, no platform calls).
 *
 * Vertical structure:
 *   header   y = 0 .. HMI_LAYOUT_HEADER_H
 *   banner   below the header, only while an alarm needs attention
 *   content  between header/banner and the dock (manifest grid maps into this)
 *   dock     y = screen_h - HMI_LAYOUT_DOCK_H .. screen_h  (tab bar)
 */

#ifndef INDUSTRIAL_HMI_LAYOUT_H
#define INDUSTRIAL_HMI_LAYOUT_H

#include "../config.h"
#include "../drivers/device_info.h"

#define HMI_LAYOUT_MIN_W      640
#define HMI_LAYOUT_MIN_H      400
#define HMI_LAYOUT_HEADER_H   56
#define HMI_LAYOUT_BANNER_H   62
#define HMI_LAYOUT_DOCK_H     60
#define HMI_LAYOUT_MARGIN     16
#define HMI_LAYOUT_GAP        12
#define HMI_LAYOUT_TITLE_H    44    /* screen title row on non-manifest screens */
#define HMI_LAYOUT_BUTTON_H   44
#define HMI_LAYOUT_TOUCH_SLOP 4     /* hit rects are this much larger than drawn rects */

typedef struct { int x, y, w, h; } hmi_rect_t;

typedef enum {
    HMI_HIT_NONE = 0,
    HMI_HIT_NAV_TAB,          /* param = screen index 0..SCREEN_COUNT-1 */
    HMI_HIT_ESTOP,            /* header E-STOP button (always visible) */
    HMI_HIT_THEME,            /* header theme button */
    HMI_HIT_ROLE,             /* header role chip (demo role selector) */
    HMI_HIT_BANNER_ACK,       /* "Acknowledge" button inside the alarm banner */
    HMI_HIT_WIDGET_BUTTON,    /* manifest WIDGET_ACTION_BUTTON; param = tag id */
    HMI_HIT_ALARM_ACK,        /* alarm screen: acknowledge all */
    HMI_HIT_ALARM_SILENCE,    /* alarm screen: silence horn */
    HMI_HIT_ESTOP_RESET,      /* alarm screen: reset E-stop */
    HMI_HIT_SET_ROLE,         /* settings screen: next role */
    HMI_HIT_SET_THEME,        /* settings screen: toggle theme */
    HMI_HIT_SET_WD_TEST,      /* settings screen: watchdog fault injection test */
    HMI_HIT_SET_WD_RESET,     /* settings screen: reset watchdog failover latch */
    HMI_HIT_MODAL_ROLE,       /* role picker row; param = hmi_role_t */
    HMI_HIT_MODAL_KEY,        /* PIN keypad; param 0..9 = digit, 10 = delete, 11 = OK */
    HMI_HIT_MODAL_CANCEL,     /* dialog Cancel button, or a tap outside the dialog */
    HMI_HIT_MODAL_BLOCK       /* tap inside the dialog on no control: swallowed */
} hmi_hit_kind_t;

/* Dialogs drawn over the screen. While one is open only the dialog and the
 * header E-STOP accept input. */
typedef enum {
    HMI_MODAL_NONE = 0,
    HMI_MODAL_ROLE,           /* choose a role */
    HMI_MODAL_PIN             /* PIN keypad for the chosen role */
} hmi_modal_t;

#define HMI_KEY_DELETE 10
#define HMI_KEY_OK     11

typedef struct {
    hmi_hit_kind_t kind;
    uint8_t        param;
    hmi_rect_t     rect;
} hmi_hit_t;

bool       hmi_rect_contains(hmi_rect_t r, int x, int y);
hmi_rect_t hmi_rect_inset(hmi_rect_t r, int dx, int dy);

/* Fixed chrome */
hmi_rect_t hmi_layout_header(int sw, int sh);
hmi_rect_t hmi_layout_banner(int sw, int sh);
hmi_rect_t hmi_layout_banner_ack(int sw, int sh);
hmi_rect_t hmi_layout_content(int sw, int sh, bool banner_active);
hmi_rect_t hmi_layout_dock(int sw, int sh);
hmi_rect_t hmi_layout_dock_tab(int sw, int sh, int idx);
hmi_rect_t hmi_layout_estop_button(int sw, int sh);
hmi_rect_t hmi_layout_role_pill(int sw, int sh);
hmi_rect_t hmi_layout_alarm_pill(int sw, int sh);
hmi_rect_t hmi_layout_health_pill(int sw, int sh);
hmi_rect_t hmi_layout_theme_button(int sw, int sh);

/* A manifest token's widget rect (cell rect with the widget-type inset applied) */
hmi_rect_t hmi_layout_token_rect(uint32_t token, hmi_rect_t content);

/* Non-manifest screens: body below the title row, and their cards */
hmi_rect_t hmi_layout_screen_body(int sw, int sh, bool banner_active);
/* Settings: idx 0 = access control (left), 1 = system (right) */
hmi_rect_t hmi_layout_settings_card(int sw, int sh, bool banner_active, int idx);

/* Per-screen buttons. kind is one of HMI_HIT_ALARM_*, HMI_HIT_ESTOP_RESET, HMI_HIT_SET_*;
 * returns {0,0,0,0} for any other kind. */
hmi_rect_t hmi_layout_screen_button(int sw, int sh, bool banner_active, hmi_hit_kind_t kind);

/* Dialog geometry */
hmi_rect_t hmi_layout_modal(int sw, int sh, uint8_t modal);
hmi_rect_t hmi_layout_modal_cancel(int sw, int sh, uint8_t modal);
hmi_rect_t hmi_layout_modal_role(int sw, int sh, int role);      /* role 0..3 */
hmi_rect_t hmi_layout_modal_key(int sw, int sh, int key);        /* key 0..11, see HMI_KEY_* */

/* --- Plug-and-play dashboard ---
 * The process screen is split into one panel per present device:
 *   1 -> full area; 2 -> side by side (or stacked on tall screens);
 *   3 -> three columns on wide screens, else 2 x 2; 4 -> 2 x 2.
 * Each panel = a header row (device name, address, status) + a body that shows the
 * profile's full manifest, or its compact manifest when the body is small. */
#define HMI_LAYOUT_DEVICE_HEADER_H 34
#define HMI_LAYOUT_COMPACT_W       620
#define HMI_LAYOUT_COMPACT_H       260

hmi_rect_t hmi_layout_device_panel(hmi_rect_t content, int ndev, int idx);
hmi_rect_t hmi_layout_device_body(hmi_rect_t panel);
bool       hmi_layout_device_compact(hmi_rect_t body);
/* The manifest a device shows in a body of this size */
const uint32_t* hmi_layout_device_manifest(const hmi_device_profile_t *p, hmi_rect_t body, uint8_t *count);

/* Topmost interactive element at (x, y).
 *   tokens/ntok : manifest of a fixed-manifest screen (telemetry), may be NULL/0
 *   devs/ndev   : present devices in display order (process screen), may be NULL/0
 *   modal       : hmi_modal_t currently open (HMI_MODAL_NONE for none)
 * HMI_HIT_WIDGET_BUTTON returns the ABSOLUTE tag id of the button. */
hmi_hit_t hmi_layout_hit_test(int sw, int sh, uint8_t screen, bool banner_active, uint8_t modal,
                              const uint32_t *tokens, uint8_t ntok,
                              const hmi_device_info_t *devs, uint8_t ndev, int x, int y);

#endif /* INDUSTRIAL_HMI_LAYOUT_H */
