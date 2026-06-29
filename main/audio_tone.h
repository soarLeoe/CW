/*
 * audio_tone.h — Tone/melody generation for MAX98357A
 *
 * Provides simple beep and melody playback using a pre-computed
 * sine wave lookup table.
 */
#ifndef AUDIO_TONE_H_
#define AUDIO_TONE_H_

#include <stdint.h>

/**
 * Play a single beep tone.
 *
 * @param freq        Frequency in Hz (e.g. 880).
 * @param duration_ms Duration in milliseconds.
 */
void audio_tone_play_beep(uint32_t freq, uint32_t duration_ms);

/**
 * Play a short startup melody (3 ascending notes).
 */
void audio_tone_play_melody(void);

#endif /* AUDIO_TONE_H_ */
