/**
 * @file config.h
 * @brief Industrial HMI Runtime System Configuration
 * @standards IEC 62443-4-2 / IEC 61508 SIL-ready / ISA-101 / ISA-18.2
 *
 * All runtime state is statically allocated (no malloc/calloc/free in
 * application code). The display is rendered through one 19,200-byte
 * band buffer (4-bit CLUT) that is streamed to the window band by band.
 */

#ifndef INDUSTRIAL_HMI_CONFIG_H
#define INDUSTRIAL_HMI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- Display & Band Buffer --- */
#define HMI_DISPLAY_WIDTH_DEFAULT   800
#define HMI_DISPLAY_HEIGHT_DEFAULT  480
#define HMI_DISPLAY_BPP             4      /* 4-bit indexed, 16-colour CLUT */

/* Static band buffer: 19,200 bytes (18.75 KB) */
#define HMI_DISPLAY_MAX_BUF_SIZE    19200

/* Widest frame the renderer will rasterise. Wider windows are drawn at this
 * width (the remainder of the client area stays background). 7680 px gives
 * row_bytes = 3840 and band_h = 5, so one band always fits the buffer. */
#define HMI_DISPLAY_MAX_WIDTH       7680
#define HMI_DISPLAY_MAX_HEIGHT      4320

/* Normalised permille coordinate system (0..1000 of the content area) */
#define HMI_PERMILLE_MAX            1000
#define HMI_NORM_TO_PIXEL(norm, total_px) (((norm) * (total_px)) / HMI_PERMILLE_MAX)

/* --- Timing --- */
#define HMI_FIELDBUS_FREQ_HZ        1000   /* 1 kHz fieldbus / supervision loop */
#define HMI_FIELDBUS_PERIOD_US      (1000000u / HMI_FIELDBUS_FREQ_HZ)
#define HMI_UI_FREQ_HZ              30     /* ~33 ms UI frame period */
#define HMI_UI_FRAME_PERIOD_MS      (1000 / HMI_UI_FREQ_HZ)

/* --- Watchdog --- */
#define HMI_WATCHDOG_MAX_AGE_MS     500    /* heartbeat silence that trips the watchdog */
#define HMI_WATCHDOG_TICK_MS        50     /* evaluation period */
#define HMI_FAULT_INJECT_STALL_MS   750    /* test: fieldbus thread deliberately stalls this long */

/* --- Snapshot staleness (UI marks data stale when the last publish is older) --- */
#define HMI_SNAPSHOT_STALE_MS       500

/* --- Modbus TCP --- */
#define HMI_MODBUS_DEFAULT_PORT     502
#define HMI_MODBUS_POLL_MS          100    /* one read cycle every 100 ms */
#define HMI_MODBUS_TIMEOUT_MS       500    /* response timeout per request */
#define HMI_MODBUS_CONNECT_TIMEOUT_MS 2000
#define HMI_MODBUS_BACKOFF_MIN_MS   1000
#define HMI_MODBUS_BACKOFF_MAX_MS   8000
#define HMI_MODBUS_WRITE_QUEUE      8      /* pending operator writes */
#define HMI_MODBUS_EXC_OFFLINE      3      /* consecutive read exceptions before a device is declared OFFLINE */

/* --- Persistent alarm log --- */
#define HMI_ALARM_LOG_PATH          "alarms.log"
#define HMI_ALARM_LOG_MAX_BYTES     (1024u * 1024u)  /* rotate to alarms.log.1 beyond this */

/* --- Capacities (statically bounded) --- */
/* Tag id map: 0..31 runtime (8..11 host telemetry, 13 HMI watchdog),
 * then HMI_MAX_DEVICES blocks of HMI_DEVICE_TAG_SPAN tags, one per plugged-in PLC. */
#define HMI_MAX_DEVICES             4      /* PLCs on the dashboard at once */
#define HMI_DEVICE_TAG_BASE         32
#define HMI_DEVICE_TAG_SPAN         24
#define HMI_MAX_TAGS                (HMI_DEVICE_TAG_BASE + HMI_MAX_DEVICES * HMI_DEVICE_TAG_SPAN)  /* 128 x 8 B = 1 KB */
#define HMI_DEV_TAG(slot, rel)      ((uint8_t)(HMI_DEVICE_TAG_BASE + (slot) * HMI_DEVICE_TAG_SPAN + (rel)))

