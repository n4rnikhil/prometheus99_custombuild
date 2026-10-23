/**
 * @file config.h
 * @brief Industrial HMI Runtime System Configuration
 * @standards IEC 62443-4-2 / IEC 61508 SIL-ready / Apple HIG Ergonomics
 *
 * Fully static memory budget: Zero heap allocations.
 * Elastic L1-Cache Resident Display Band Buffer: 9.3 KB (2-bit) / 18.75 KB (4-bit).
 * Apple-inspired Calm Industrial Aesthetic & Action-Oriented Emergency Layout.
 */

#ifndef INDUSTRIAL_HMI_CONFIG_H
#define INDUSTRIAL_HMI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- Display & Elastic Band Buffer Specifications --- */
#define HMI_DISPLAY_WIDTH_DEFAULT   800
#define HMI_DISPLAY_HEIGHT_DEFAULT  480
#define HMI_DISPLAY_BPP             4      /* 4-bit Indexed: 16-color Apple industrial CLUT */

/* Maximum static band buffer: 19,200 bytes (18.75 KB) - fits entirely in CPU L1 data cache */
#define HMI_DISPLAY_MAX_BUF_SIZE    19200

/* Normalized Permille Coordinate System (0 to 1000‰ of screen width/height) */
#define HMI_PERMILLE_MAX            1000

/* Macro to convert permille to actual screen pixels */
#define HMI_NORM_TO_PIXEL(norm, total_px) (((norm) * (total_px)) / HMI_PERMILLE_MAX)

/* --- Timing Specifications --- */
#define HMI_FIELDBUS_FREQ_HZ        1000   /* 1,000 Hz / 1 ms fieldbus & sensor loop */
#define HMI_FIELDBUS_PERIOD_MS      (1000 / HMI_FIELDBUS_FREQ_HZ)
#define HMI_UI_FREQ_HZ              30     /* 30 Hz / ~33 ms UI refresh frame rate */
#define HMI_UI_FRAME_PERIOD_MS      (1000 / HMI_UI_FREQ_HZ)

/* --- Watchdog Supervisor Specifications --- */
#define HMI_WATCHDOG_MAX_AGE_MS     500    /* Trip threshold: 500 ms heartbeat silence */
#define HMI_WATCHDOG_TICK_MS        50     /* Independent watchdog cycle */

/* --- Capacities (100% Statically Bounded - Zero Heap) --- */
#define HMI_MAX_TAGS                64     /* Max concurrent PLC/hardware tags (512 bytes total) */
#define HMI_MAX_WIDGETS_PER_SCREEN  24     /* Max declarative widgets on one screen */
#define HMI_MAX_SCREENS             5      /* Process, Diagnostics, Telemetry, Alarms, Settings */
#define HMI_TREND_SAMPLES           60     /* 60 data points (60 seconds at 1 Hz) */
#define HMI_ALARM_JOURNAL_SIZE      16     /* Circular ISA-18.2 audit log */
#define HMI_INPUT_QUEUE_SIZE        16     /* Debounced input event circular buffer */

/* --- Ergonomics & Apple HIG Touch Targets --- */
#define HMI_MIN_TOUCH_TARGET_PX     48     /* ISO 9241 / Apple HIG minimum button size */

