/**
 * @file alarms.c
 * @brief ISA-18.2 Per-Point Alarm State Machine, Horn, Guidance, and Audit Journal
 * @standards ISA-18.2 / IEC 62682 Management of Alarm Systems
 */

#include "../../include/core/alarms.h"
#include <stdio.h>
#include <string.h>

void hmi_alarms_init(hmi_alarm_supervisor_t *sup)
{
    if (!sup) return;
    memset(sup, 0, sizeof(*sup));
    sup->latch_state = ALARM_LATCH_NORMAL;
    sup->highest_severity = ALARM_SEV_NONE;
}

const char* hmi_alarm_sev_name(uint8_t severity)
{
    switch (severity) {
        case ALARM_SEV_NONE:     return "NONE";
        case ALARM_SEV_INFO:     return "INFO";
        case ALARM_SEV_WARNING:  return "WARN";
        case ALARM_SEV_CRITICAL: return "CRIT";
        case ALARM_SEV_ESTOP:    return "ESTOP";
        default:                 return "UNKN";
    }
}

const char* hmi_alarm_state_name(uint8_t state)
{
    switch (state) {
        case ALARM_PT_NORMAL:    return "NORMAL";
        case ALARM_PT_UNACK:     return "UNACK";
        case ALARM_PT_ACKED:     return "ACKED";
        case ALARM_PT_RTN_UNACK: return "RTN-UNACK";
        default:                 return "UNKN";
    }
}

void hmi_alarms_log_event(hmi_alarm_supervisor_t *sup, uint32_t now_ms, uint8_t tag_id,
                          uint8_t severity, uint8_t state, const char *msg)
{
    if (!sup) return;

    sup->journal_total++;
    uint8_t idx = sup->journal_head;
    hmi_alarm_record_t *rec = &sup->journal[idx];

    rec->timestamp_ms = now_ms;
    rec->seq          = sup->journal_total;
    rec->tag_id       = tag_id;
    rec->severity     = severity;
    rec->state        = state;

    if (msg) {
        size_t i = 0;
        for (; msg[i] && i + 1 < sizeof(rec->message); i++) {
            rec->message[i] = msg[i];
        }
        rec->message[i] = '\0';
    } else {
        rec->message[0] = '\0';
    }

    sup->journal_head = (uint8_t)((idx + 1) % HMI_ALARM_JOURNAL_SIZE);
    if (sup->journal_count < HMI_ALARM_JOURNAL_SIZE) {
        sup->journal_count++;
    }
}

uint8_t hmi_alarms_journal_count(const hmi_alarm_supervisor_t *sup)
{
    return sup ? sup->journal_count : 0;
}

const hmi_alarm_record_t* hmi_alarms_journal_newest(const hmi_alarm_supervisor_t *sup, uint8_t idx)
{
    if (!sup || idx >= sup->journal_count) return NULL;
    int pos = (int)sup->journal_head - 1 - (int)idx;
    while (pos < 0) pos += HMI_ALARM_JOURNAL_SIZE;
    return &sup->journal[pos % HMI_ALARM_JOURNAL_SIZE];
}

const hmi_alarm_record_t* hmi_alarms_journal_by_seq(const hmi_alarm_supervisor_t *sup, uint32_t seq)
{
    if (!sup || sup->journal_count == 0) return NULL;
    for (uint8_t i = 0; i < sup->journal_count; i++) {
        const hmi_alarm_record_t *rec = &sup->journal[i];
        if (rec->seq == seq) return rec;
    }
    return NULL;
}

void hmi_alarms_silence_horn(hmi_alarm_supervisor_t *sup)
{
    if (!sup) return;
    sup->horn_silenced = true;
    sup->horn_sounding = false;
}

uint16_t hmi_alarms_acknowledge_all(hmi_alarm_supervisor_t *sup, uint32_t now_ms)
{
    if (!sup) return 0;
    uint16_t count = 0;

    for (uint8_t i = 0; i < HMI_MAX_TAGS; i++) {
        hmi_alarm_point_t *pt = &sup->points[i];
        if (pt->state == ALARM_PT_UNACK) {
            pt->state = ALARM_PT_ACKED;
            pt->since_ms = now_ms;
            count++;
        } else if (pt->state == ALARM_PT_RTN_UNACK) {
            pt->state = ALARM_PT_NORMAL;
            pt->peak_severity = ALARM_SEV_NONE;
            pt->since_ms = now_ms;
            count++;
        }
    }

    sup->horn_sounding = false;
    sup->horn_silenced = false;
    return count;
}

