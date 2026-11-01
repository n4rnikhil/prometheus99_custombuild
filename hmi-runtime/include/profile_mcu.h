/**
 * @file profile_mcu.h
 * @brief Build profile for microcontroller targets (ESP32-S3 class), selected with -DHMI_PROFILE_MCU
 *
 * Same code, smaller numbers: fewer device slots, fewer parallel discovery sockets (lwIP has a
 * small socket table), a 500 Hz fieldbus loop (FreeRTOS tick = 1 ms) and a 20 Hz display.
 * Any value can still be overridden with -D.
 */

#ifndef INDUSTRIAL_HMI_PROFILE_MCU_H
#define INDUSTRIAL_HMI_PROFILE_MCU_H

#ifndef HMI_FIELDBUS_FREQ_HZ
#define HMI_FIELDBUS_FREQ_HZ    500
#endif
#ifndef HMI_UI_FREQ_HZ
#define HMI_UI_FREQ_HZ          20
#endif
#ifndef HMI_MAX_DEVICES
#define HMI_MAX_DEVICES         2
#endif
#ifndef HMI_DISCOVERY_BATCH
#define HMI_DISCOVERY_BATCH     6
#endif
#ifndef HMI_ALARM_LOG_MAX_BYTES
#define HMI_ALARM_LOG_MAX_BYTES (256u * 1024u)
#endif
#ifndef HMI_STACK_FIELDBUS
#define HMI_STACK_FIELDBUS      (8u * 1024u)
#endif
#ifndef HMI_STACK_DISCOVERY
#define HMI_STACK_DISCOVERY     (6u * 1024u)
#endif

#endif /* INDUSTRIAL_HMI_PROFILE_MCU_H */
