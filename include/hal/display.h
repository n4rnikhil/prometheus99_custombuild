/**
 * @file display.h
 * @brief Display Driver Flush Interface
 */

#ifndef INDUSTRIAL_HMI_DISPLAY_H
#define INDUSTRIAL_HMI_DISPLAY_H

#include "../config.h"

void hmi_display_init(void);
void hmi_display_flush_band(int x, int y, int w, int h, const uint8_t *pixels_4bit, const uint32_t *palette16);

#endif /* INDUSTRIAL_HMI_DISPLAY_H */
