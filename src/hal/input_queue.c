/**
 * @file input_queue.c
 * @brief Static Debounced Circular Buffer Event Queue Implementation
 */

#include "../../include/hal/input_queue.h"
#include <string.h>

static hmi_key_t s_key_buf[HMI_INPUT_QUEUE_SIZE];
static volatile uint8_t s_key_head = 0;
static volatile uint8_t s_key_tail = 0;

static hmi_touch_event_t s_touch_buf[HMI_INPUT_QUEUE_SIZE];
static volatile uint8_t s_touch_head = 0;
static volatile uint8_t s_touch_tail = 0;

void hmi_input_init(void)
{
    s_key_head = 0;
    s_key_tail = 0;
    s_touch_head = 0;
    s_touch_tail = 0;
}

void hmi_input_queue_key(hmi_key_t key)
{
    uint8_t next_head = (s_key_head + 1) % HMI_INPUT_QUEUE_SIZE;
    if (next_head == s_key_tail) return; /* Drop if full */
    s_key_buf[s_key_head] = key;
    s_key_head = next_head;
}

void hmi_input_queue_touch(int16_t x, int16_t y, bool pressed)
{
    uint8_t next_head = (s_touch_head + 1) % HMI_INPUT_QUEUE_SIZE;
    if (next_head == s_touch_tail) return;
    s_touch_buf[s_touch_head].x = x;
    s_touch_buf[s_touch_head].y = y;
    s_touch_buf[s_touch_head].pressed = pressed;
    s_touch_head = next_head;
}

bool hmi_input_poll_key(hmi_key_t *out_key)
{
    if (s_key_head == s_key_tail) return false;
    if (out_key) *out_key = s_key_buf[s_key_tail];
    s_key_tail = (s_key_tail + 1) % HMI_INPUT_QUEUE_SIZE;
    return true;
}

bool hmi_input_poll_touch(hmi_touch_event_t *out_touch)
{
    if (s_touch_head == s_touch_tail) return false;
    if (out_touch) *out_touch = s_touch_buf[s_touch_tail];
    s_touch_tail = (s_touch_tail + 1) % HMI_INPUT_QUEUE_SIZE;
    return true;
}
