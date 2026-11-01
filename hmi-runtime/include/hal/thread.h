/**
 * @file thread.h
 * @brief Start / join a worker thread. No heap: the thread record lives in the caller's storage.
 *
 * thread_win32.c: CreateThread.  thread_posix.c: pthreads (Linux, Raspberry Pi, ESP-IDF).
 */

#ifndef INDUSTRIAL_HMI_HAL_THREAD_H
#define INDUSTRIAL_HMI_HAL_THREAD_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HAL_PRIO_LOW,       /* background work (discovery) */
    HAL_PRIO_NORMAL,
    HAL_PRIO_HIGH       /* the fieldbus loop; best effort, silently normal if the OS refuses */
} hal_prio_t;

typedef struct {
    uintptr_t handle;            /* OS thread handle */
    void    (*fn)(void *);
    void     *arg;
    bool      started;
} hal_thread_t;

/* stack_bytes is a minimum (0 = OS default). Small MCU targets need it set explicitly. */
bool hal_thread_start(hal_thread_t *t, void (*fn)(void *), void *arg, hal_prio_t prio, uint32_t stack_bytes);
/* Wait for the thread to end. POSIX ignores the timeout. Safe on a thread that never started. */
void hal_thread_join(hal_thread_t *t, uint32_t timeout_ms);

#endif /* INDUSTRIAL_HMI_HAL_THREAD_H */
