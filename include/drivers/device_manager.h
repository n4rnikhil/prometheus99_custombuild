/**
 * @file device_manager.h
 * @brief Plug-and-play device slots: attach, poll, act, and remove PLCs at runtime
 *
 * Threading: every function except hmi_devmgr_init() is called ONLY from the
 * fieldbus thread, so the manager needs no locking. Nothing blocks: each slot runs
 * its own non-blocking Modbus TCP state machine (or the built-in simulator).
 *
 * Lifecycle of a slot:
 *   EMPTY --attach--> CONNECTING --first good read--> ONLINE
 *   ONLINE/CONNECTING --link lost--> OFFLINE (comm-loss alarm, keeps reconnecting)
 *   OFFLINE --reconnected--> ONLINE
 *   OFFLINE --offline for remove_after_ms--> EMPTY (tags unbound, alarm points cleared,
 *            trend cleared, "removed" journal event)
 */

#ifndef INDUSTRIAL_HMI_DEVICE_MANAGER_H
#define INDUSTRIAL_HMI_DEVICE_MANAGER_H

#include "device_info.h"
#include "../core/alarms.h"

/* remove_after_ms: 0 = never remove offline devices */
void hmi_devmgr_init(uint32_t remove_after_ms);

/* Put the built-in tank-skid simulator in the first free slot. */
bool hmi_devmgr_attach_simulator(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* Attach a discovered device. false if it is already attached or no slot is free. */
bool hmi_devmgr_attach(const hmi_device_candidate_t *c, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

bool hmi_devmgr_is_attached(const char *host, uint16_t port, uint8_t unit);

/* Run every slot once (call every fieldbus iteration). */
void hmi_devmgr_poll(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* Execute action idx of the device in slot (already RBAC-authorised by the UI).
 * Logs the outcome to the journal. */
void hmi_devmgr_action(uint8_t slot, uint8_t action_idx, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* E-stop trip (true) or reset (false) on every device that has an E-stop coil.
 * Returns the number of devices the command was sent to. */
uint8_t hmi_devmgr_estop(bool trip, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* Copy the state of all slots (EMPTY ones included) for publishing. */
void hmi_devmgr_publish(hmi_device_info_t out[HMI_MAX_DEVICES], uint32_t now_ms);

uint8_t hmi_devmgr_count(uint8_t state_mask);   /* bit (1 << state) set = count that state */

void hmi_devmgr_shutdown(void);

#endif /* INDUSTRIAL_HMI_DEVICE_MANAGER_H */
