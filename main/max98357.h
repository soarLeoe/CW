/*
 * max98357.h — MAX98357A I2S amplifier driver for ESP32-S3 (ESP-IDF v5.x)
 *
 * Hardware connections:
 *   MAX98357A DIN  ->  ESP32-S3 GPIO21
 *   MAX98357A BCLK ->  ESP32-S3 GPIO39
 *   MAX98357A LRC  ->  ESP32-S3 GPIO2
 *   MAX98357A SD   ->  floating (selects I2S format)
 *   MAX98357A GAIN ->  GND
 */
#ifndef MAX98357_H_
#define MAX98357_H_

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ---- Pin assignments ---- */
#define MAX98357_I2S_PORT           I2S_NUM_0
#define MAX98357_BCLK_IO            39
#define MAX98357_WS_IO              2
#define MAX98357_DIN_IO             21      /* ESP32 → MAX98357 data out */

/* ---- Defaults ---- */
#define MAX98357_SAMPLE_RATE        16000
#define MAX98357_BITS_PER_SAMPLE    16

/* ---- Public API ---- */

/**
 * Initialise the I2S master TX channel for the MAX98357A.
 *
 * @param sample_rate  Desired sample rate in Hz (e.g. 16000, 44100).
 *                     Pass 0 to use the default MAX98357_SAMPLE_RATE.
 * @return ESP_OK on success.
 */
esp_err_t max98357_init(uint32_t sample_rate);

/**
 * Write raw audio samples to the I2S TX channel.
 *
 * @param data           Pointer to PCM data (16-bit signed, little-endian).
 * @param len            Number of bytes to write.
 * @param bytes_written  Number of bytes actually written.
 * @return ESP_OK on success.
 */
esp_err_t max98357_write(const uint8_t *data, size_t len, size_t *bytes_written);

/**
 * Disable and free the I2S channel.
 */
esp_err_t max98357_deinit(void);

/**
 * Suspend the I2S TX channel (disable DMA).
 * Call this after playback to stop DMA activity and avoid bus interference
 * with other peripherals (e.g. I2C sensors).
 */
esp_err_t max98357_suspend(void);

/**
 * Resume the I2S TX channel (re-enable DMA).
 * Call this before writing new audio data after a suspend.
 */
esp_err_t max98357_resume(void);

#endif /* MAX98357_H_ */
