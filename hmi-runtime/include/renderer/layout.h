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

#include "config.h"
#include "fieldbus/device_info.h"

#define HMI_LAYOUT_MIN_W      640   /* smallest display the full HMI is laid out for (5" 800x480 and up) */
#define HMI_LAYOUT_MIN_H      400   /* below it: a single safe screen, STOP ALL and the alarm count only */
#define HMI_LAYOUT_TOO_SMALL(sw, sh) ((sw) < HMI_LAYOUT_MIN_W || (sh) < HMI_LAYOUT_MIN_H)
#define HMI_LAYOUT_HEADER_H   56
#define HMI_LAYOUT_BANNER_H   82    /* title + one line per guidance step: never truncated */
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
    HMI_HIT_ESTOP,            /* header EMERGENCY button (always visible, anyone may press it) */
    HMI_HIT_THEME,            /* header theme button */
    HMI_HIT_ROLE,             /* header role chip (demo role selector) */
    HMI_HIT_BANNER_ACK,       /* "Acknowledge" button inside the alarm banner */
    HMI_HIT_WIDGET_BUTTON,    /* manifest WIDGET_ACTION_BUTTON; param = tag id */
    HMI_HIT_ALARM_ACK,        /* alarm screen: acknowledge all */
    HMI_HIT_ALARM_SILENCE,    /* alarm screen: silence horn */
    HMI_HIT_ESTOP_RESET,      /* alarm screen: reset the stop latch */
    HMI_HIT_SET_ROLE,         /* settings screen: next role */
    HMI_HIT_SET_THEME,        /* settings screen: toggle theme */
    HMI_HIT_SET_WD_TEST,      /* settings screen: watchdog fault injection test */
    HMI_HIT_SET_WD_RESET,     /* settings screen: reset watchdog failover latch */
    HMI_HIT_MODAL_ROLE,       /* role picker row; param = hmi_role_t */
    HMI_HIT_MODAL_KEY,        /* PIN keypad; param 0..9 = digit, 10 = delete, 11 = OK */
    HMI_HIT_MODAL_CANCEL,     /* dialog Cancel button, or a tap outside the dialog */
    HMI_HIT_MODAL_BLOCK,      /* tap inside the dialog on no control: swallowed */
    HMI_HIT_DEVICE_OPEN,      /* device panel header: open its faceplate; param = device slot */
    HMI_HIT_FP_CTRL,          /* faceplate control; param = index into hmi_profile_fp_controls() */
    HMI_HIT_NUMPAD_KEY,       /* numeric keypad; param = 0..9 or HMI_NP_* */
    HMI_HIT_CONFIRM_OK,       /* confirmation dialog: confirm */
    HMI_HIT_ALARM_TAB,        /* alarm screen: param 0 = Active, 1 = History */
    HMI_HIT_ALARM_PAGE,       /* alarm screen: param 0 = previous page, 1 = next page */
    HMI_HIT_DEVICE_PAGE,      /* process screen: param 0 = previous page, 1 = next page */
    HMI_HIT_EMERGENCY_RESET,  /* EMERGENCY screen: "Reset EMERGENCY" (Maintenance) */
    HMI_HIT_READ_FILTER,      /* Readings: filter chip; param = HMI_READ_FILTER_* */
    HMI_HIT_READ_SEARCH,      /* Readings: open the search keyboard */
    HMI_HIT_READ_PAGE,        /* Readings: param 0 = previous page, 1 = next page */
    HMI_HIT_READ_ROW,         /* Readings: param = row on the page (a device row opens its faceplate) */
    HMI_HIT_KBD_KEY,          /* search keyboard: param = key index, see hmi_layout_kbd_char() */
    HMI_HIT_ALMD_BTN,         /* alarm dialog: param = HMI_ALMD_* button */
    HMI_HIT_DEVICE_FOCUS,     /* overview tile: show that device's full graphics; param = device slot */
    HMI_HIT_FOCUS_BACK        /* device view: back to all devices */
} hmi_hit_kind_t;

