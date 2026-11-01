/** @file alsa_sound.h  @brief Optional alarm tone on Linux (libasound loaded at run time) */
#ifndef HMI_ALSA_SOUND_H
#define HMI_ALSA_SOUND_H
#include <stdbool.h>
bool alsa_sound_start(void);   /* false: no libasound - the panel runs without the tone */
void alsa_sound_set(bool on);  /* follow the horn */
void alsa_sound_enable(bool on);
void alsa_sound_stop(void);
#endif
