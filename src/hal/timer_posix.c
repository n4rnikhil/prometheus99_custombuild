/**
 * @file timer_posix.c
 * @brief Monotonic time base and drift-free periodic pacer on POSIX (Linux, Raspberry Pi, ESP-IDF)
 *
 * CLOCK_MONOTONIC never jumps with wall-clock changes. The pacer sleeps to an absolute
 * deadline (clock_nanosleep TIMER_ABSTIME) so the average rate does not drift.
 * On ESP-IDF the sleep granularity is one FreeRTOS tick: set CONFIG_FREERTOS_HZ=1000
 * for a 1 kHz fieldbus loop.
 */

#include <errno.h>
#include <time.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "../../include/hal/timer.h"

static uint64_t s_boot_us;

static uint64_t raw_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

void hmi_timer_init(void)     { s_boot_us = raw_us(); }
void hmi_timer_shutdown(void) {}

uint64_t hmi_timer_get_us(void)
{
    return raw_us() - s_boot_us;
}

uint32_t hmi_timer_get_ms(void)
{
    return (uint32_t)(hmi_timer_get_us() / 1000u);
}

void hmi_timer_sleep_ms(uint32_t ms)
{
#if defined(ESP_PLATFORM)
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);                      /* always yield: never starve the idle task */
#else
    struct timespec req, rem;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) req = rem;
#endif
}

bool hmi_pacer_init(hmi_pacer_t *p, uint32_t period_us)
{
    if (!p || period_us == 0) return false;
    p->handle = NULL;
    p->period_us = period_us;
    p->overruns = 0;
    p->high_res = true;
    p->next_deadline_us = hmi_timer_get_us() + period_us;
    return true;
}

static void sleep_until(uint64_t deadline_us)           /* deadline on the hmi_timer_get_us() scale */
{
#if defined(__linux__)
    uint64_t abs_us = s_boot_us + deadline_us;
    struct timespec ts;
    ts.tv_sec = (time_t)(abs_us / 1000000u);
    ts.tv_nsec = (long)(abs_us % 1000000u) * 1000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
#elif defined(ESP_PLATFORM)
    uint64_t now = hmi_timer_get_us();
    uint64_t wait_us = deadline_us > now ? deadline_us - now : 0;
    TickType_t ticks = (TickType_t)((wait_us + (uint64_t)portTICK_PERIOD_MS * 1000u - 1u) / ((uint64_t)portTICK_PERIOD_MS * 1000u));
    vTaskDelay(ticks ? ticks : 1);                      /* whole ticks: CONFIG_FREERTOS_HZ=1000 gives 1 ms */
#else
    uint64_t now = hmi_timer_get_us();
    if (deadline_us > now) {
        uint64_t us = deadline_us - now;
        struct timespec req;
        req.tv_sec = (time_t)(us / 1000000u);
        req.tv_nsec = (long)(us % 1000000u) * 1000L;
        nanosleep(&req, NULL);
    }
#endif
}

void hmi_pacer_wait(hmi_pacer_t *p)
{
    if (!p) return;
    uint64_t now = hmi_timer_get_us();

    if (now > p->next_deadline_us + 10u * (uint64_t)p->period_us) {
        /* Far behind (stall, debugger, suspend): re-base instead of bursting. */
        p->overruns++;
        p->next_deadline_us = now + p->period_us;
        return;
    }
    if (now < p->next_deadline_us) sleep_until(p->next_deadline_us);
    p->next_deadline_us += p->period_us;
}

void hmi_pacer_close(hmi_pacer_t *p)
{
    if (p) p->high_res = false;
}