/* Dialogs drawn over the screen. While one is open only the dialog and the
 * header EMERGENCY button accept input. */
typedef enum {
    HMI_MODAL_NONE = 0,
    HMI_MODAL_ROLE,           /* choose a role */
    HMI_MODAL_PIN,            /* PIN keypad for the chosen role */
    HMI_MODAL_FACEPLATE,      /* a device's values and all its controls */
    HMI_MODAL_NUMPAD,         /* numeric entry of a setpoint */
    HMI_MODAL_CONFIRM,        /* "are you sure?" before a command is sent */
    HMI_MODAL_SEARCH,         /* on-screen keyboard for the Readings search */
    HMI_MODAL_ALARM           /* one alarm: details, guidance, Ack / Shelve / Out of service / Open device */
} hmi_modal_t;

/* Alarm dialog buttons */
enum { HMI_ALMD_ACK = 0, HMI_ALMD_SHELVE = 1, HMI_ALMD_SERVICE = 2, HMI_ALMD_DEVICE = 3, HMI_ALMD_BUTTONS = 4 };

/* Numeric keypad keys besides the digits 0..9 */
#define HMI_NP_DEL   10
#define HMI_NP_OK    11
#define HMI_NP_DOT   12
#define HMI_NP_MINUS 13
#define HMI_NP_KEYS  14

#define HMI_KEY_DELETE 10
#define HMI_KEY_OK     11

