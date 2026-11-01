/**
 * @file atomic.h
 * @brief Sequentially consistent 32-bit atomics shared by the UI, fieldbus and discovery threads
 *
 * GCC / Clang (Windows, Linux, ESP32, Cortex-M with GCC): __atomic builtins.
 * MSVC: Interlocked intrinsics. The same hal_atomic_t word is used for run flags,
 * heartbeats, command bit words and the producer/consumer indices of the SPSC rings.
 */

#ifndef INDUSTRIAL_HMI_HAL_ATOMIC_H
#define INDUSTRIAL_HMI_HAL_ATOMIC_H

#include <stdint.h>

typedef volatile int32_t hal_atomic_t;

#if defined(_MSC_VER) && !defined(__clang__)
#include <windows.h>

static inline int32_t hal_atomic_load(const hal_atomic_t *a)
{
    return (int32_t)InterlockedCompareExchange((volatile LONG *)a, 0, 0);
}
static inline void hal_atomic_store(hal_atomic_t *a, int32_t v)
{
    (void)InterlockedExchange((volatile LONG *)a, (LONG)v);
}
static inline int32_t hal_atomic_exchange(hal_atomic_t *a, int32_t v)
{
    return (int32_t)InterlockedExchange((volatile LONG *)a, (LONG)v);
}
static inline int32_t hal_atomic_or(hal_atomic_t *a, int32_t bits)       /* returns the old value */
{
    return (int32_t)InterlockedOr((volatile LONG *)a, (LONG)bits);
}
static inline int32_t hal_atomic_add(hal_atomic_t *a, int32_t d)         /* returns the new value */
{
    return (int32_t)InterlockedExchangeAdd((volatile LONG *)a, (LONG)d) + d;
}

#else

static inline int32_t hal_atomic_load(const hal_atomic_t *a)
{
    return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}
static inline void hal_atomic_store(hal_atomic_t *a, int32_t v)
{
    __atomic_store_n(a, v, __ATOMIC_SEQ_CST);
}
static inline int32_t hal_atomic_exchange(hal_atomic_t *a, int32_t v)
{
    return __atomic_exchange_n(a, v, __ATOMIC_SEQ_CST);
}
static inline int32_t hal_atomic_or(hal_atomic_t *a, int32_t bits)       /* returns the old value */
{
    return __atomic_fetch_or(a, bits, __ATOMIC_SEQ_CST);
}
static inline int32_t hal_atomic_add(hal_atomic_t *a, int32_t d)         /* returns the new value */
{
    return __atomic_add_fetch(a, d, __ATOMIC_SEQ_CST);
}

#endif

#endif /* INDUSTRIAL_HMI_HAL_ATOMIC_H */
