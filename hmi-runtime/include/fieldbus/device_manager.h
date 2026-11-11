/**
 * @file device_manager.h
 * @brief Plug-and-play device slots: attach, poll, act, and remove PLCs at runtime
 *
 * Threading: every function except hmi_devmgr_init() is called ONLY from the
 * fieldbus thread, so the manager needs no locking. Nothing blocks: each slot runs
 * its own non-blocking Modbus TCP state machine.
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

#include "fieldbus/device_info.h"
#include "core/alarms.h"

/* remove_after_ms: 0 = never remove offline devices */
void hmi_devmgr_init(uint32_t remove_after_ms);
/* Attach at most n devices (1..HMI_MAX_DEVICES; out of range = HMI_MAX_DEVICES). Call before
 * hmi_devmgr_init's caller starts attaching; devices beyond it are reported as not shown. */
void hmi_devmgr_set_limit(int n);
int  hmi_devmgr_limit(void);

/* In-process Modbus server: answers one request ADU, returns the reply length (0 = no reply). */
typedef size_t (*hmi_loopback_fn)(void *ctx, const uint8_t *req, size_t len, uint8_t *rsp, size_t cap);

/* Attach a device reached through fn instead of TCP (tests, embedded co-processors). It is
 * driven by the same state machine, module and Modbus frames as a TCP device. The identity
 * picks the module exactly like FC43 would, and non-PLC identities are refused - unless
 * driver_key names a module explicitly (like --plc <ip>@<key>). */
bool hmi_devmgr_attach_loopback(const char *vendor, const char *product, const char *app,
                                const char *driver_key, hmi_loopback_fn fn, void *ctx,
                                hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* The module a discovered device gets: forced with --plc <ip>@<key>, else by identity, else generic. */
const hmi_device_profile_t *hmi_devmgr_module_for(const hmi_device_candidate_t *c);

/* Attach a discovered device. false if it is already attached or no slot is free. */
bool hmi_devmgr_attach(const hmi_device_candidate_t *c, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

bool hmi_devmgr_is_attached(const char *host, uint16_t port, uint8_t unit);

/* Run every slot once (call every fieldbus iteration). */
void hmi_devmgr_poll(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* Execute action idx of the device in slot (already RBAC-authorised by the UI).
 * value: SET_REG = raw value to write, SET_ENUM = option index, ignored otherwise.
 * Interlocks (block_tag) and ranges are checked here again. Logs the outcome to the journal. */
void hmi_devmgr_action(uint8_t slot, uint8_t action_idx, int16_t value, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* E-stop trip (true) or reset (false) on every device that has an E-stop coil.
 * Returns the number of devices the command was sent to. */
uint8_t hmi_devmgr_estop(bool trip, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms);

/* STOP ALL: every attached device that can move is told to stop.
 *   - pending writes of each device (starts, setpoints, reset pulses) are cancelled first, so a
 *     command queued a moment earlier can never restart the machine after the stop
 *   - a pending reset pulse is completed (its release is sent first), never left half-way
 *   - a device with a stop latch gets the latch; any other device gets every Stop control of its
 *     module, and every mode selector declared stop_all is set to its option 0 ("Off")
 *   - devices that cannot be reached (offline) or have no stop control are logged as CRITICAL:
 *     the operator must stop them at the machine
 * Devices that cannot start anything (meters, passive devices) are not counted. Returns the devices stopped;
 * *not_stopped (optional) receives the devices that could not be stopped. */
/* The HMI heartbeat (to PLC programs that have one) is only sent while this is true: the app
 * passes the UI thread's liveness, so a frozen screen lets the PLC go to its safe state. */
void hmi_devmgr_heartbeat_enable(bool ok);
/* Devices seen on the network in the last 15 s that are not attached (every place in use, or kept
 * for a --plc device). STOP ALL cannot reach them and counts them as not stopped. */
uint8_t hmi_devmgr_unattached(uint32_t now_ms);

uint8_t hmi_devmgr_stop_all(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms, uint8_t *not_stopped);

/* While the EMERGENCY is active (call every fieldbus iteration): stop every online device that has
 * not been stopped yet - one that was offline and came back, or one that was attached since.
 * Each device's outcome is in hmi_device_info_t.stop_result. Returns the devices
 * stopped by this call. */
uint8_t hmi_devmgr_stop_pending(hmi_alarm_supervisor_t *alarms, uint32_t now_ms);
/* EMERGENCY reset: forget the stop outcomes (the machines stay stopped; nothing is restarted). */
void hmi_devmgr_stop_clear(void);

/* Copy the state of all slots (EMPTY ones included) for publishing. */
void hmi_devmgr_publish(hmi_device_info_t out[HMI_MAX_DEVICES], uint32_t now_ms);
/* The same for the places used so far only: writes out[0 .. n-1] and returns n. Entries at n and
 * beyond are untouched (callers keep them zero = EMPTY). Keeps unused places out of memory. */
int  hmi_devmgr_publish_used(hmi_device_info_t *out, uint32_t now_ms);


void hmi_devmgr_shutdown(void);

#endif /* INDUSTRIAL_HMI_DEVICE_MANAGER_H */
