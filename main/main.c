/*
 * main.c — MAX30102 heart-rate detection + MAX98357A audio + INMP441 mic
 *          on ESP32-S3 (ESP-IDF v5.3)
 *
 * Hardware connections:
 *   MAX30102 SDA  ->  ESP32-S3 GPIO11
 *   MAX30102 SCL  ->  ESP32-S3 GPIO12
 *   MAX98357A DIN  -> ESP32-S3 GPIO21
 *   MAX98357A BCLK -> ESP32-S3 GPIO39
 *   MAX98357A LRC  -> ESP32-S3 GPIO2
 *   INMP441 SCK  ->   ESP32-S3 GPIO14
 *   INMP441 WS   ->   ESP32-S3 GPIO15
 *   INMP441 SD   ->   ESP32-S3 GPIO16   (INMP441 OUT -> ESP32 IN)
 *   INMP441 L/R  ->   GND               (left channel)
 *
 * Network:
 *   WiFi → MQTT → OneNET (China Mobile IoT platform)
 *   HR + SpO2 data published every measurement cycle (~1s)
 *
 * Audio:
 *   MAX98357A plays boot/beep tones on I2S_NUM_0 (TX).
 *   INMP441 captures ambient audio on I2S_NUM_1 (RX) in an independent
 *   task; current dBFS / VU level is logged every ~5s.
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
#include "wifi_sta.h"
#include "onenet_mqtt.h"
#include "inmp441.h"
#include "mic_recorder.h"

static const char *TAG = "MAIN";

/* --- WiFi credentials (modify for your network) --- */
#define WIFI_SSID       "xLeno"
#define WIFI_PASSWORD   "12345678"
#define WIFI_TIMEOUT_MS 15000

#define FINGER_THRESHOLD    50000
#define HR_MIN             30
#define HR_MAX             200

/* Sample buffers — 500 samples = 5 seconds @ 100 sps */
static uint32_t aun_ir_buffer[BUFFER_SIZE];
static uint32_t aun_red_buffer[BUFFER_SIZE];

/* -----------------------------------------------------------------------
 * Mic level uploader task — independent of the HR main loop.
 *
 * Why this exists: the HR main loop blocks for long stretches
 * (wait_for_finger can stall for minutes; collect_samples recovery takes
 * 30+ seconds). If dBFS publishing lived in the main loop, the mic level
 * would stop being reported every time the finger left the sensor or the
 * MAX30102 needed a restart. By running the uploader as its own task on
 * core 1 (alongside mic_recorder), HR state never affects noise reporting.
 * --------------------------------------------------------------------- */
#define MIC_UPLOADER_STACK_BYTES   (3 * 1024)
#define MIC_UPLOADER_PRIO          3
#define MIC_UPLOADER_INTERVAL_MS   1000

static StaticTask_t s_mic_uploader_tcb;
static StackType_t  s_mic_uploader_stack[MIC_UPLOADER_STACK_BYTES / sizeof(StackType_t)];

