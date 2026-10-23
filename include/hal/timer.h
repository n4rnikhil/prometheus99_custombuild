/**
 * @file timer.h
 * @brief High-Resolution Precision System Timer
 * @fixes Fixes Windows 15.6 ms timer granularity bug via timeBeginPeriod(1)
 */

#ifndef INDUSTRIAL_HMI_TIMER_H
#define INDUSTRIAL_HMI_TIMER_H

#include <stdint.h>

void hmi_timer_init(void);
void hmi_timer_shutdown(void);
uint32_t hmi_timer_get_ms(void);
void hmi_timer_sleep_ms(uint32_t ms);

#endif /* INDUSTRIAL_HMI_TIMER_H */
