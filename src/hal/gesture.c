/**
 * @file gesture.c
 * @brief Tap / horizontal-swipe classifier (actions fire on release)
 */

#include "../../include/hal/gesture.h"

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

static int32_t chebyshev(int32_t dx, int32_t dy)
{
    int32_t ax = iabs32(dx), ay = iabs32(dy);
    return ax > ay ? ax : ay;
}

void hmi_gesture_begin(hmi_gesture_tracker_t *g, int32_t x, int32_t y, uint32_t now_ms)
{
    if (!g) return;
    g->active = true;
    g->x0 = x;
    g->y0 = y;
    g->max_travel = 0;
    g->t0_ms = now_ms;
}

void hmi_gesture_move(hmi_gesture_tracker_t *g, int32_t x, int32_t y)
{
    if (!g || !g->active) return;
    int32_t d = chebyshev(x - g->x0, y - g->y0);
    if (d > g->max_travel) g->max_travel = d;
}

hmi_gesture_t hmi_gesture_end(hmi_gesture_tracker_t *g, int32_t x, int32_t y, uint32_t now_ms,
                              int32_t screen_w, int32_t *tap_x, int32_t *tap_y)
{
    if (!g || !g->active) return GESTURE_NONE;
    hmi_gesture_move(g, x, y);
    g->active = false;

    uint32_t dur = now_ms - g->t0_ms;              /* wrap-safe */
    int32_t dx = x - g->x0;
    int32_t dy = y - g->y0;

    int32_t min_swipe = (screen_w > 0) ? (screen_w * HMI_GESTURE_SWIPE_MIN_PERMILLE) / 1000 : 0;
    if (min_swipe < HMI_GESTURE_SWIPE_MIN_PX) min_swipe = HMI_GESTURE_SWIPE_MIN_PX;

    if (iabs32(dx) >= min_swipe && iabs32(dy) * 2 <= iabs32(dx) && dur <= HMI_GESTURE_SWIPE_MAX_MS) {
        return (dx < 0) ? GESTURE_SWIPE_LEFT : GESTURE_SWIPE_RIGHT;
    }
    if (g->max_travel <= HMI_GESTURE_TAP_SLOP_PX && dur <= HMI_GESTURE_TAP_MAX_MS) {
        if (tap_x) *tap_x = g->x0;
        if (tap_y) *tap_y = g->y0;
        return GESTURE_TAP;
    }
    return GESTURE_NONE;
}

void hmi_gesture_cancel(hmi_gesture_tracker_t *g)
{
    if (g) g->active = false;
}
