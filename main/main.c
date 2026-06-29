/*
 * main.c — MAX30102 heart-rate detection + MAX98357A audio on ESP32-S3 (ESP-IDF v5.3)
 *
 * Hardware connections:
 *   MAX30102 SDA  ->  ESP32-S3 GPIO11
 *   MAX30102 SCL  ->  ESP32-S3 GPIO12
 *   MAX98357A DIN  -> ESP32-S3 GPIO21
 *   MAX98357A BCLK -> ESP32-S3 GPIO39
 *   MAX98357A LRC  -> ESP32-S3 GPIO2
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "max30102.h"
#include "algorithm.h"
#include "max98357.h"
#include "audio_tone.h"

static const char *TAG = "MAIN";

#define FINGER_THRESHOLD    50000
#define HR_MIN             30
#define HR_MAX             200

/* Sample buffers — 500 samples = 5 seconds @ 100 sps */
static uint32_t aun_ir_buffer[BUFFER_SIZE];
static uint32_t aun_red_buffer[BUFFER_SIZE];

/* -----------------------------------------------------------------------
 * Read one sample from FIFO. Blocks up to ~1s waiting for data.
 * --------------------------------------------------------------------- */
static bool read_one_sample(uint32_t *red, uint32_t *ir)
{
    int wait_count = 0;
    uint8_t wr_ptr, rd_ptr;

    while (1) {
        if (!maxim_max30102_read_reg(REG_FIFO_WR_PTR, &wr_ptr)) return false;
        if (!maxim_max30102_read_reg(REG_FIFO_RD_PTR, &rd_ptr)) return false;

        if ((wr_ptr & 0x1F) != (rd_ptr & 0x1F)) {
            return maxim_max30102_read_fifo(red, ir);
        }

        if (++wait_count > 1000) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* -----------------------------------------------------------------------
 * Collect n samples at ~100 sps (10ms interval).
 * --------------------------------------------------------------------- */
static void collect_samples(uint32_t *red_buf, uint32_t *ir_buf, int n)
{
    for (int i = 0; i < n; i++) {
        if (!read_one_sample(&red_buf[i], &ir_buf[i])) {
            ESP_LOGW(TAG, "collect_samples: timeout at sample %d", i);
            i--;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* -----------------------------------------------------------------------
 * Check finger presence.
 * --------------------------------------------------------------------- */
static bool finger_present(uint32_t *ir_buf, int count)
{
    uint32_t sum = 0;
    for (int i = 0; i < count; i++)
        sum += ir_buf[i];
    return (sum / count) > FINGER_THRESHOLD;
}

/* -----------------------------------------------------------------------
 * Wait for finger: poll at 10ms.
 * --------------------------------------------------------------------- */
static void wait_for_finger(void)
{
    uint32_t red, ir;
    int no_data_count = 0;
    while (1) {
        if (read_one_sample(&red, &ir)) {
            no_data_count = 0;
            if (ir > FINGER_THRESHOLD) {
                return;
            }
        } else {
            /* read_one_sample timed out — FIFO empty, sensor may have
             * stopped due to I2S interference. Restart it. */
            if (++no_data_count >= 3) {
                ESP_LOGW(TAG, "wait_for_finger: no FIFO data, restarting sensor...");
                maxim_max30102_restart_measurement();
                no_data_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* -----------------------------------------------------------------------
 * Wait for signal to stabilize after finger placement.
 *
 * When a finger is first placed, the MAX30102's ambient light
 * cancellation circuit ramps up — the raw readings increase
 * monotonically for 1-2 seconds.  During this ramp there is no
 * pulsatile waveform, so the HR algorithm cannot find peaks.
 *
 * This function drains the FIFO for up to 3 seconds and checks that
 * consecutive samples are oscillating (AC present) rather than
 * monotonically increasing (DC ramp).
 *
 * Returns true if signal stabilized, false on timeout.
 * --------------------------------------------------------------------- */
static bool wait_for_signal_stable(void)
{
    uint32_t red, ir;
    uint32_t prev_ir = 0;
    int stable_count = 0;
    int monotonic_count = 0;
    int fail_count = 0;

    ESP_LOGI(TAG, "Waiting for signal to stabilize...");

    /* Drain FIFO and monitor for ~3 seconds */
    for (int tick = 0; tick < 300; tick++) {
        if (!read_one_sample(&red, &ir)) {
            fail_count++;
            if (fail_count % 5 == 0) {
                ESP_LOGW(TAG, "wait_for_signal_stable: %d read failures so far",
                         fail_count);
                /* Diagnose: dump sensor registers to see if mode got corrupted */
                maxim_max30102_dump_regs();
            }
            /* If I2C is persistently failing, something is seriously wrong.
             * After 30 failures (~300ms of failures) bail out rather than
             * spinning forever in a blocked I2C call. */
            if (fail_count > 15) {
                ESP_LOGE(TAG, "Too many read failures (%d), aborting stabilize",
                         fail_count);
                /* Try one more restart before giving up */
                ESP_LOGI(TAG, "Attempting emergency restart...");
                maxim_max30102_restart_measurement();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fail_count = 0;   /* reset on success */

        if (tick > 0) {
            int32_t diff = (int32_t)ir - (int32_t)prev_ir;

            /* If signal is still ramping (large monotonic change) */
            if (diff > 500 || diff < -500) {
                monotonic_count++;
                stable_count = 0;
            } else {
                /* Signal is oscillating — AC component present */
                stable_count++;
                monotonic_count = 0;
            }

            /* Need 20 consecutive stable samples (~200ms) to confirm */
            if (stable_count >= 20) {
                ESP_LOGI(TAG, "Signal stable after %d ms (IR=%lu)",
                         tick * 10, (unsigned long)ir);
                return true;
            }
        }

        prev_ir = ir;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Signal did not stabilize in 3s, trying anyway");
    return false;
}

/* -----------------------------------------------------------------------
 * Dump first 10 IR/RED samples for debugging.
 * --------------------------------------------------------------------- */
static void dump_samples(uint32_t *red, uint32_t *ir, int n)
{
    for (int i = 0; i < n && i < 10; i++) {
        ESP_LOGI(TAG, "  [%d] red=%lu ir=%lu",
                 i, (unsigned long)red[i], (unsigned long)ir[i]);
    }
}

void app_main(void)
{
    int32_t n_heart_rate;
    int8_t  ch_hr_valid;
    int32_t n_sp02;
    int8_t  ch_spo2_valid;

    ESP_LOGI(TAG, "=== ESP32-S3 Health Monitor (MAX30102 + MAX98357A) ===");

    /* --- Initialise MAX98357A audio amplifier --- */
    esp_err_t audio_ret = max98357_init(MAX98357_SAMPLE_RATE);
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "MAX98357A audio amplifier ready.");
        audio_tone_play_melody();   /* boot melody */
    } else {
        ESP_LOGE(TAG, "MAX98357A init failed: %s (audio disabled)",
                 esp_err_to_name(audio_ret));
    }

    /* --- Initialise MAX30102 heart-rate sensor --- */
    if (!maxim_max30102_init()) {
        ESP_LOGE(TAG, "MAX30102 init failed! Check wiring.");
        audio_tone_play_beep(200, 500);   /* low beep = error */
        while (1) vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* --- Wait for finger --- */
    ESP_LOGI(TAG, "Place finger on sensor...");
    audio_tone_play_beep(880, 100);   /* prompt tone */
    wait_for_finger();
    ESP_LOGI(TAG, "Finger detected.");
    audio_tone_play_beep(1046, 100);  /* confirm tone */

    /* I2S DMA activity from the beep above can corrupt MAX30102's
     * registers (especially MODE_CONFIG), putting it into standby and
     * stopping FIFO sampling. Restart measurement to be safe. */
    if (!maxim_max30102_restart_measurement()) {
        ESP_LOGE(TAG, "Failed to restart MAX30102 measurement!");
    }

    /* --- Wait for signal to stabilize --- */
    wait_for_signal_stable();

    /* --- Phase 1: collect first 500 samples --- */
    ESP_LOGI(TAG, "Collecting 500 samples (~5s)...");
    collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE);
    ESP_LOGI(TAG, "First 500 samples done.");
    dump_samples(aun_red_buffer, aun_ir_buffer, 10);

    /* Compute initial HR */
    maxim_heart_rate_and_oxygen_saturation(
        aun_ir_buffer, BUFFER_SIZE, aun_red_buffer,
        &n_sp02, &ch_spo2_valid,
        &n_heart_rate, &ch_hr_valid);
    ESP_LOGI(TAG, "Initial: HR=%ld (valid=%d) SpO2=%ld (valid=%d)",
             (long)n_heart_rate, (int)ch_hr_valid,
             (long)n_sp02, (int)ch_spo2_valid);

    /* --- Phase 2: continuous heart rate --- */
    while (1) {
        /* Shift buffer: discard oldest 100, keep 400 */
        for (int i = 100; i < BUFFER_SIZE; i++) {
            aun_red_buffer[i - 100] = aun_red_buffer[i];
            aun_ir_buffer[i - 100]  = aun_ir_buffer[i];
        }

        /* Collect 100 new samples */
        collect_samples(&aun_red_buffer[400], &aun_ir_buffer[400], 100);

        /* Check finger */
        if (!finger_present(&aun_ir_buffer[400], 100)) {
            ESP_LOGW(TAG, "No finger detected. Waiting...");
            audio_tone_play_beep(880, 100);   /* prompt tone */
            wait_for_finger();
            ESP_LOGI(TAG, "Finger detected again.");
            audio_tone_play_beep(1046, 100);  /* confirm tone */
            /* Restart measurement — beep may have corrupted sensor regs */
            maxim_max30102_restart_measurement();
            wait_for_signal_stable();
            ESP_LOGI(TAG, "Recollecting 500 samples...");
            collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE);
            continue;
        }

        /* Compute HR and SpO2 */
        maxim_heart_rate_and_oxygen_saturation(
            aun_ir_buffer, BUFFER_SIZE, aun_red_buffer,
            &n_sp02, &ch_spo2_valid,
            &n_heart_rate, &ch_hr_valid);

        /* Print with sanity checks */
        bool hr_ok = (ch_hr_valid == 1) && (n_heart_rate >= HR_MIN)
                     && (n_heart_rate <= HR_MAX);
        bool spo2_ok = (ch_spo2_valid == 1) && (n_sp02 > 0) && (n_sp02 <= 100);

        if (hr_ok) {
            if (spo2_ok) {
                ESP_LOGI(TAG, "HR = %ld BPM  |  SpO2 = %ld%%",
                         (long)n_heart_rate, (long)n_sp02);
            } else {
                ESP_LOGI(TAG, "HR = %ld BPM  |  SpO2 = --",
                         (long)n_heart_rate);
            }
        } else {
            ESP_LOGW(TAG, "HR = --  (raw HR=%ld, valid=%d)",
                     (long)n_heart_rate, (int)ch_hr_valid);
        }
    }
}
