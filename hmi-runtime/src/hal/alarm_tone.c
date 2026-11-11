/**
 * @file alarm_tone.c
 * @brief The HMI's own alarm sound: a two-tone (high / low) signal, generated without libm or heap
 *
 * Plants do not rely on a desktop's notification sound for an alarm: it may be muted, set to a
 * soft "ding" or switched off by the sound scheme. The shells play this tone in a loop while
 * the horn sounds (unacknowledged alarms, not silenced).
 */

#include "hal/sys.h"

static const int16_t s_sine[64] = {
         0,   1372,   2731,   4064,   5358,   6600,   7778,   8882,
      9899,  10822,  11641,  12347,  12934,  13397,  13731,  13933,
     14000,  13933,  13731,  13397,  12934,  12347,  11641,  10822,
      9899,   8882,   7778,   6600,   5358,   4064,   2731,   1372,
         0,  -1372,  -2731,  -4064,  -5358,  -6600,  -7778,  -8882,
     -9899, -10822, -11641, -12347, -12934, -13397, -13731, -13933,
    -14000, -13933, -13731, -13397, -12934, -12347, -11641, -10822,
     -9899,  -8882,  -7778,  -6600,  -5358,  -4064,  -2731,  -1372,
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

size_t hal_alarm_tone_pcm(int16_t *out, size_t max_samples, uint32_t rate)
{
    /* 0.30 s at 960 Hz, then 0.30 s at 760 Hz: an urgent, easily located two-tone pattern */
    static const uint32_t freq[2] = { 960u, 760u };
    size_t per = (size_t)(rate * 3u / 10u), n = 0;
    for (int part = 0; part < 2 && out; part++) {
        uint32_t phase = 0, step = (uint32_t)(((uint64_t)freq[part] << 22) / rate);   /* 64 steps << 16 */
        for (size_t i = 0; i < per && n < max_samples; i++, n++) {
            int32_t v = s_sine[(phase >> 16) & 63u];
            size_t edge = i < per - i ? i : per - i;               /* 4 ms ramps: no clicks */
            size_t ramp = rate / 250u;
            if (edge < ramp) v = (int32_t)((v * (int32_t)edge) / (int32_t)ramp);
            out[n] = (int16_t)v;
            phase += step;
        }
    }
    return n;
}

size_t hal_emergency_tone_pcm(int16_t *out, size_t max_samples, uint32_t rate)
{
    /* EMERGENCY: a fast rising "yelp", 650 -> 1500 Hz in 0.15 s, four times: never mistaken for
     * the two-tone alarm. The phase runs on across sweeps, so the jump back down does not click. */
    size_t per = (size_t)(rate * 15u / 100u), n = 0;
    uint32_t phase = 0;
    for (int sweep = 0; sweep < 4 && out; sweep++) {
        for (size_t i = 0; i < per && n < max_samples; i++, n++) {
            uint64_t f = 650u + (uint64_t)850u * i / per;
            phase += (uint32_t)((f << 22) / rate);
            out[n] = (int16_t)((int32_t)s_sine[(phase >> 16) & 63u] * 9 / 5);   /* about 77 % of full scale: never quieter than an alarm */
        }
    }
    size_t ramp = rate / 250u;                                     /* 4 ms in and out: no click when it loops */
    for (size_t i = 0; i < ramp && i < n / 2; i++) {
        out[i] = (int16_t)((out[i] * (int32_t)i) / (int32_t)ramp);
        out[n - 1 - i] = (int16_t)((out[n - 1 - i] * (int32_t)i) / (int32_t)ramp);
    }
    return n;
}

static size_t wav_image(uint8_t *out, size_t cap, uint32_t rate, bool emergency)
{
    if (!out || cap < 44) return 0;
    int16_t *pcm = (int16_t *)(void *)(out + 44);
    size_t samples = emergency ? hal_emergency_tone_pcm(pcm, (cap - 44) / 2, rate)
                               : hal_alarm_tone_pcm(pcm, (cap - 44) / 2, rate);
    uint32_t data = (uint32_t)(samples * 2u);
    out[0] = 'R'; out[1] = 'I'; out[2] = 'F'; out[3] = 'F'; put32(out + 4, 36u + data);
    out[8] = 'W'; out[9] = 'A'; out[10] = 'V'; out[11] = 'E';
    out[12] = 'f'; out[13] = 'm'; out[14] = 't'; out[15] = ' '; put32(out + 16, 16u);
    put16(out + 20, 1u); put16(out + 22, 1u); put32(out + 24, rate); put32(out + 28, rate * 2u);
    put16(out + 32, 2u); put16(out + 34, 16u);
    out[36] = 'd'; out[37] = 'a'; out[38] = 't'; out[39] = 'a'; put32(out + 40, data);
    return 44u + data;
}

size_t hal_alarm_tone_wav(uint8_t *out, size_t cap, uint32_t rate)     { return wav_image(out, cap, rate, false); }
size_t hal_emergency_tone_wav(uint8_t *out, size_t cap, uint32_t rate) { return wav_image(out, cap, rate, true); }
