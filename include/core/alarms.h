/**
 * @file alarms.h
 * @brief ISA-18.2 Latching Alarm Supervisor & Emergency Action Advisor
 * @standard ISA-18.2 / IEC 62682 Management of Alarm Systems
 */

#ifndef INDUSTRIAL_HMI_ALARMS_H
#define INDUSTRIAL_HMI_ALARMS_H

#include "../config.h"
#include "tags.h"

/* --- ISA-18.2 Alarm Entry in Audit Journal --- */
typedef struct {
    uint32_t timestamp_ms;
    uint8_t  tag_id;
    uint8_t  severity;     /* hmi_alarm_sev_t */
    uint8_t  state;        /* hmi_alarm_latch_t */
    char     message[32];  /* Plain English description */
} hmi_alarm_record_t;

/* --- Emergency Action Guidance Block --- */
typedef struct {
    bool     active;
    uint8_t  tag_id;
    uint8_t  severity;
    char     title[48];       /* e.g. "REACTOR TEMP HIGH (84.2 C)" */
    char     action_step1[48];/* e.g. "CLOSE FEED VALVE V-101" */
    char     action_step2[48];/* e.g. "VERIFY COOLANT PUMP B ACTIVE" */
} hmi_emergency_action_t;

/* --- Alarm Supervisor State Container --- */
typedef struct {
    hmi_alarm_latch_t       latch_state;
    hmi_alarm_sev_t         highest_severity;
    bool                    horn_sounding;
    hmi_emergency_action_t  current_action;
    hmi_alarm_record_t      journal[HMI_ALARM_JOURNAL_SIZE];
    uint8_t                 journal_head;
    uint8_t                 journal_count;
} hmi_alarm_supervisor_t;

/* --- Alarm Supervisor API --- */
void hmi_alarms_init(hmi_alarm_supervisor_t *sup);
void hmi_alarms_evaluate(hmi_alarm_supervisor_t *sup, const hmi_tag_table_t *tags, uint32_t now_ms);

/* Operator Actions */
void hmi_alarms_silence_horn(hmi_alarm_supervisor_t *sup);
void hmi_alarms_acknowledge(hmi_alarm_supervisor_t *sup, uint32_t now_ms);
void hmi_alarms_trigger_estop(hmi_alarm_supervisor_t *sup, uint32_t now_ms);

const hmi_emergency_action_t* hmi_alarms_get_action(const hmi_alarm_supervisor_t *sup);
uint8_t hmi_alarms_get_journal(const hmi_alarm_supervisor_t *sup, const hmi_alarm_record_t **out_records);

#endif /* INDUSTRIAL_HMI_ALARMS_H */
