/**
 * @file alarms.h
 * @brief ISA-18.2 per-point alarm state machine, horn, operator guidance and journal
 * @standards ISA-18.2 / IEC 62682 Management of Alarm Systems
 *
 * Every registered tag is an alarm point with its own latched state:
 *
 *   NORMAL --condition--> UNACK --ack--> ACKED --clears--> NORMAL
 *                           |                \--escalates--> UNACK (re-annunciate)
 *                           \--clears--> RTN_UNACK --ack--> NORMAL
 *                                            \--condition returns--> UNACK
 *
 * Analog points: value >= trip_high -> CRITICAL, value >= warn_high -> WARNING.
 * A deadband of 1 % of span (min 1 raw unit) applies before a level clears.
 * Digital points: value != 0 -> meta->digital_alarm_sev (if non-zero).
 * Points whose quality is not GOOD/SIMULATED are not limit-checked (state frozen);
 * communication loss is reported by the driver through its own digital point.
 */

#ifndef INDUSTRIAL_HMI_ALARMS_H
#define INDUSTRIAL_HMI_ALARMS_H

#include "../config.h"
#include "tags.h"

/* --- Per-point state --- */
typedef enum {
    ALARM_PT_NORMAL    = 0,
    ALARM_PT_UNACK     = 1,  /* condition active, not acknowledged (horn) */
    ALARM_PT_ACKED     = 2,  /* condition active, acknowledged */
    ALARM_PT_RTN_UNACK = 3   /* condition cleared, never acknowledged (latched) */
} hmi_alarm_point_state_t;

typedef struct {
    uint8_t  state;          /* hmi_alarm_point_state_t */
    uint8_t  severity;       /* current condition severity (ALARM_SEV_NONE when clear) */
    uint8_t  peak_severity;  /* highest severity since the point left NORMAL */
    uint8_t  reserved;
    int16_t  trip_value;     /* raw value when the point last annunciated */
    int16_t  reserved2;
    uint32_t since_ms;       /* time of last state change */
} hmi_alarm_point_t;

/* --- Journal record --- */
typedef struct {
    uint32_t timestamp_ms;   /* uptime ms */
    uint32_t seq;            /* 1-based, monotonically increasing */
    uint8_t  tag_id;         /* 0xFF = system event */
    uint8_t  severity;       /* hmi_alarm_sev_t */
    uint8_t  state;          /* hmi_alarm_point_state_t after the event */
    char     message[57];    /* ASCII, NUL-terminated */
} hmi_alarm_record_t;

#define HMI_ALARM_TAG_SYSTEM 0xFFu

/* --- Operator guidance for the highest-priority alarm --- */
typedef struct {
    bool     active;
    uint8_t  tag_id;
    uint8_t  severity;
    uint8_t  state;           /* hmi_alarm_point_state_t of that point */
    char     title[96];       /* e.g. "TANK 01 LEVEL HI-HI 96.2 % (TRIP 95.0)" */
    char     action_step1[56];
    char     action_step2[56];
} hmi_emergency_action_t;

/* --- Supervisor --- */
typedef struct {
    hmi_alarm_point_t       points[HMI_MAX_TAGS];

    /* Summary (recomputed by every evaluate / acknowledge) */
    hmi_alarm_latch_t       latch_state;
    hmi_alarm_sev_t         highest_severity;  /* over points not in NORMAL */
    bool                    horn_sounding;     /* true while any point is UNACK/RTN_UNACK and not silenced */
    bool                    horn_silenced;     /* silence without ack; cleared by a new annunciation */
    uint16_t                active_count;      /* points with condition active */
    uint16_t                unacked_count;     /* points in UNACK or RTN_UNACK */
    hmi_emergency_action_t  current_action;

    /* In-memory journal (circular) */
    hmi_alarm_record_t      journal[HMI_ALARM_JOURNAL_SIZE];
    uint8_t                 journal_head;      /* next write index */
    uint8_t                 journal_count;     /* valid records (<= HMI_ALARM_JOURNAL_SIZE) */
    uint32_t                journal_total;     /* records ever written (== seq of newest) */
} hmi_alarm_supervisor_t;

/* --- API --- */
void hmi_alarms_init(hmi_alarm_supervisor_t *sup);

/* Scan every tag that has metadata; apply the state machine; rebuild the summary. */
void hmi_alarms_evaluate(hmi_alarm_supervisor_t *sup, const hmi_tag_table_t *tags, uint32_t now_ms);

/* Acknowledge every UNACK / RTN_UNACK point. Returns the number of points acknowledged. */
uint16_t hmi_alarms_acknowledge_all(hmi_alarm_supervisor_t *sup, uint32_t now_ms);

/* Forget the alarm points of a removed device (tags first_tag .. first_tag+count-1).
 * The journal keeps their history; the summary is rebuilt by the next evaluate. */
void hmi_alarms_clear_range(hmi_alarm_supervisor_t *sup, uint8_t first_tag, uint8_t count);

/* Silence the horn without acknowledging (a new annunciation re-arms it). */
void hmi_alarms_silence_horn(hmi_alarm_supervisor_t *sup);

/* Append a free-form journal event (operator commands, watchdog, comm events). */
void hmi_alarms_log_event(hmi_alarm_supervisor_t *sup, uint32_t now_ms, uint8_t tag_id,
                          uint8_t severity, uint8_t state, const char *msg);

const hmi_emergency_action_t* hmi_alarms_get_action(const hmi_alarm_supervisor_t *sup); /* NULL if none */

uint8_t hmi_alarms_journal_count(const hmi_alarm_supervisor_t *sup);
/* idx 0 = newest. NULL if idx >= count. */
const hmi_alarm_record_t* hmi_alarms_journal_newest(const hmi_alarm_supervisor_t *sup, uint8_t idx);
/* Record with the given seq if it is still in the buffer, else NULL. */
const hmi_alarm_record_t* hmi_alarms_journal_by_seq(const hmi_alarm_supervisor_t *sup, uint32_t seq);

/* Short labels for UI / log text */
const char* hmi_alarm_sev_name(uint8_t severity);     /* "NONE","INFO","WARN","CRIT","ESTOP" */
const char* hmi_alarm_state_name(uint8_t state);      /* "NORMAL","UNACK","ACKED","RTN-UNACK" */

#endif /* INDUSTRIAL_HMI_ALARMS_H */