/* --- Plug and play --- */
#define HMI_DISCOVERY_PERIOD_MS     3000   /* how often the discovery thread looks for new PLCs */
#define HMI_DISCOVERY_CONNECT_MS    300    /* probe connect timeout */
#define HMI_DISCOVERY_REPLY_MS      600    /* probe identification reply timeout */
#define HMI_DEVICE_REMOVE_MS        10000  /* offline this long -> removed from the dashboard (0 = never) */
#define HMI_DEVICE_SLOT_COOLDOWN_MS 1500   /* a freed slot is not reused before this */
#define HMI_MAX_WIDGETS_PER_SCREEN  24
#define HMI_MAX_SCREENS             5
#define HMI_TREND_SAMPLES           60     /* 60 s at 1 Hz */
#define HMI_ALARM_JOURNAL_SIZE      16     /* in-memory circular journal */
#define HMI_INPUT_QUEUE_SIZE        16

/* --- Ergonomics --- */
#define HMI_MIN_TOUCH_TARGET_PX     48     /* ISO 9241-9 minimum touch target */

/* --- 4-bit CLUT palette indices --- */
#define COLOR_OBSIDIAN_BG      0   /* #0B0F17 */
#define COLOR_TITANIUM_CARD    1   /* #161F2E */
#define COLOR_CARD_BORDER      2   /* #26334D */
#define COLOR_CHALK_WHITE      3   /* #F8FAFC */
#define COLOR_SILVER_MUTED     4   /* #94A3B8 */
#define COLOR_DARK_GRAY        5   /* #334155 */
#define COLOR_CYAN_ACCENT      6   /* #00D2FF */
#define COLOR_BLUE_ACCENT      7   /* #38BDF8 */
#define COLOR_EMERALD_OK       8   /* #10B981 */
#define COLOR_AMBER_WARN       9   /* #F59E0B */
#define COLOR_CRIMSON_TRIP     10  /* #EF4444 */
#define COLOR_VIBRANT_ORANGE   11  /* #FB923C */
#define COLOR_PURPLE_SECURE    12  /* #A855F7 */
#define COLOR_SURFACE_HOVER    13  /* #1E293B */
#define COLOR_DIM_TEXT         14  /* tertiary text / disabled */
#define COLOR_NEON_GREEN       COLOR_DIM_TEXT  /* legacy alias */
#define COLOR_WHITE_PURE       15  /* #FFFFFF */

/* --- Role-Based Access Control (IEC 62443) --- */
typedef enum {
    ROLE_VIEWER      = 0,  /* read-only monitoring */
    ROLE_OPERATOR    = 1,  /* acknowledge alarms, start/stop equipment, reset E-stop */
    ROLE_MAINTENANCE = 2,  /* setpoints, diagnostics */
    ROLE_ENGINEER    = 3   /* configuration, watchdog fault injection */
} hmi_role_t;

#define HMI_ROLE_COUNT 4

/* --- Screens --- */
typedef enum {
    SCREEN_PROCESS_OVERVIEW = 0,
    SCREEN_FIELD_DIAGNOSTICS,
    SCREEN_HOST_TELEMETRY,
    SCREEN_ALARM_MATRIX,
    SCREEN_SECURITY_SETTINGS,
    SCREEN_COUNT
} hmi_screen_id_t;

/* --- Input action keys (input_queue) --- */
typedef enum {
    KEY_NONE = 0,
    KEY_NAV_PREV,
    KEY_NAV_NEXT,
    KEY_ACTION_ACK,
    KEY_ACTION_TOGGLE_CONTRAST,
    KEY_ACTION_EMERGENCY_STOP,
    KEY_ACTION_PUMP_TOGGLE,
    KEY_ACTION_VALVE_TOGGLE,
    KEY_ACTION_SIMULATE_FAULT,
    KEY_ACTION_ROLE_CYCLE
} hmi_key_t;

/* --- Alarm priority (ISA-18.2) --- */
typedef enum {
    ALARM_SEV_NONE     = 0,
    ALARM_SEV_INFO     = 1,
    ALARM_SEV_WARNING  = 2,
    ALARM_SEV_CRITICAL = 3,
    ALARM_SEV_ESTOP    = 4
} hmi_alarm_sev_t;

/* --- Summary latch state shown to the operator --- */
typedef enum {
    ALARM_LATCH_NORMAL = 0,         /* nothing active, nothing unacknowledged */
    ALARM_LATCH_UNACKNOWLEDGED = 1, /* at least one alarm needs acknowledgement */
    ALARM_LATCH_ACKNOWLEDGED = 2    /* all acknowledged, at least one still active */
} hmi_alarm_latch_t;

#endif /* INDUSTRIAL_HMI_CONFIG_H */
