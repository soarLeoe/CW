/*
 * audio_tone.c — Tone/melody generation for MAX98357A
 *
 * Generates 16-bit sine wave samples at runtime and writes them
 * to the MAX98357A via the I2S driver.
 */

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max98357.h"
#include "audio_tone.h"

static const char *TAG = "AUDIO";

/* Sine table: one full cycle, 256 entries, 16-bit signed */
#define SINE_TABLE_SIZE 256
static int16_t s_sine_table[SINE_TABLE_SIZE];

static bool s_table_ready = false;

/* DMA write buffer (in samples) */
#define TONE_BUF_SAMPLES  512
static int16_t s_buf[TONE_BUF_SAMPLES];

/*
 * DMA buffer in max98357.c is 6 desc × 240 frames = 1440 samples.
 * When playback stops, the DMA keeps looping whatever is left in its
 * ring buffer, causing a repeating "beep-beep-beep". To fully flush
 * residual audio we must write at least one full DMA buffer's worth
 * of silence (2× for safety).
 */
#define DMA_BUF_SAMPLES    1440
#define SILENCE_FLUSH_MUL  2

/* ----------------------------------------------------------------------- */
/*  Internal helpers                                                        */
/* ----------------------------------------------------------------------- */

static void build_sine_table(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        s_sine_table[i] = (int16_t)(sinf(2.0f * M_PI * i / SINE_TABLE_SIZE) * 12000.0f);
    }
    s_table_ready = true;
}

/* Flush the I2S DMA ring buffer with silence so no residual audio
 * keeps looping after a beep finishes. */
static void flush_silence(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    uint32_t total = DMA_BUF_SAMPLES * SILENCE_FLUSH_MUL;
    size_t bytes_written = 0;
    while (total > 0) {
        uint32_t chunk = (total > TONE_BUF_SAMPLES) ? TONE_BUF_SAMPLES : total;
        max98357_write((const uint8_t *)s_buf, chunk * sizeof(int16_t),
                       &bytes_written);
        total -= chunk;
    }
}

/* ----------------------------------------------------------------------- */
/*  Public API                                                              */
/* ----------------------------------------------------------------------- */

void audio_tone_play_beep(uint32_t freq, uint32_t duration_ms)
{
    if (!s_table_ready) {
        build_sine_table();
    }

    /* Phase accumulator: fixed-point 24.8 */
    uint32_t total_samples = (uint32_t)((uint64_t)MAX98357_SAMPLE_RATE * duration_ms / 1000);
    uint32_t phase_step = (uint32_t)((uint64_t)freq * SINE_TABLE_SIZE * 256 / MAX98357_SAMPLE_RATE);
    uint32_t phase = 0;       /* 24.8 fixed point */

    ESP_LOGI(TAG, "Beep: %lu Hz, %lu ms (%lu samples)",
             (unsigned long)freq, (unsigned long)duration_ms,
             (unsigned long)total_samples);

    uint32_t written = 0;
    while (written < total_samples) {
        uint32_t chunk = total_samples - written;
        if (chunk > TONE_BUF_SAMPLES) {
            chunk = TONE_BUF_SAMPLES;
        }

        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t idx = (phase >> 8) & (SINE_TABLE_SIZE - 1);
            s_buf[i] = s_sine_table[idx];
            phase += phase_step;
        }

        size_t bytes_to_write = chunk * sizeof(int16_t);
        size_t bytes_written = 0;
        max98357_write((const uint8_t *)s_buf, bytes_to_write, &bytes_written);
        written += chunk;
    }

    /* Flush DMA ring buffer with silence to prevent residual audio
     * from looping ("beep-beep-beep" after playback ends). */
    flush_silence();

    /* Suspend the I2S channel to stop DMA activity.
     * This is CRITICAL: an enabled I2S DMA channel continuously
     * generates BCLK/LRC toggles and GDMA transfers, which interfere
     * with I2C peripherals (e.g. MAX30102). Suspending after playback
     * leaves the bus quiet for sensor communication. The next
     * max98357_write() call auto-resumes. */
    max98357_suspend();
}

void audio_tone_play_melody(void)
{
    ESP_LOGI(TAG, "Playing startup melody...");
    /* C5(523) → E5(659) → G5(784) */
    audio_tone_play_beep(523, 150);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_tone_play_beep(659, 150);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_tone_play_beep(784, 250);
    ESP_LOGI(TAG, "Melody done.");
}
