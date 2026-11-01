/**
 * @file alsa_sound.c
 * @brief Alarm tone on Linux: ALSA "default" device (PulseAudio / PipeWire route it on desktops)
 *
 * libasound is loaded at run time: a panel or server without it runs normally, without the tone.
 * A small thread loops the two-tone pattern while the horn sounds; nothing blocks the UI.
 * The device can be chosen with HMI_SOUND_DEVICE (default "default").
 */

#include "alsa_sound.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hal/sys.h"

typedef struct snd_pcm snd_pcm_t;
static int  (*p_open)(snd_pcm_t **, const char *, int, int);
static int  (*p_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
static long (*p_writei)(snd_pcm_t *, const void *, unsigned long);
static int  (*p_recover)(snd_pcm_t *, int, int);
static int  (*p_drop)(snd_pcm_t *);
static int  (*p_close)(snd_pcm_t *);
typedef void (*snd_error_fn)(const char *file, int line, const char *func, int err, const char *fmt, ...);
static int  (*p_set_error_handler)(snd_error_fn);    /* optional */

/* ALSA prints its own lines for every failed open; after the first failure (shown once, with our
 * own summary) the retries every 2 s stay quiet so the log is not flooded */
static void quiet_alsa(const char *file, int line, const char *func, int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}

#define PCM_STREAM_PLAYBACK  0
#define PCM_FORMAT_S16_LE    2
#define PCM_ACCESS_RW_INTER  3
#define TONE_RATE            11025u

static void          *s_lib;
static pthread_t      s_thread;
static volatile int   s_want = 0, s_run = 0, s_enabled = 1;
static int16_t        s_tone[TONE_RATE];          /* 0.6 s pattern fits in 1 s */
static unsigned long  s_tone_len;

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *sound_thread(void *arg)
{
    (void)arg;
    snd_pcm_t *pcm = NULL;
    const char *dev = getenv("HMI_SOUND_DEVICE");
    if (!dev || !dev[0]) dev = "default";
    bool reported = false;
    while (s_run) {
        if (!s_want || !s_enabled) {
            if (pcm) { p_drop(pcm); p_close(pcm); pcm = NULL; }
            sleep_ms(50);
            continue;
        }
        if (!pcm) {
            if (p_open(&pcm, dev, PCM_STREAM_PLAYBACK, 0) < 0 ||
                p_set_params(pcm, PCM_FORMAT_S16_LE, PCM_ACCESS_RW_INTER, 1, TONE_RATE, 1, 200000) < 0) {
                if (pcm) { p_close(pcm); pcm = NULL; }
                if (!reported) {
                    fprintf(stderr, "industrial_hmi: no sound device \"%s\": alarm tone off (retried every 2 s)\n", dev);
                    reported = true;
                    if (p_set_error_handler) p_set_error_handler(quiet_alsa);
                }
                sleep_ms(2000);                    /* a headset may be plugged in later */
                continue;
            }
            if (reported) {
                fprintf(stderr, "industrial_hmi: sound device \"%s\" found: alarm tone on\n", dev);
                if (p_set_error_handler) p_set_error_handler(NULL);   /* ALSA's own messages again */
            }
            reported = false;
        }
        long w = p_writei(pcm, s_tone, s_tone_len);   /* blocks for one 0.6 s pattern */
        if (w < 0 && p_recover(pcm, (int)w, 1) < 0) { p_close(pcm); pcm = NULL; }
    }
    if (pcm) { p_drop(pcm); p_close(pcm); }
    return NULL;
}

bool alsa_sound_start(void)
{
    s_lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!s_lib) return false;
    *(void **)(&p_open)       = dlsym(s_lib, "snd_pcm_open");
    *(void **)(&p_set_params) = dlsym(s_lib, "snd_pcm_set_params");
    *(void **)(&p_writei)     = dlsym(s_lib, "snd_pcm_writei");
    *(void **)(&p_recover)    = dlsym(s_lib, "snd_pcm_recover");
    *(void **)(&p_drop)       = dlsym(s_lib, "snd_pcm_drop");
    *(void **)(&p_close)      = dlsym(s_lib, "snd_pcm_close");
    *(void **)(&p_set_error_handler) = dlsym(s_lib, "snd_lib_error_set_handler");
    if (!p_open || !p_set_params || !p_writei || !p_recover || !p_drop || !p_close) {
        dlclose(s_lib); s_lib = NULL; return false;
    }
    s_tone_len = hal_alarm_tone_pcm(s_tone, sizeof(s_tone) / sizeof(s_tone[0]), TONE_RATE);
    s_run = 1;
    if (pthread_create(&s_thread, NULL, sound_thread, NULL) != 0) { s_run = 0; dlclose(s_lib); s_lib = NULL; return false; }
    return true;
}

void alsa_sound_set(bool on)    { s_want = on; }
void alsa_sound_enable(bool on) { s_enabled = on; }

void alsa_sound_stop(void)
{
    if (!s_run) return;
    s_run = 0;
    pthread_join(s_thread, NULL);
    dlclose(s_lib);
    s_lib = NULL;
}
