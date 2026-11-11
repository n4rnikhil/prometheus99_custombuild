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
 * Analog points: value >= trip_high -> CRITICAL, value >= warn_high -> WARNING. A limit of 0, or
 * one at or below min_val, is "no limit". A deadband of 1 % of span (min 1 raw unit) applies
 * before a level clears.
 * Digital points: value != 0 -> meta->digital_alarm_sev (if non-zero).
 * Points whose quality is not GOOD/SIMULATED are not limit-checked (state frozen: suppressed by
 * design while the data is bad); communication loss is reported by the driver through its own
 * digital point, so one lost device raises one alarm, not one per reading.
 *
 * Besides the four states, ISA-18.2 handling:
 *   Shelved         an operator hides one alarm for a limited time (HMI_ALARM_SHELVE_MS); it comes
 *                   back by itself. Out of service: maintenance hides it until returned.
 *                   Hidden points are still evaluated and journaled, but are not annunciated,
 *                   not counted, not sounded and not in the summary. Stop-priority points
 *                   (ALARM_SEV_ESTOP) can never be hidden.
 *   Flood           more than HMI_ALARM_FLOOD_ON annunciations in 10 minutes starts a flood, fewer
 *                   than HMI_ALARM_FLOOD_OFF ends it (both journaled). During a flood INFO alarms
 *                   do not sound the horn.
 *   Chattering      a point that annunciates HMI_ALARM_CHATTER_COUNT times within
 *                   HMI_ALARM_CHATTER_MS is flagged (journaled once) until it calms down.
 */

#ifndef INDUSTRIAL_HMI_ALARMS_H
#define INDUSTRIAL_HMI_ALARMS_H

#include "config.h"
#include "core/tags.h"

/* --- Per-point state --- */
typedef enum {
    ALARM_PT_NORMAL    = 0,
    ALARM_PT_UNACK     = 1,  /* condition active, not acknowledged (horn) */
    ALARM_PT_ACKED     = 2,  /* condition active, acknowledged */
    ALARM_PT_RTN_UNACK = 3   /* condition cleared, never acknowledged (latched) */
} hmi_alarm_point_state_t;

/* Point flags */
#define HMI_ALARM_F_SHELVED  0x01u   /* hidden by an operator until shelve_until_ms */
#define HMI_ALARM_F_OOS      0x02u   /* out of service: hidden by maintenance until returned */
#define HMI_ALARM_F_CHATTER  0x04u   /* annunciated too often recently */
#define HMI_ALARM_F_HIDDEN   (HMI_ALARM_F_SHELVED | HMI_ALARM_F_OOS)

typedef struct {
    uint8_t  state;          /* hmi_alarm_point_state_t */
    uint8_t  severity;       /* current condition severity (ALARM_SEV_NONE when clear) */
    uint8_t  peak_severity;  /* highest severity since the point left NORMAL */
    uint8_t  flags;          /* HMI_ALARM_F_* */
    int16_t  trip_value;     /* raw value when the point last annunciated */
    uint8_t  ann_count;      /* annunciations in the current chatter window */
    uint8_t  reserved;
    uint32_t since_ms;       /* time of last state change */
    uint32_t shelve_until_ms;/* SHELVED: returns by itself at this time */
    uint32_t ann_window_ms;  /* start of the chatter window */
} hmi_alarm_point_t;

/* A point the operator sees in the summary and hears */
static inline bool hmi_alarm_point_shown(const hmi_alarm_point_t *p)
{
    return p->state != 0 && !(p->flags & HMI_ALARM_F_HIDDEN);
}

/* --- Journal record --- */
typedef struct {
    uint32_t timestamp_ms;   /* uptime ms */
    uint32_t seq;            /* 1-based, monotonically increasing */
    hmi_tag_id_t tag_id;     /* HMI_ALARM_TAG_SYSTEM = system event */
    uint8_t  severity;       /* hmi_alarm_sev_t */
    uint8_t  state;          /* hmi_alarm_point_state_t after the event */
    char     message[57];    /* ASCII, NUL-terminated */
} hmi_alarm_record_t;

#define HMI_ALARM_TAG_SYSTEM 0xFFFFu