/* --- Apple-Inspired Industrial Color Palette Indices (4-bit CLUT) --- */
#define COLOR_OBSIDIAN_BG      0   /* #0B0F17 - Deep calming dark space */
#define COLOR_TITANIUM_CARD    1   /* #161F2E - Elevated card container */
#define COLOR_CARD_BORDER      2   /* #26334D - Subtle divider stroke */
#define COLOR_CHALK_WHITE      3   /* #F8FAFC - Primary high-contrast text */
#define COLOR_SILVER_MUTED     4   /* #94A3B8 - Secondary label text */
#define COLOR_DARK_GRAY        5   /* #334155 - Inactive bars / gridlines */
#define COLOR_CYAN_ACCENT      6   /* #00D2FF - Electric Apple Cyan */
#define COLOR_BLUE_ACCENT      7   /* #38BDF8 - Calm status blue */
#define COLOR_EMERALD_OK       8   /* #10B981 - Plant normal / nominal */
#define COLOR_AMBER_WARN       9   /* #F59E0B - Caution / high limit */
#define COLOR_CRIMSON_TRIP     10  /* #EF4444 - Critical trip / emergency */
#define COLOR_VIBRANT_ORANGE   11  /* #FB923C - Active setpoint */
#define COLOR_PURPLE_SECURE    12  /* #A855F7 - Role/Security badge */
#define COLOR_SURFACE_HOVER    13  /* #1E293B - Pressed button state */
#define COLOR_NEON_GREEN       14  /* #00FF66 - Digital run status */
#define COLOR_WHITE_PURE       15  /* #FFFFFF - Pure highlight */

/* --- Role-Based Access Control (RBAC - IEC 62443 Security) --- */
typedef enum {
    ROLE_VIEWER      = 0,  /* Read-only monitoring */
    ROLE_OPERATOR    = 1,  /* Acknowledge alarms, toggle basic equipment */
    ROLE_MAINTENANCE = 2,  /* Setpoint adjustments, diagnostic tests */
    ROLE_ENGINEER    = 3   /* Full configuration, firmware failover, calibration */
} hmi_role_t;

/* --- Standard Screen Identifiers --- */
typedef enum {
    SCREEN_PROCESS_OVERVIEW = 0, /* Primary PLC Process Overview (Tanks, Pumps, Valves) */
    SCREEN_FIELD_DIAGNOSTICS,   /* Fieldbus Comms, Tag Tables, I/O Status */
    SCREEN_HOST_TELEMETRY,      /* Host Hardware Telemetry (CPU, RAM, Disk, Fan) */
    SCREEN_ALARM_MATRIX,        /* ISA-18.2 Latching Alarm Matrix & Event Journal */
    SCREEN_SECURITY_SETTINGS,   /* RBAC Login, High-Contrast Toggle, Safe States */
    SCREEN_COUNT
} hmi_screen_id_t;

/* --- Unified Input Action Keys --- */
typedef enum {
    KEY_NONE = 0,
    KEY_NAV_PREV,
    KEY_NAV_NEXT,
    KEY_ACTION_ACK,            /* Acknowledge active alarm */
    KEY_ACTION_TOGGLE_CONTRAST,/* High-contrast outdoor theme */
    KEY_ACTION_EMERGENCY_STOP, /* E-Stop command to PLC */
    KEY_ACTION_PUMP_TOGGLE,    /* Toggle primary pump */
    KEY_ACTION_VALVE_TOGGLE,   /* Toggle control valve */
    KEY_ACTION_SIMULATE_FAULT, /* Simulate sensor/watchdog fault */
    KEY_ACTION_ROLE_CYCLE      /* Cycle RBAC role for testing */
} hmi_key_t;

/* --- Alarm Severity Levels (ISA-18.2 Compliant) --- */
typedef enum {
    ALARM_SEV_NONE     = 0,
    ALARM_SEV_INFO     = 1,
    ALARM_SEV_WARNING  = 2,
    ALARM_SEV_CRITICAL = 3,
    ALARM_SEV_ESTOP    = 4
} hmi_alarm_sev_t;

/* --- Alarm Latching State Machine (ISA-18.2) --- */
typedef enum {
    ALARM_LATCH_NORMAL = 0,        /* Normal: No trip condition */
    ALARM_LATCH_UNACKNOWLEDGED = 1,/* Active alarm, horn sounding, requires ACK */
    ALARM_LATCH_ACKNOWLEDGED = 2   /* Operator acknowledged, awaiting return to normal */
} hmi_alarm_latch_t;

#endif /* INDUSTRIAL_HMI_CONFIG_H */