static void fmt_point(char *out, size_t cap, const hmi_tag_meta_t *meta, int16_t val)
{
    char num[16];
    hmi_format_scaled(num, sizeof(num), val, meta ? meta->decimals : 0);
    if (meta && meta->type == TAG_TYPE_DIGITAL) snprintf(out, cap, "%s", val ? "ON" : "OFF");
    else snprintf(out, cap, "%s %s", num, (meta && meta->unit) ? meta->unit : "");
}

static void build_guidance(hmi_emergency_action_t *act, uint8_t tag_id, uint8_t sev, uint8_t state, int16_t val)
{
    const hmi_tag_meta_t *meta = hmi_tag_get_meta(tag_id);
    const char *name = meta ? meta->name : "PROCESS VALUE";
    act->active = true;
    act->tag_id = tag_id;
    act->severity = sev;
    act->state = state;

    if (meta && meta->type == TAG_TYPE_DIGITAL) {
        snprintf(act->title, sizeof(act->title), "%s%s", name,
                 state == ALARM_PT_RTN_UNACK ? " cleared - acknowledge" : " active");
    } else {
        char v[24], lim[24];
        fmt_point(v, sizeof(v), meta, val);
        int16_t limit = meta ? (sev >= ALARM_SEV_CRITICAL ? meta->trip_high : meta->warn_high) : 0;
        fmt_point(lim, sizeof(lim), meta, limit);
        snprintf(act->title, sizeof(act->title), "%s %s  %s  (limit %s)", name,
                 sev >= ALARM_SEV_CRITICAL ? "high-high" : "high", v, lim);
    }
    snprintf(act->action_step1, sizeof(act->action_step1), "1. %s",
             (meta && meta->action1) ? meta->action1 : "Check the field device and its wiring");
    snprintf(act->action_step2, sizeof(act->action_step2), "2. %s",
             (meta && meta->action2) ? meta->action2 : "Acknowledge once the cause is understood");
}