/* --- Operator guidance for the highest-priority alarm --- */
typedef struct {
    bool     active;
    hmi_tag_id_t tag_id;
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
    bool                    horn_sounding;     /* true while a shown point needs acknowledging and the horn is not silenced */
    bool                    horn_silenced;     /* silence without ack; cleared by a new annunciation */
    uint16_t                active_count;      /* shown points with condition active */
    uint16_t                unacked_count;     /* shown points in UNACK or RTN_UNACK */
    uint16_t                shelved_count;     /* hidden points (shelved or out of service) */
    uint16_t                chatter_count;     /* points flagged as chattering */
    uint16_t                points_in_use;     /* loops stop here: one past the highest point ever evaluated */
    hmi_emergency_action_t  current_action;

    /* ISA-18.2 alarm rate: annunciations per minute over the last 10 minutes */
    uint16_t                rate_bucket[10];
    uint32_t                rate_minute;       /* minute number (now_ms / 60000) of the newest bucket */
    uint16_t                rate_10min;        /* sum of the buckets */
    uint16_t                rate_peak_10min;   /* highest rate_10min seen */
    bool                    flood;             /* alarm flood in progress */
    uint32_t                flood_count;       /* floods since start */

    /* In-memory journal (circular) */
    hmi_alarm_record_t      journal[HMI_ALARM_JOURNAL_SIZE];
    uint16_t                journal_head;      /* next write index */
    uint16_t                journal_count;     /* valid records (<= HMI_ALARM_JOURNAL_SIZE) */
    uint32_t                journal_total;     /* records ever written (== seq of newest) */
} hmi_alarm_supervisor_t;

/* --- API --- */
void hmi_alarms_init(hmi_alarm_supervisor_t *sup);

/* Scan every tag that has metadata; apply the state machine; rebuild the summary. */
void hmi_alarms_evaluate(hmi_alarm_supervisor_t *sup, const hmi_tag_table_t *tags, uint32_t now_ms);

/* Acknowledge every shown UNACK / RTN_UNACK point. Returns the number of points acknowledged. */
uint16_t hmi_alarms_acknowledge_all(hmi_alarm_supervisor_t *sup, uint32_t now_ms);

/* Shelve one point for duration_ms, or take it out of service (until returned). Both are journaled.
 * Refused (false) for a point without metadata, an unknown tag, or a stop-priority point. */
bool hmi_alarms_shelve(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t duration_ms, uint32_t now_ms);
bool hmi_alarms_set_out_of_service(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, bool oos, uint32_t now_ms);
/* Unshelve / return to service: the point is shown again in its current state. */
bool hmi_alarms_unshelve(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t now_ms);
/* Can this point be hidden at all (not a stop-priority point)? */
bool hmi_alarms_can_hide(hmi_tag_id_t tag_id);
/* Hidden points (shelved first, then out of service), up to max; returns the number written. */
uint16_t hmi_alarms_hidden(const hmi_alarm_supervisor_t *sup, hmi_tag_id_t *out, uint16_t max);

/* Acknowledge one point (ISA-18.2: the operator acknowledges the alarm they have read).
 * true if the point was UNACK or RTN_UNACK. The horn is re-evaluated by the next evaluate. */
bool hmi_alarms_acknowledge_point(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t now_ms);

/* Alarm summary order (ISA-18.2): highest priority first, then unacknowledged before
 * acknowledged, then newest first. Writes up to max tag ids of shown points not in NORMAL
 * (shelved and out-of-service points are left out); returns the number written. The renderer and the touch handling both use this, so a row is always the same alarm. */
uint16_t hmi_alarms_sorted(const hmi_alarm_supervisor_t *sup, hmi_tag_id_t *out, uint16_t max);

/* Forget the alarm points of a removed device (tags first_tag .. first_tag+count-1).
 * The journal keeps their history; the summary is rebuilt by the next evaluate. */
void hmi_alarms_clear_range(hmi_alarm_supervisor_t *sup, hmi_tag_id_t first_tag, uint16_t count);

/* Silence the horn without acknowledging (a new annunciation re-arms it). */
void hmi_alarms_silence_horn(hmi_alarm_supervisor_t *sup);

/* Append a free-form journal event (operator commands, watchdog, comm events). */
void hmi_alarms_log_event(hmi_alarm_supervisor_t *sup, uint32_t now_ms, hmi_tag_id_t tag_id,
                          uint8_t severity, uint8_t state, const char *msg);


uint16_t hmi_alarms_journal_count(const hmi_alarm_supervisor_t *sup);
/* idx 0 = newest. NULL if idx >= count. */
const hmi_alarm_record_t* hmi_alarms_journal_newest(const hmi_alarm_supervisor_t *sup, uint16_t idx);
/* Record with the given seq if it is still in the buffer, else NULL. */
const hmi_alarm_record_t* hmi_alarms_journal_by_seq(const hmi_alarm_supervisor_t *sup, uint32_t seq);

/* Short labels for UI / log text */
const char* hmi_alarm_sev_name(uint8_t severity);     /* "NONE","INFO","WARN","CRIT","ESTOP" */
const char* hmi_alarm_state_name(uint8_t state);      /* "NORMAL","UNACK","ACKED","RTN-UNACK" */

#endif /* INDUSTRIAL_HMI_ALARMS_H */
