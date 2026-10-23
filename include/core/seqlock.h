/**
 * @file seqlock.h
 * @brief Lock-Free Atomic Sequence Lock for Real-Time Cross-Thread State Exchange
 * @standard Zero torn-reads, zero mutex jitter, zero priority inversion
 *
 * Writer (1000 Hz Fieldbus Loop):
 *   hmi_seqlock_write_begin(&lock);
 *   // copy new state data
 *   hmi_seqlock_write_end(&lock);
 *
 * Reader (30 Hz UI Frame Loop):
 *   do {
 *       seq = hmi_seqlock_read_begin(&lock);
 *       // copy state data to local buffer
 *   } while (hmi_seqlock_read_retry(&lock, seq));
 */

#ifndef INDUSTRIAL_HMI_SEQLOCK_H
#define INDUSTRIAL_HMI_SEQLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    volatile uint32_t sequence;
} hmi_seqlock_t;

static inline void hmi_seqlock_init(hmi_seqlock_t *lock)
{
    if (lock) lock->sequence = 0;
}

static inline void hmi_seqlock_write_begin(hmi_seqlock_t *lock)
{
    /* Increment sequence to odd (signaling write in progress) */
    lock->sequence++;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__GNUC__)
    __sync_synchronize();
#endif
}

static inline void hmi_seqlock_write_end(hmi_seqlock_t *lock)
{
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__GNUC__)
    __sync_synchronize();
#endif
    /* Increment sequence to even (signaling stable completed frame) */
    lock->sequence++;
}

static inline uint32_t hmi_seqlock_read_begin(const hmi_seqlock_t *lock)
{
    uint32_t seq;
    for (;;) {
        seq = lock->sequence;
        /* If odd, a write is currently in progress; spin until stable */
        if ((seq & 1) == 0) {
            break;
        }
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__GNUC__)
        __builtin_ia32_pause();
#endif
    }
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__GNUC__)
    __sync_synchronize();
#endif
    return seq;
}

static inline bool hmi_seqlock_read_retry(const hmi_seqlock_t *lock, uint32_t start_seq)
{
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__GNUC__)
    __sync_synchronize();
#endif
    /* If sequence changed during read or is odd, read was interrupted -> retry */
    return (lock->sequence != start_seq);
}

#endif /* INDUSTRIAL_HMI_SEQLOCK_H */