static void mic_uploader_task(void *arg)
{
    ESP_LOGI(TAG, "mic_uploader task started on core 1");
    while (1) {
        if (onenet_mqtt_is_connected()) {
            onenet_mqtt_publish_noise_level(mic_recorder_get_level_db());
        }
        vTaskDelay(pdMS_TO_TICKS(MIC_UPLOADER_INTERVAL_MS));
    }
}

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
 *
 * Has a watchdog: if read_one_sample fails N consecutive times (sensor
 * stopped producing samples — usually because I2S/MQTT bus activity
 * corrupted MAX30102's MODE_CONFIG), call restart_measurement() to
 * bring the sensor back.  Without this, the original `i--` retry loop
 * would spin forever on sample 0.
 * --------------------------------------------------------------------- */
#define COLLECT_MAX_FAIL  5

static bool collect_samples(uint32_t *red_buf, uint32_t *ir_buf, int n)
{
    int consec_fail = 0;

    for (int i = 0; i < n; i++) {
        if (!read_one_sample(&red_buf[i], &ir_buf[i])) {
            ESP_LOGW(TAG, "collect_samples: timeout at sample %d (fail %d/%d)",
                     i, consec_fail + 1, COLLECT_MAX_FAIL);
            if (++consec_fail >= COLLECT_MAX_FAIL) {
                ESP_LOGE(TAG, "Sensor not producing data, restarting measurement...");
                maxim_max30102_dump_regs();
                if (!maxim_max30102_restart_measurement()) {
                    ESP_LOGE(TAG, "restart_measurement failed, aborting collect");
                    return false;
                }
                consec_fail = 0;
                /* Don't advance i — retry this sample slot */
            }
            i--;   /* retry same slot */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        consec_fail = 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
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

    ESP_LOGI(TAG, "=== ESP32-S3 Health Monitor (MAX30102 + MAX98357A + OneNET) ===");

    /* --- Connect WiFi --- */
    ESP_LOGI(TAG, "Connecting to WiFi \"%s\" ...", WIFI_SSID);
    esp_err_t wifi_ret = wifi_sta_connect(WIFI_SSID, WIFI_PASSWORD,
                                          WIFI_TIMEOUT_MS);
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed, cloud upload disabled.");
    }

    /* --- Initialize OneNET MQTT --- */
    esp_err_t mqtt_ret = onenet_mqtt_init();
    if (mqtt_ret != ESP_OK) {
        ESP_LOGE(TAG, "OneNET MQTT init failed.");
    }

    /* --- Initialise MAX98357A audio amplifier --- */
    esp_err_t audio_ret = max98357_init(MAX98357_SAMPLE_RATE);
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "MAX98357A audio amplifier ready.");
        audio_tone_play_melody();   /* boot melody */
    } else {
        ESP_LOGE(TAG, "MAX98357A init failed: %s (audio disabled)",
                 esp_err_to_name(audio_ret));
    }

    /* --- Initialise INMP441 microphone (I2S_NUM_1 RX, independent task) --- */
    /* Runs on its own FreeRTOS task pinned to core 1, so it does not block
     * the HR-detection main loop on core 0. MAX98357A uses I2S_NUM_0; the
     * two I2S controllers are independent, so capture and playback can run
     * concurrently. */
    esp_err_t mic_ret = mic_recorder_init(INMP441_SAMPLE_RATE);
    if (mic_ret == ESP_OK) {
        ESP_LOGI(TAG, "INMP441 microphone ready (capture task running).");
    } else {
        ESP_LOGE(TAG, "INMP441 init failed: %s (mic disabled)",
                 esp_err_to_name(mic_ret));
    }

    /* --- Start the mic level uploader task (independent of HR loop) ---
     * Reports dBFS to OneNET every second regardless of HR/finger state.
     * Pinned to core 1 alongside mic_recorder so core 0 stays free for HR. */
    xTaskCreateStaticPinnedToCore(
        mic_uploader_task,
        "mic_upl",
        MIC_UPLOADER_STACK_BYTES / sizeof(StackType_t),
        NULL,
        MIC_UPLOADER_PRIO,
        s_mic_uploader_stack,
        &s_mic_uploader_tcb,
        1);
    ESP_LOGI(TAG, "mic_uploader task created");

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
    if (!collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE)) {
        ESP_LOGE(TAG, "Initial collect failed, retrying after restart...");
        maxim_max30102_restart_measurement();
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE)) {
            ESP_LOGE(TAG, "Collect failed twice, continuing anyway...");
        }
    }
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
        if (!collect_samples(&aun_red_buffer[400], &aun_ir_buffer[400], 100)) {
            ESP_LOGE(TAG, "Incremental collect failed, sensor may be dead.");
            ESP_LOGI(TAG, "Attempting full restart + signal-stable...");
            maxim_max30102_restart_measurement();
            wait_for_signal_stable();
            ESP_LOGI(TAG, "Recollecting 500 samples...");
            collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE);
            continue;
        }

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
            if (!collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE)) {
                ESP_LOGE(TAG, "Recollect after finger loss failed, retrying...");
                maxim_max30102_restart_measurement();
                vTaskDelay(pdMS_TO_TICKS(500));
                collect_samples(aun_red_buffer, aun_ir_buffer, BUFFER_SIZE);
            }
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

        /* --- Publish to OneNET --- */
        onenet_mqtt_publish_hr_spo2(n_heart_rate, hr_ok,
                                    n_sp02, spo2_ok);

        /* Note: dBFS publishing is handled by the independent mic_uploader
         * task (started in app_main), NOT here. This keeps noise reporting
         * running even when the HR loop stalls on wait_for_finger or
         * collect_samples recovery. */
    }
}
