/**
 * @file thread_posix.c
 * @brief hal/thread.h on pthreads (Linux, Raspberry Pi, ESP-IDF)
 *
 * HAL_PRIO_HIGH asks for SCHED_FIFO (needs root or CAP_SYS_NICE on Linux; on refusal the thread
 * simply runs at normal priority). HAL_PRIO_LOW lowers the nice value of the calling thread on Linux.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#endif

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "../../include/hal/thread.h"

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 4096
#endif

static void *trampoline(void *p)
{
    hal_thread_t *t = (hal_thread_t *)p;
    t->fn(t->arg);
    return NULL;
}

static void *trampoline_low(void *p)
{
#if defined(__linux__) && defined(SYS_gettid)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    return trampoline(p);
}

bool hal_thread_start(hal_thread_t *t, void (*fn)(void *), void *arg, hal_prio_t prio, uint32_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_t th;
    if (!t || !fn) return false;
    t->fn = fn;
    t->arg = arg;
    t->started = false;

#if defined(ESP_PLATFORM)
    /* FreeRTOS: priority, stack and core come from the esp_pthread configuration. The fieldbus
     * loop gets its own core (1) on dual-core chips; the UI and Wi-Fi stay on core 0. */
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = stack_bytes ? stack_bytes : 8192u;
    cfg.prio = prio == HAL_PRIO_HIGH ? 10 : prio == HAL_PRIO_LOW ? 2 : 5;
    cfg.thread_name = prio == HAL_PRIO_HIGH ? "hmi_fieldbus" : "hmi_worker";
    cfg.pin_to_core = (prio == HAL_PRIO_HIGH && portNUM_PROCESSORS > 1) ? 1 : tskNO_AFFINITY;
    esp_pthread_set_cfg(&cfg);
    if (pthread_create(&th, NULL, trampoline, t) != 0) return false;
    memcpy(&t->handle, &th, sizeof(th) < sizeof(t->handle) ? sizeof(th) : sizeof(t->handle));
    t->started = true;
    return true;
#endif

    if (pthread_attr_init(&attr) != 0) return false;
    if (stack_bytes) {
        size_t sz = stack_bytes < PTHREAD_STACK_MIN ? PTHREAD_STACK_MIN : stack_bytes;
        pthread_attr_setstacksize(&attr, sz);
    }

    int rc = -1;
    if (prio == HAL_PRIO_HIGH) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 50;
        if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0 &&
            pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == 0 &&
            pthread_attr_setschedparam(&attr, &sp) == 0) {
            rc = pthread_create(&th, &attr, trampoline, t);      /* EPERM without privileges */
        }
        if (rc != 0) {                                            /* fall back to normal priority */
            pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
            rc = pthread_create(&th, &attr, trampoline, t);
        }
    } else {
        rc = pthread_create(&th, &attr, prio == HAL_PRIO_LOW ? trampoline_low : trampoline, t);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    memcpy(&t->handle, &th, sizeof(th) < sizeof(t->handle) ? sizeof(th) : sizeof(t->handle));
    t->started = true;
    return true;
}

void hal_thread_join(hal_thread_t *t, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!t || !t->started) return;
    pthread_t th;
    memset(&th, 0, sizeof(th));
    memcpy(&th, &t->handle, sizeof(th) < sizeof(t->handle) ? sizeof(th) : sizeof(t->handle));
    pthread_join(th, NULL);
    t->started = false;
}
