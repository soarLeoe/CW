/*
 * inmp441.h — INMP441 I2S MEMS microphone driver for ESP32-S3 (ESP-IDF v5.x)
 *
 * Hardware connections:
 *   INMP441 SCK  ->  ESP32-S3 GPIO14
 *   INMP441 WS   ->  ESP32-S3 GPIO15
 *   INMP441 SD   ->  ESP32-S3 GPIO16   (INMP441 data OUT -> ESP32 data IN)
 *   INMP441 L/R  ->  GND               (left channel output)
 *   INMP441 VDD  ->  3.3V
 *   INMP441 GND  ->  GND
 *
 * Uses I2S_NUM_1 (independent of MAX98357A on I2S_NUM_0).
 * Data format: 24-bit, two's complement, MSB-first, Philips I2S, packed
 * left-aligned in a 32-bit slot (low 8 bits are 0).
 */
#ifndef INMP441_H_
#define INMP441_H_

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2s_types.h"

/* ---- Pin assignments ---- */
#define INMP441_I2S_PORT            I2S_NUM_1
#define INMP441_SCK_IO              14
#define INMP441_WS_IO               15
#define INMP441_SD_IO               16      /* ESP32 data IN (INMP441 SD OUT) */

/* ---- Defaults ---- */
#define INMP441_SAMPLE_RATE         16000
#define INMP441_BITS_PER_SLOT       32      /* physical slot width on the wire */
#define INMP441_DATA_BITS           24      /* INMP441 effective data width */

/* ---- Public API ---- */

/**
 * Initialise the I2S master RX channel for the INMP441.
 *
 * @param sample_rate  Desired sample rate in Hz (e.g. 16000, 48000).
 *                     Pass 0 to use the default INMP441_SAMPLE_RATE.
 * @return ESP_OK on success.
 */
esp_err_t inmp441_init(uint32_t sample_rate);

/**
 * Read raw audio samples from the I2S RX channel.
 *
 * Each sample is 32-bit (high 24 bits are valid INMP441 data; low 8 bits are 0).
 *
 * @param data         Pointer to destination buffer.
 * @param len          Number of BYTES to read (must be a multiple of 4).
 * @param bytes_read   Number of bytes actually read.
 * @return ESP_OK on success.
 */
esp_err_t inmp441_read(uint8_t *data, size_t len, size_t *bytes_read);

/**
 * Disable and free the I2S RX channel.
 */
esp_err_t inmp441_deinit(void);

/**
 * Suspend the I2S RX channel (disable DMA).
 * Call this to stop DMA activity if it interferes with other peripherals.
 */
esp_err_t inmp441_suspend(void);

/**
 * Resume the I2S RX channel (re-enable DMA).
 */
esp_err_t inmp441_resume(void);

/**
 * @return The sample rate currently in use (set at init time).
 */
uint32_t inmp441_get_sample_rate(void);

#endif /* INMP441_H_ */
