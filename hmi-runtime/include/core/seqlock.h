/**
 * @file seqlock.h
 * @brief Single-writer sequence lock for publishing a struct from the fieldbus thread to the UI
 *
 * Writer (exactly ONE thread):  write_begin(); copy data; write_end();
 * Reader (any thread):
 *     do { s = read_begin(); copy data; } while (read_retry(s));
 *
 * read_begin() spins while a write is in progress (odd sequence); read_retry()
 * reports whether a write overlapped the copy. The fences follow the standard
 * C11 seqlock pattern (H. Boehm, "Can seqlocks get along with programming
 * language memory models?", 2012) so the scheme is also correct on weakly
 * ordered CPUs (ARM64), not only on x86.
 */

#ifndef INDUSTRIAL_HMI_SEQLOCK_H
#define INDUSTRIAL_HMI_SEQLOCK_H

#include <stdint.h>
#include <stdbool.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <windows.h>
#include <intrin.h>
#define HMI_SEQ_FULL_FENCE()  MemoryBarrier()
#endif

typedef struct {
    volatile uint32_t sequence;
} hmi_seqlock_t;

static inline void hmi_seqlock_init(hmi_seqlock_t *lock)
{
    if (lock) lock->sequence = 0;
}

static inline void hmi_seqlock_cpu_relax(void)
{
#if defined(_MSC_VER) && !defined(__clang__)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

static inline void hmi_seqlock_write_begin(hmi_seqlock_t *lock)
{
#if defined(_MSC_VER) && !defined(__clang__)
    InterlockedIncrement((volatile LONG *)&lock->sequence);   /* full barrier */
#else
    uint32_t s = __atomic_load_n(&lock->sequence, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->sequence, s + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* odd sequence ordered before data stores */
#endif
}

static inline void hmi_seqlock_write_end(hmi_seqlock_t *lock)
{
#if defined(_MSC_VER) && !defined(__clang__)
    InterlockedIncrement((volatile LONG *)&lock->sequence);   /* full barrier */
#else
    uint32_t s = __atomic_load_n(&lock->sequence, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->sequence, s + 1u, __ATOMIC_RELEASE); /* data stores ordered before even sequence */
#endif
}

static inline uint32_t hmi_seqlock_read_begin(const hmi_seqlock_t *lock)
{
    uint32_t seq;
    for (;;) {
#if defined(_MSC_VER) && !defined(__clang__)
        seq = lock->sequence;
        HMI_SEQ_FULL_FENCE();
#else
        seq = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
#endif
        if ((seq & 1u) == 0u) {
            break;
        }
        hmi_seqlock_cpu_relax();
    }
    return seq;
}

static inline bool hmi_seqlock_read_retry(const hmi_seqlock_t *lock, uint32_t start_seq)
{
#if defined(_MSC_VER) && !defined(__clang__)
    HMI_SEQ_FULL_FENCE();
    return (lock->sequence != start_seq);
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* data loads ordered before the re-check */
    return (__atomic_load_n(&lock->sequence, __ATOMIC_RELAXED) != start_seq);
#endif
}

#endif /* INDUSTRIAL_HMI_SEQLOCK_H */
