/**
 * @file gesture.h
 * @brief Tap / horizontal-swipe classifier for mouse and touch input
 *
 * Actions fire on release, never on press, so a swipe that starts on a
 * control can never operate it. Classification on release:
 *   SWIPE : |dx| >= max(60 px, 12 % of screen width), |dy| <= |dx| / 2,
 *           duration <= 800 ms. dx < 0 (finger moved left) = SWIPE_LEFT (next screen).
 *   TAP   : max travel from the press point seen during the gesture <= 12 px
 *           and duration <= 1000 ms. The tap position is the PRESS position.
 *   NONE  : anything else (drag, long press): no action.
 */

#ifndef INDUSTRIAL_HMI_GESTURE_H
#define INDUSTRIAL_HMI_GESTURE_H

#include <stdint.h>
#include <stdbool.h>

#define HMI_GESTURE_TAP_SLOP_PX        12
#define HMI_GESTURE_TAP_MAX_MS         1000
#define HMI_GESTURE_SWIPE_MIN_PX       60
#define HMI_GESTURE_SWIPE_MIN_PERMILLE 120
#define HMI_GESTURE_SWIPE_MAX_MS       800

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_RIGHT
} hmi_gesture_t;

typedef struct {
    bool     active;
    int32_t  x0, y0;        /* press position */
    int32_t  max_travel;    /* max Chebyshev distance from press seen so far */
    uint32_t t0_ms;
} hmi_gesture_tracker_t;

void hmi_gesture_begin(hmi_gesture_tracker_t *g, int32_t x, int32_t y, uint32_t now_ms);
void hmi_gesture_move(hmi_gesture_tracker_t *g, int32_t x, int32_t y);
/* Classify on release. For GESTURE_TAP, *tap_x / *tap_y receive the press position. */
hmi_gesture_t hmi_gesture_end(hmi_gesture_tracker_t *g, int32_t x, int32_t y, uint32_t now_ms,
                              int32_t screen_w, int32_t *tap_x, int32_t *tap_y);
void hmi_gesture_cancel(hmi_gesture_tracker_t *g);

#endif /* INDUSTRIAL_HMI_GESTURE_H */
