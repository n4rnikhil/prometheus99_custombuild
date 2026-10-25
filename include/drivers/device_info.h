/**
 * @file device_info.h
 * @brief Published state of each plug-and-play device slot (portable, read by the UI)
 */

#ifndef INDUSTRIAL_HMI_DEVICE_INFO_H
#define INDUSTRIAL_HMI_DEVICE_INFO_H

#include "../config.h"
#include "device_profile.h"
#include "modbus_proto.h"

typedef enum {
    HMI_DEV_EMPTY = 0,      /* slot free */
    HMI_DEV_CONNECTING,     /* found, first connection in progress */
    HMI_DEV_ONLINE,         /* answering */
    HMI_DEV_OFFLINE         /* lost; reconnecting; removed after the remove timeout */
} hmi_device_state_t;

typedef enum {
    HMI_TRANSPORT_SIM = 0,  /* built-in process simulator */
    HMI_TRANSPORT_TCP = 1   /* Modbus TCP */
} hmi_transport_t;

typedef struct {
    uint8_t  state;             /* hmi_device_state_t */
    uint8_t  slot;              /* 0..HMI_MAX_DEVICES-1, tag block HMI_DEV_TAG(slot, 0) */
    uint8_t  transport;         /* hmi_transport_t */
    uint8_t  unit_id;
    const hmi_device_profile_t *profile;   /* static const data: safe to read from any thread */
    char     label[24];         /* "Tank skid 2" */
    char     endpoint[24];      /* "127.0.0.1:1502" or "Simulator" */
    char     vendor[32];        /* from FC43, "" if the device did not identify itself */
    char     product[32];
    uint32_t since_ms;          /* time of the last state change */
    uint32_t remove_in_ms;      /* OFFLINE: time left before removal (0 = not scheduled) */
    uint32_t requests, responses, timeouts, exceptions, protocol_errors;
    uint32_t connects, writes_ok, writes_failed;
    uint32_t rtt_us, max_rtt_us;
} hmi_device_info_t;

/* A device found by discovery, handed to the device manager */
typedef struct {
    char           host[16];    /* dotted IPv4 */
    uint16_t       port;
    uint8_t        unit;
    bool           identified;  /* answered FC43 */
    bool           explicit_target;  /* configured by the user (--plc): attach even without a known driver */
    mb_device_id_t id;
} hmi_device_candidate_t;

static inline bool hmi_device_present(const hmi_device_info_t *d)
{
    return d && d->state != HMI_DEV_EMPTY && d->profile != NULL;
}

#endif /* INDUSTRIAL_HMI_DEVICE_INFO_H */
