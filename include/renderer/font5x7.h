/**
 * @file font5x7.h
 * @brief Clean Micro-Font Interface (ASCII 32..127)
 */

#ifndef INDUSTRIAL_HMI_FONT5X7_H
#define INDUSTRIAL_HMI_FONT5X7_H

#include <stdint.h>

#define FONT5X7_WIDTH   5
#define FONT5X7_HEIGHT  7

const uint8_t* hmi_font_get_glyph(char c);

#endif /* INDUSTRIAL_HMI_FONT5X7_H */