typedef struct {
    hmi_hit_kind_t kind;
    uint16_t       param;          /* screen, role, key, row ... or an absolute tag id */
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

/* The top-alarm banner is shown on every screen except Alarms, whose first row is that alarm.
 * Renderer and touch handling both ask this, so the content area always agrees. */
bool       hmi_layout_banner_shown(uint8_t screen, bool alarm_active);

/* Alarms screen: one card that holds either the Active summary or the History (journal),
 * chosen with two tabs in the title row, with Prev / Next paging in the card footer.
 * Shared by the renderer and the touch handling, so a drawn button and its touch area are
 * always the same rectangle. */
#define HMI_LAYOUT_ALARM_ROW_H   44    /* summary rows: an Ack button big enough for a gloved finger */
#define HMI_LAYOUT_HISTORY_ROW_H 26
#define HMI_LAYOUT_ALARM_FOOT_H  52
enum { HMI_ALARM_VIEW_ACTIVE = 0, HMI_ALARM_VIEW_HISTORY = 1, HMI_ALARM_VIEW_SHELVED = 2, HMI_ALARM_VIEWS = 3 };
hmi_rect_t hmi_layout_alarm_active_card(int sw, int sh, bool banner_active, int nact);
int        hmi_layout_alarm_rows(hmi_rect_t card);                 /* summary rows per page */
int        hmi_layout_history_rows(hmi_rect_t card);               /* journal rows per page */
int        hmi_layout_alarm_row_y(hmi_rect_t card, int row);       /* text top of a summary row */
hmi_rect_t hmi_layout_alarm_row(hmi_rect_t card, int row);         /* the whole row: opens the alarm dialog */
hmi_rect_t hmi_layout_alarm_row_ack(hmi_rect_t card, int row);     /* that row's Ack (or Unshelve) button */
hmi_rect_t hmi_layout_alarm_tab(int sw, int sh, int view);         /* Active / Shelved / History tab */
/* Alarm dialog (HMI_MODAL_ALARM) */
hmi_rect_t hmi_layout_almd_button(int sw, int sh, int button);    /* HMI_ALMD_* */
hmi_rect_t hmi_layout_alarm_page(hmi_rect_t card, int dir);        /* dir 0 = previous, 1 = next */
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
 *   3 -> three columns on wide screens, else 2 x 2; 4 -> 2 x 2;
 *   5 or more -> a grid of readable cards (2..4 columns, 2..3 rows by screen size) when they all
 *   fit on one screen; otherwise an overview of small tiles (name, state, main value, alarms),
 *   as many as the screen holds, with Previous / Next pages when even the tiles do not fit.
 * Each card = a header row (device name, address, status) + a body that shows the
 * profile's full manifest, or its compact manifest when the body is small. A tile is one tap
 * target that opens the device's faceplate. */
#define HMI_LAYOUT_TILE_MIN_W      176   /* 4 tiles across an 800 px display */
#define HMI_LAYOUT_TILE_MIN_H      88    /* name, value name, value: never overlapping */
#define HMI_LAYOUT_TILE_MAX_COLS   8
bool hmi_layout_device_tiles(hmi_rect_t content, int ndev);            /* overview tiles instead of cards */
/* Device view (a tile was tapped): one device with its full graphics, as when it is the only one,
 * above a row with "< All devices" (left) and "Controls >" (right, opens the faceplate). */
hmi_rect_t hmi_layout_focus_panel(hmi_rect_t content);
hmi_rect_t hmi_layout_focus_button(hmi_rect_t content, int which);     /* 0 = back, 1 = controls */
hmi_hit_t  hmi_layout_focus_hit_test(int sw, int sh, bool banner_active, const hmi_device_info_t *dev, int x, int y);
#define HMI_LAYOUT_DEVICE_HEADER_H 34
/* A device panel lower than this cannot show its widgets without overlap: it shows a summary
 * (main value, state) instead, and a tap anywhere on it opens the faceplate with every control. */
#define HMI_LAYOUT_SUMMARY_BELOW_H 200
bool hmi_layout_device_summary(hmi_rect_t panel);
#define HMI_LAYOUT_COMPACT_W       620
#define HMI_LAYOUT_COMPACT_H       260

#define HMI_LAYOUT_DEV_PAGER_H     40
/* content = the screen's content area, ndev = devices present, idx = position ON THE PAGE */
hmi_rect_t hmi_layout_device_panel(hmi_rect_t content, int ndev, int idx);
int        hmi_layout_devices_per_page(hmi_rect_t content, int ndev);   /* ndev when no pages are needed */
hmi_rect_t hmi_layout_process_area(hmi_rect_t content, int ndev);       /* content minus the pager row */
hmi_rect_t hmi_layout_device_pager(hmi_rect_t content, int which);      /* 0 = previous, 1 = next */
hmi_rect_t hmi_layout_device_body(hmi_rect_t panel);
bool       hmi_layout_device_compact(hmi_rect_t body);
/* The manifest a device shows in a body of this size */
const uint32_t* hmi_layout_device_manifest(const hmi_device_profile_t *p, hmi_rect_t body, uint8_t *count);

/* --- Device faceplate (HMI_MODAL_FACEPLATE) ---
 * Left column: every value of the device. Right column: one row per controlled tag,
 * with its buttons (Start / Stop, Set..., mode options, Reset). */
hmi_rect_t hmi_layout_device_open(hmi_rect_t panel);                 /* tap target: panel header */
hmi_rect_t hmi_layout_fp_values(int sw, int sh);
hmi_rect_t hmi_layout_fp_row(int sw, int sh, int row);
hmi_rect_t hmi_layout_fp_ctrl(int sw, int sh, const hmi_device_profile_t *p, int ctrl);

/* --- Numeric keypad (HMI_MODAL_NUMPAD) and confirmation (HMI_MODAL_CONFIRM) --- */
hmi_rect_t hmi_layout_numpad_display(int sw, int sh);
hmi_rect_t hmi_layout_numpad_key(int sw, int sh, int key);           /* 0..9, HMI_NP_* */
hmi_rect_t hmi_layout_confirm_button(int sw, int sh, bool ok);       /* ok = Confirm, else Cancel */

/* --- Readings screen ---
 * One card listing every reading of every device, grouped under a row per device, in rows tall
 * enough to touch. The title row holds the filter chips (All / In alarm / Bad data) and Search;
 * the card footer holds Previous / Next. A device row opens that device's faceplate. */
#define HMI_LAYOUT_READ_ROW_H     40
#define HMI_LAYOUT_READ_ROW_MIN_H 34  /* short cards (under 360 px): more rows per page */
#define HMI_LAYOUT_READ_HEAD_H    24  /* column captions */
#define HMI_LAYOUT_READ_FOOT_H    48
enum { HMI_READ_FILTER_ALL = 0, HMI_READ_FILTER_ALARM = 1, HMI_READ_FILTER_BAD = 2, HMI_READ_FILTERS = 3 };
hmi_rect_t hmi_layout_read_card(int sw, int sh, bool banner_active);
int        hmi_layout_read_rows(hmi_rect_t card);                  /* rows per page (>= 1) */
hmi_rect_t hmi_layout_read_row(hmi_rect_t card, int row);
hmi_rect_t hmi_layout_read_page(hmi_rect_t card, int dir);         /* dir 0 = previous, 1 = next */
hmi_rect_t hmi_layout_read_filter(int sw, int sh, bool banner_active, int filter);
hmi_rect_t hmi_layout_read_search(int sw, int sh, bool banner_active);

/* --- Search keyboard (HMI_MODAL_SEARCH): 4 rows of 10 keys --- */
#define HMI_KBD_KEYS   40
#define HMI_KBD_DEL    '\b'
#define HMI_KBD_CLEAR  0x7F
#define HMI_KBD_DONE   '\n'
char       hmi_layout_kbd_char(int key);   /* '0'..'9', 'A'..'Z', ' ', HMI_KBD_DEL / _CLEAR / _DONE; 0 = none */
const char *hmi_layout_kbd_label(int key); /* key cap text */
hmi_rect_t hmi_layout_kbd_display(int sw, int sh);
hmi_rect_t hmi_layout_kbd_key(int sw, int sh, int key);

/* Topmost interactive element at (x, y).
 *   tokens/ntok : manifest of a fixed-manifest screen (telemetry), may be NULL/0
 *   devs/ndev   : present devices in display order (process screen), may be NULL/0
 *   dev_page    : process-screen page shown (clamped to the last page)
 *   modal       : hmi_modal_t currently open (HMI_MODAL_NONE for none)
 *   fp_slot     : device slot of the open faceplate (looked up in devs)
 * HMI_HIT_WIDGET_BUTTON returns the ABSOLUTE tag id of the button. */
hmi_hit_t hmi_layout_hit_test(int sw, int sh, uint8_t screen, bool banner_active, uint8_t modal,
                              const uint32_t *tokens, uint8_t ntok,
                              const hmi_device_info_t *devs, uint8_t ndev, uint16_t dev_page, uint8_t fp_slot,
                              int x, int y);

/* --- EMERGENCY screen ---
 * While the EMERGENCY is active it replaces every other screen (any display size): red, the
 * EMERGENCY button in its usual header place, two message lines, one row per device saying
 * whether it was stopped, and the Reset button. Dialogs (sign-in, confirm) open over it. */
#define HMI_LAYOUT_EMERG_ROW_H 34
hmi_rect_t hmi_layout_emergency_title(int sw, int sh);    /* headline + two message lines */
hmi_rect_t hmi_layout_emergency_list(int sw, int sh);     /* device rows */
int        hmi_layout_emergency_cols(int sw, int sh);     /* 1, or 2 on wide screens */
int        hmi_layout_emergency_rows(int sw, int sh);     /* rows per column that fit (>= 0) */
hmi_rect_t hmi_layout_emergency_row(int sw, int sh, int idx);   /* row idx (column-major), w = 0 if it does not fit */
hmi_rect_t hmi_layout_emergency_reset(int sw, int sh);    /* "Reset EMERGENCY" */
/* Only EMERGENCY (HMI_HIT_ESTOP) and Reset (HMI_HIT_EMERGENCY_RESET) answer; anything else is
 * HMI_HIT_NONE. Use the normal hit test while a dialog is open over the EMERGENCY screen. */
hmi_hit_t  hmi_layout_emergency_hit_test(int sw, int sh, int x, int y);

#endif /* INDUSTRIAL_HMI_LAYOUT_H */
