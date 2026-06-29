/*
 * mic_recorder.c — Application layer for the INMP441 microphone.
 *
 * Owns a FreeRTOS task that:
 *   1. Reads 1024-sample blocks from the I2S RX channel.
 *   2. Computes RMS / dBFS over the block.
 *   3. Pushes samples into a single-producer / multi-consumer ring buffer.
 *
 * The level getters and the PCM reader run on the consumer side and are
 * designed to be cheap (a couple of atomic reads + a short copy under a
 * spinlock) so the HR-detection main loop can poll them without blocking.
 */

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "inmp441.h"
#include "mic_recorder.h"

static const char *TAG = "MIC";

/* ---- Tunables ---- */
#define BLOCK_SAMPLES           1024                /* samples per read */
#define RECORDER_STACK_BYTES    (6 * 1024)          /* room for locals + logging */
#define RECORDER_TASK_PRIO      4
#define RECORDER_TASK_CORE      1                   /* pin to core 1, leave core 0 for main/HR */

/* Full-scale 24-bit reference (2^23 - 1) for dBFS conversion. */
#define FS_24BIT                8388607.0f

/* ---- State ---- */
static StaticTask_t s_task_tcb;
static StackType_t  s_task_stack[RECORDER_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_task_handle = NULL;

static volatile float    s_last_dbfs   = -90.0f;
static volatile uint8_t  s_last_vu     = 0;
static volatile bool     s_suspended   = false;

/* Lock-free-ish ring buffer (single producer = recorder task, multiple
 * consumers). The producer updates head after writing; consumers read head
 * then copy under a brief critical section to keep tail consistent. */
static int32_t  s_ring[MIC_RECORDER_RING_SAMPLES];
static volatile int s_head = 0;     /* producer writes here */
static volatile int s_tail = 0;     /* consumer reads here */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

/* 2^23 - 1 reference, captured once. Avoids recomputing log() in hot path. */
static float s_log10_fs = 0.0f;

/* ----------------------------------------------------------------------- */
/*  Helpers                                                                 */
/* ----------------------------------------------------------------------- */

/* Map dBFS in [-60, -10] to VU in [0, 10]. Out-of-range values clamp. */
static uint8_t dbfs_to_vu(float dbfs)
{
    if (dbfs <= -60.0f) return 0;
    if (dbfs >= -10.0f) return 10;
    return (uint8_t)((dbfs + 60.0f) / 5.0f + 0.5f);
}

/* Push a block of samples into the ring buffer. If full, drop the oldest.
 *
 * The ring is single-producer (this function, called only from the recorder
 * task) / multi-consumer (read_pcm). On the ESP32-S3 SMP port we must guard
 * s_tail against concurrent writes from read_pcm, so the whole push runs
 * under a brief critical section. Hold time is ~1024 simple writes
 * (a few hundred microseconds at most) — acceptable given the 64 ms block
 * cadence and the low consumer read frequency. */
static void ring_push(const int32_t *samples, int n)
{
    portENTER_CRITICAL(&s_ring_mux);
    for (int i = 0; i < n; i++) {
        int next = (s_head + 1) % MIC_RECORDER_RING_SAMPLES;
        if (next == s_tail) {
            /* Buffer full: advance tail to make room (drop oldest). */
            s_tail = (s_tail + 1) % MIC_RECORDER_RING_SAMPLES;
        }
        s_ring[s_head] = samples[i];
        s_head = next;
    }
    portEXIT_CRITICAL(&s_ring_mux);
}

/* ----------------------------------------------------------------------- */
/*  Recorder task                                                           */
/* ----------------------------------------------------------------------- */

static void mic_recorder_task(void *arg)
{
    ESP_LOGI(TAG, "recorder task started on core %d", RECORDER_TASK_CORE);

    /* Block buffer lives in BSS (static) instead of on the task stack —
     * 1024 × 4 = 4 KB is too large to safely fit in a 6 KB stack alongside
     * FreeRTOS overhead, logging, and the RMS loop locals. */
    static int32_t block[BLOCK_SAMPLES];
    /* Read in bytes: 1024 samples × 4 bytes = 4096 bytes per block. */
    const size_t block_bytes = BLOCK_SAMPLES * sizeof(int32_t);

    while (1) {
        if (s_suspended) {
            /* Pause: stop reading, sleep briefly, wait for resume. */
            inmp441_suspend();
            while (s_suspended) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            inmp441_resume();
            ESP_LOGI(TAG, "recorder resumed");
        }

        size_t bytes_read = 0;
        esp_err_t ret = inmp441_read((uint8_t *)block, block_bytes, &bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "inmp441_read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int samples_read = (int)(bytes_read / sizeof(int32_t));
        if (samples_read == 0) continue;

        /* ---- RMS over the block (use 24-bit effective value) ----
         * INMP441 data lives in the upper 24 bits; arithmetic shift right by
         * 8 gives the signed 24-bit value without changing the ratio between
         * samples, so RMS scaling is consistent. We compute on the shifted
         * value to keep the int64 accumulator from overflowing unnecessarily
         * and to keep dBFS math against the 24-bit full-scale reference. */
        int64_t sum_sq = 0;
        for (int i = 0; i < samples_read; i++) {
            int32_t v24 = block[i] >> 8;     /* arithmetic shift keeps sign */
            sum_sq += (int64_t)v24 * v24;
        }
        double mean_sq = (double)sum_sq / samples_read;
        double rms = (mean_sq > 0.0) ? sqrt(mean_sq) : 0.0;

        /* dBFS = 20 * log10(rms / FS_24BIT) */
        float dbfs = -90.0f;
        if (rms > 0.5) {
            dbfs = 20.0f * (float)(log10(rms) - s_log10_fs);
            if (dbfs < -90.0f) dbfs = -90.0f;
            if (dbfs > 0.0f)   dbfs = 0.0f;
        }
        s_last_dbfs = dbfs;
        s_last_vu   = dbfs_to_vu(dbfs);

        /* Push into ring buffer for any PCM consumer. */
        ring_push(block, samples_read);

        /* Light periodic log: ~every 5s (every ~78 blocks @ 16 kHz). */
        static int log_divider = 0;
        if (++log_divider >= 78) {
            log_divider = 0;
            ESP_LOGI(TAG, "level: %.1f dBFS  VU=%d", dbfs, s_last_vu);
        }
    }

    /* Unreachable — task runs forever. */
    vTaskDelete(NULL);
}

/* ----------------------------------------------------------------------- */
/*  Public API                                                              */
/* ----------------------------------------------------------------------- */

esp_err_t mic_recorder_init(uint32_t sample_rate)
{
    /* Cache log10(FS_24BIT) so the hot path uses a single log10(rms). */
    s_log10_fs = log10f(FS_24BIT);

    esp_err_t ret = inmp441_init(sample_rate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INMP441 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Use a statically-allocated task so we don't depend on heap at boot. */
    s_task_handle = xTaskCreateStaticPinnedToCore(
        mic_recorder_task,
        "mic_rec",
        RECORDER_STACK_BYTES / sizeof(StackType_t),
        NULL,
        RECORDER_TASK_PRIO,
        s_task_stack,
        &s_task_tcb,
        RECORDER_TASK_CORE);

    if (s_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create recorder task");
        inmp441_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "mic_recorder task created (core=%d, prio=%d, stack=%d B)",
             RECORDER_TASK_CORE, RECORDER_TASK_PRIO, RECORDER_STACK_BYTES);
    return ESP_OK;
}

float mic_recorder_get_level_db(void)
{
    return s_last_dbfs;
}

uint8_t mic_recorder_get_vu(void)
{
    return s_last_vu;
}

int mic_recorder_read_pcm(int32_t *buf, int max_samples)
{
    portENTER_CRITICAL(&s_ring_mux);
    int head = s_head;
    int tail = s_tail;
    int available = (head - tail + MIC_RECORDER_RING_SAMPLES) % MIC_RECORDER_RING_SAMPLES;
    int to_copy = (available < max_samples) ? available : max_samples;
    for (int i = 0; i < to_copy; i++) {
        buf[i] = s_ring[tail];
        tail = (tail + 1) % MIC_RECORDER_RING_SAMPLES;
    }
    s_tail = tail;
    portEXIT_CRITICAL(&s_ring_mux);
    return to_copy;
}

void mic_recorder_suspend(void)
{
    ESP_LOGI(TAG, "suspend requested");
    s_suspended = true;
}

void mic_recorder_resume(void)
{
    ESP_LOGI(TAG, "resume requested");
    s_suspended = false;
}
