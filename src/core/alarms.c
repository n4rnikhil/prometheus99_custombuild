/**
 * @file alarms.c
 * @brief ISA-18.2 Latching Alarm Supervisor & Emergency Guidance Implementation
 */

#include "../../include/core/alarms.h"
#include <string.h>
#include <stdio.h>

static void log_alarm_event(hmi_alarm_supervisor_t *sup, uint32_t now_ms, uint8_t tag_id,
                            uint8_t sev, uint8_t state, const char *msg)
{
    hmi_alarm_record_t *rec = &sup->journal[sup->journal_head];
    rec->timestamp_ms = now_ms;
    rec->tag_id = tag_id;
    rec->severity = sev;
    rec->state = state;
    memset(rec->message, 0, sizeof(rec->message));
    if (msg) {
        strncpy(rec->message, msg, sizeof(rec->message) - 1);
    }
    sup->journal_head = (sup->journal_head + 1) % HMI_ALARM_JOURNAL_SIZE;
    if (sup->journal_count < HMI_ALARM_JOURNAL_SIZE) {
        sup->journal_count++;
    }
}

void hmi_alarms_init(hmi_alarm_supervisor_t *sup)
{
    if (!sup) return;
    memset(sup, 0, sizeof(hmi_alarm_supervisor_t));
    sup->latch_state = ALARM_LATCH_NORMAL;
    sup->highest_severity = ALARM_SEV_NONE;
    sup->horn_sounding = false;
}

void hmi_alarms_evaluate(hmi_alarm_supervisor_t *sup, const hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!sup || !tags) return;

    hmi_alarm_sev_t max_sev = ALARM_SEV_NONE;
    uint8_t tripping_tag = 0;
    int16_t trip_val = 0;
    const hmi_tag_meta_t *trip_meta = NULL;

    /* Scan registered tags against limits */
    for (uint8_t i = 0; i < HMI_MAX_TAGS; i++) {
        const hmi_tag_meta_t *meta = hmi_tag_get_meta(i);
        if (!meta) continue;

        int16_t v = tags->tags[i].value;

        /* Critical High Trip Check */
        if (meta->trip_high > meta->min_val && v >= meta->trip_high) {
            max_sev = ALARM_SEV_CRITICAL;
            tripping_tag = i;
            trip_val = v;
            trip_meta = meta;
            break;
        }
        /* Warning High Check */
        else if (meta->warn_high > meta->min_val && v >= meta->warn_high) {
            if (max_sev < ALARM_SEV_WARNING) {
                max_sev = ALARM_SEV_WARNING;
                tripping_tag = i;
                trip_val = v;
                trip_meta = meta;
            }
        }
    }

    sup->highest_severity = max_sev;

    /* ISA-18.2 Latching State Machine */
    switch (sup->latch_state) {
        case ALARM_LATCH_NORMAL:
            if (max_sev >= ALARM_SEV_WARNING) {
                sup->latch_state = ALARM_LATCH_UNACKNOWLEDGED;
                sup->horn_sounding = true;
                sup->current_action.active = true;
                sup->current_action.tag_id = tripping_tag;
                sup->current_action.severity = max_sev;

                /* Formulate Calm Action-Oriented Advice (Apple HIG) */
                snprintf(sup->current_action.title, sizeof(sup->current_action.title),
                         "▲ %s HIGH: %d %s (TRIP %d)",
                         trip_meta ? trip_meta->name : "PROCESS",
                         trip_val, trip_meta ? trip_meta->unit : "",
                         trip_meta ? trip_meta->trip_high : 0);
                snprintf(sup->current_action.action_step1, sizeof(sup->current_action.action_step1),
                         "STEP 1: CLOSE FEED VALVE V-101");
                snprintf(sup->current_action.action_step2, sizeof(sup->current_action.action_step2),
                         "STEP 2: VERIFY EMERGENCY COOLING PUMP B");

                log_alarm_event(sup, now_ms, tripping_tag, max_sev, ALARM_LATCH_UNACKNOWLEDGED,
                                sup->current_action.title);
            }
            break;

        case ALARM_LATCH_UNACKNOWLEDGED:
            /* Remains unacknowledged and horn sounds until operator clicks ACK */
            if (max_sev == ALARM_SEV_NONE) {
                /* Even if condition clears, operator must know it occurred */
                strncpy(sup->current_action.title, "AUTOCLEARED - AWAITING OPERATOR ACK",
                        sizeof(sup->current_action.title) - 1);
            }
            break;

        case ALARM_LATCH_ACKNOWLEDGED:
            /* Operator acknowledged. Horn is silent. Awaiting return to normal. */
            if (max_sev == ALARM_SEV_NONE) {
                sup->latch_state = ALARM_LATCH_NORMAL;
                sup->current_action.active = false;
                log_alarm_event(sup, now_ms, 0, ALARM_SEV_NONE, ALARM_LATCH_NORMAL, "ALL CONDITIONS CLEARED");
            }
            break;
    }
}

void hmi_alarms_silence_horn(hmi_alarm_supervisor_t *sup)
{
    if (sup) sup->horn_sounding = false;
}

void hmi_alarms_acknowledge(hmi_alarm_supervisor_t *sup, uint32_t now_ms)
{
    if (!sup) return;
    if (sup->latch_state == ALARM_LATCH_UNACKNOWLEDGED) {
        sup->latch_state = ALARM_LATCH_ACKNOWLEDGED;
        sup->horn_sounding = false;
        log_alarm_event(sup, now_ms, sup->current_action.tag_id, sup->highest_severity,
                        ALARM_LATCH_ACKNOWLEDGED, "OPERATOR ACKNOWLEDGED");
    }
}

void hmi_alarms_trigger_estop(hmi_alarm_supervisor_t *sup, uint32_t now_ms)
{
    if (!sup) return;
    sup->latch_state = ALARM_LATCH_UNACKNOWLEDGED;
    sup->highest_severity = ALARM_SEV_ESTOP;
    sup->horn_sounding = true;
    sup->current_action.active = true;
    snprintf(sup->current_action.title, sizeof(sup->current_action.title), "■ EMERGENCY STOP TRIGGERED");
    snprintf(sup->current_action.action_step1, sizeof(sup->current_action.action_step1), "ALL MOTORS DE-ENERGIZED");
    snprintf(sup->current_action.action_step2, sizeof(sup->current_action.action_step2), "RELIEVE PRESSURE SAFELY");
    log_alarm_event(sup, now_ms, 0, ALARM_SEV_ESTOP, ALARM_LATCH_UNACKNOWLEDGED, "E-STOP ENGAGED");
}

const hmi_emergency_action_t* hmi_alarms_get_action(const hmi_alarm_supervisor_t *sup)
{
    if (!sup || !sup->current_action.active) return NULL;
    return &sup->current_action;
}

uint8_t hmi_alarms_get_journal(const hmi_alarm_supervisor_t *sup, const hmi_alarm_record_t **out_records)
{
    if (!sup || !out_records) return 0;
    *out_records = sup->journal;
    return sup->journal_count;
}
