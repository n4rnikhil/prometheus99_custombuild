/**
 * @file board.h
 * @brief Pin map and panel timing of the target board
 *
 * Default: Waveshare ESP32-S3-Touch-LCD-4.3 (800 x 480 RGB565 panel, GT911 touch on I2C).
 * These values follow that board's published example code; check them against your board's
 * schematic. Any ESP32-S3 board with a 16-bit RGB panel and a GT911 works after editing this file.
 * Boards that drive backlight or touch reset through an IO expander need board_init() in
 * main_esp32.c filled in.
 */

#ifndef HMI_BOARD_H
#define HMI_BOARD_H

#define BOARD_LCD_H_RES        800
#define BOARD_LCD_V_RES        480
#define BOARD_LCD_PCLK_HZ      (16 * 1000 * 1000)
#define BOARD_LCD_HSYNC_PULSE  4
#define BOARD_LCD_HSYNC_BACK   8
#define BOARD_LCD_HSYNC_FRONT  8
#define BOARD_LCD_VSYNC_PULSE  4
#define BOARD_LCD_VSYNC_BACK   8
#define BOARD_LCD_VSYNC_FRONT  8
#define BOARD_LCD_PCLK_NEG     1

#define BOARD_PIN_HSYNC        46
#define BOARD_PIN_VSYNC        3
#define BOARD_PIN_DE           5
#define BOARD_PIN_PCLK         7
#define BOARD_PIN_DISP         (-1)
/* RGB565 data lines D0..D15 = B3..B7, G2..G7, R3..R7 */
#define BOARD_PIN_DATA         { 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 }

#define BOARD_TOUCH_SDA        8
#define BOARD_TOUCH_SCL        9
#define BOARD_TOUCH_I2C_PORT   0

#endif /* HMI_BOARD_H */