void hmi_alarms_evaluate(hmi_alarm_supervisor_t *sup, const hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!sup || !tags) return;

    for (uint8_t id = 0; id < HMI_MAX_TAGS; id++) {
        const hmi_tag_meta_t *meta = hmi_tag_get_meta(id);
        if (!meta) continue;

        uint8_t q = hmi_tag_read_quality(tags, id);
        if (q != QUALITY_GOOD && q != QUALITY_SIMULATED) {
            /* Sensor/comm fault: freeze limit-checking for this tag */
            continue;
        }

        int16_t val = hmi_tag_read_value(tags, id);
        uint8_t cond_sev = ALARM_SEV_NONE;
        hmi_alarm_point_t *pt = &sup->points[id];

        if (meta->type == TAG_TYPE_DIGITAL) {
            if (val != 0 && meta->digital_alarm_sev != ALARM_SEV_NONE) {
                cond_sev = meta->digital_alarm_sev;
            }
        } else {
            /* Analog limit check with deadband */
            int16_t span = (int16_t)(meta->max_val - meta->min_val);
            int16_t deadband = (int16_t)(span / 100);
            if (deadband < 1) deadband = 1;

            if (meta->trip_high != 0 && val >= meta->trip_high) {
                cond_sev = ALARM_SEV_CRITICAL;
            } else if (pt->severity == ALARM_SEV_CRITICAL && meta->trip_high != 0 && val > (meta->trip_high - deadband)) {
                cond_sev = ALARM_SEV_CRITICAL;
            } else if (meta->warn_high != 0 && val >= meta->warn_high) {
                cond_sev = ALARM_SEV_WARNING;
            } else if (pt->severity == ALARM_SEV_WARNING && meta->warn_high != 0 && val > (meta->warn_high - deadband)) {
                cond_sev = ALARM_SEV_WARNING;
            }
        }

        /* ISA-18.2 State Transition */
        uint8_t prev_state = pt->state;
        pt->severity = cond_sev;

        if (prev_state == ALARM_PT_NORMAL) {
            if (cond_sev != ALARM_SEV_NONE) {
                pt->state = ALARM_PT_UNACK;
                pt->peak_severity = cond_sev;
                pt->trip_value = val;
                pt->since_ms = now_ms;
                sup->horn_silenced = false;

                char msg[41];
                char v[24]; fmt_point(v, sizeof(v), meta, val);
                if (meta->type == TAG_TYPE_DIGITAL) snprintf(msg, sizeof(msg), "%s active", meta->name);
                else snprintf(msg, sizeof(msg), "%s %s %s", meta->name, cond_sev >= ALARM_SEV_CRITICAL ? "HH" : "H", v);
                hmi_alarms_log_event(sup, now_ms, id, cond_sev, pt->state, msg);
            }
        } else if (prev_state == ALARM_PT_UNACK) {
            if (cond_sev != ALARM_SEV_NONE) {
                if (cond_sev > pt->peak_severity) {
                    pt->peak_severity = cond_sev;
                    pt->trip_value = val;
                    sup->horn_silenced = false;
                    char msg[41];
                    char v[24]; fmt_point(v, sizeof(v), meta, val);
                    snprintf(msg, sizeof(msg), "%s HH %s", meta->name, v);
                    hmi_alarms_log_event(sup, now_ms, id, cond_sev, pt->state, msg);
                }
            } else {
                pt->state = ALARM_PT_RTN_UNACK;
                char msg[41];
                snprintf(msg, sizeof(msg), "%s back to normal", meta->name);
                hmi_alarms_log_event(sup, now_ms, id, pt->peak_severity, pt->state, msg);
            }
        } else if (prev_state == ALARM_PT_ACKED) {
            if (cond_sev != ALARM_SEV_NONE) {
                if (cond_sev > pt->peak_severity) {
                    pt->state = ALARM_PT_UNACK;
                    pt->peak_severity = cond_sev;
                    pt->trip_value = val;
                    sup->horn_silenced = false;
                    char msg[41];
                    char v[24]; fmt_point(v, sizeof(v), meta, val);
                    snprintf(msg, sizeof(msg), "%s HH %s (re-alarm)", meta->name, v);
                    hmi_alarms_log_event(sup, now_ms, id, cond_sev, pt->state, msg);
                }
            } else {
                pt->state = ALARM_PT_NORMAL;
                pt->peak_severity = ALARM_SEV_NONE;
                char msg[41];
                snprintf(msg, sizeof(msg), "%s cleared", meta->name);
                hmi_alarms_log_event(sup, now_ms, id, ALARM_SEV_INFO, pt->state, msg);
            }
        } else if (prev_state == ALARM_PT_RTN_UNACK) {
            if (cond_sev != ALARM_SEV_NONE) {
                pt->state = ALARM_PT_UNACK;
                pt->peak_severity = cond_sev;
                pt->trip_value = val;
                sup->horn_silenced = false;
                char msg[41];
                char v[24]; fmt_point(v, sizeof(v), meta, val);
                snprintf(msg, sizeof(msg), "%s %s (re-alarm)", meta->name, v);
                hmi_alarms_log_event(sup, now_ms, id, cond_sev, pt->state, msg);
            }
        }
    }

    /* Summary evaluation */
    uint16_t active_count = 0;
    uint16_t unacked_count = 0;
    uint8_t  highest_sev = ALARM_SEV_NONE;
    int      best_guidance_tag = -1;
    uint8_t  best_guidance_sev = ALARM_SEV_NONE;

    for (uint8_t id = 0; id < HMI_MAX_TAGS; id++) {
        const hmi_alarm_point_t *pt = &sup->points[id];
        if (pt->state != ALARM_PT_NORMAL) {
            if (pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_RTN_UNACK) {
                unacked_count++;
            }
            if (pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_ACKED) {
                active_count++;
            }
            if (pt->peak_severity > highest_sev) {
                highest_sev = pt->peak_severity;
            }

            /* Prioritize unacknowledged and higher severity for emergency guidance */
            if (pt->peak_severity > best_guidance_sev ||
                (pt->peak_severity == best_guidance_sev && pt->state == ALARM_PT_UNACK)) {
                best_guidance_sev = pt->peak_severity;
                best_guidance_tag = id;
            }
        }
    }

    sup->active_count     = active_count;
    sup->unacked_count    = unacked_count;
    sup->highest_severity = (hmi_alarm_sev_t)highest_sev;

    if (unacked_count > 0 && !sup->horn_silenced) {
        sup->horn_sounding = true;
    } else {
        sup->horn_sounding = false;
    }

    if (highest_sev == ALARM_SEV_NONE) {
        sup->latch_state = ALARM_LATCH_NORMAL;
    } else if (unacked_count > 0) {
        sup->latch_state = ALARM_LATCH_UNACKNOWLEDGED;
    } else {
        sup->latch_state = ALARM_LATCH_ACKNOWLEDGED;
    }

    if (best_guidance_tag >= 0 && best_guidance_sev >= ALARM_SEV_WARNING) {
        const hmi_alarm_point_t *bpt = &sup->points[best_guidance_tag];
        build_guidance(&sup->current_action, (uint8_t)best_guidance_tag,
                       best_guidance_sev, bpt->state, bpt->trip_value);
    } else {
        sup->current_action.active = false;
    }
}

const hmi_emergency_action_t* hmi_alarms_get_action(const hmi_alarm_supervisor_t *sup)
{
    if (sup && sup->current_action.active) {
        return &sup->current_action;
    }
    return NULL;
}
