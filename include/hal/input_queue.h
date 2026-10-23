/**
 * @file input_queue.h
 * @brief Static Debounced Circular Buffer Event Queue for Keys & Touch
 */

#ifndef INDUSTRIAL_HMI_INPUT_QUEUE_H
#define INDUSTRIAL_HMI_INPUT_QUEUE_H

#include "../config.h"

typedef struct {
    int16_t x;
    int16_t y;
    bool    pressed;
} hmi_touch_event_t;

void hmi_input_init(void);
void hmi_input_queue_key(hmi_key_t key);
void hmi_input_queue_touch(int16_t x, int16_t y, bool pressed);

bool hmi_input_poll_key(hmi_key_t *out_key);
bool hmi_input_poll_touch(hmi_touch_event_t *out_touch);

#endif /* INDUSTRIAL_HMI_INPUT_QUEUE_H */
