/*
 * mic_recorder.h — Application layer on top of the INMP441 driver.
 *
 * Spawns a FreeRTOS task that continuously pulls audio blocks from the I2S RX
 * channel, computes an RMS / dBFS level for each block, and feeds a lock-free
 * ring buffer so consumers can grab raw PCM samples at their own pace.
 *
 * The level API (get_level_db / get_vu) is the cheap read path used by the
 * HR-detection main loop or future cloud upload code. The PCM read API is for
 * future features like streaming audio to MQTT / WebSocket.
 */
#ifndef MIC_RECORDER_H_
#define MIC_RECORDER_H_

#include <stdint.h>
#include "esp_err.h"

/* Ring buffer capacity in 32-bit samples. 4096 samples @ 16 kHz ≈ 256 ms of
 * audio — enough headroom for a slow consumer without burning too much RAM. */
#define MIC_RECORDER_RING_SAMPLES   4096

/**
 * Initialise the INMP441 driver and spawn the recorder task.
 *
 * @param sample_rate  Sample rate in Hz (0 -> INMP441_SAMPLE_RATE default).
 * @return ESP_OK on success.
 */
esp_err_t mic_recorder_init(uint32_t sample_rate);

/**
 * @return The most recently computed RMS level in dBFS, in the range
 *         roughly [-90, 0]. Updated once per audio block (~64 ms @ 16 kHz).
 */
float mic_recorder_get_level_db(void);

/**
 * @return A 0..10 VU meter level derived from the latest dBFS reading.
 *         (-60 dBFS -> 0, -10 dBFS -> 10, clamped linear in between.)
 */
uint8_t mic_recorder_get_vu(void);

/**
 * Pull PCM samples out of the ring buffer.
 *
 * @param buf           Destination buffer of int32_t samples. Each sample's
 *                     upper 24 bits are the INMP441 data; low 8 bits are 0.
 * @param max_samples   Capacity of buf in samples (not bytes).
 * @return              Number of samples actually copied (0 if buffer empty).
 */
int mic_recorder_read_pcm(int32_t *buf, int max_samples);

/**
 * Ask the recorder task to pause audio capture on its next loop iteration.
 * The I2S RX channel is suspended (DMA stopped). Non-blocking.
 */
void mic_recorder_suspend(void);

/**
 * Ask the recorder task to resume audio capture. Non-blocking.
 */
void mic_recorder_resume(void);

#endif /* MIC_RECORDER_H_ */
