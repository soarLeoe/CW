/*
 * inmp441.c — INMP441 I2S MEMS microphone driver for ESP32-S3 (ESP-IDF v5.x)
 *
 * Uses the new I2S standard driver API (driver/i2s_std.h), same family as
 * max98357.c, but on I2S_NUM_1 RX (master receive) instead of I2S_NUM_0 TX.
 *
 * Data on the wire is Philips I2S, 64 SCK per stereo frame (32 SCK per slot),
 * 24-bit data left-aligned in the 32-bit slot. The ESP32-S3 I2S engine reads
 * the full 32-bit slot; the low 8 bits come back as 0 and are simply ignored
 * by the application layer.
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "inmp441.h"

static const char *TAG = "INMP441";

static i2s_chan_handle_t s_rx_handle = NULL;
static bool s_suspended = false;
static uint32_t s_sample_rate = INMP441_SAMPLE_RATE;

/* ----------------------------------------------------------------------- */
/*  Public API                                                              */
/* ----------------------------------------------------------------------- */

esp_err_t inmp441_init(uint32_t sample_rate)
{
    if (sample_rate == 0) {
        sample_rate = INMP441_SAMPLE_RATE;
    }
    s_sample_rate = sample_rate;

    /* --- 1. Allocate I2S RX channel (master) on I2S_NUM_1 --- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(INMP441_I2S_PORT,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;            /* 240 frames × 4 bytes = 960 B/desc */

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- 2. Configure standard Philips I2S mode, RX ---
     * Slot width = 32 bit (so the wire carries 64 SCK / stereo frame, as the
     * INMP441 datasheet mandates). Data bit width = 32 (we keep the full
     * slot; the upper 24 bits are the real sample). Mono mode selects the
     * left slot by default, which matches L/R = GND on the INMP441. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = INMP441_SCK_IO,
            .ws   = INMP441_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = INMP441_SD_IO,
            .mclk = I2S_GPIO_UNUSED,
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    /* --- 3. Enable the channel --- */
    ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    /* INMP441 needs 2^18 SCK cycles (~85 ms @ SCK=3.072 MHz) after power-up
     * before it produces valid output. The first reads will return near-zero
     * samples; the application task handles this transparently. */
    ESP_LOGI(TAG, "INMP441 initialised: SCK=%d WS=%d SD=%d  %lu Hz, 32-bit slot / 24-bit data, mono-left",
             INMP441_SCK_IO, INMP441_WS_IO, INMP441_SD_IO,
             (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t inmp441_read(uint8_t *data, size_t len, size_t *bytes_read)
{
    if (s_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Auto-resume if suspended — caller doesn't need to manage this. */
    if (s_suspended) {
        i2s_channel_enable(s_rx_handle);
        s_suspended = false;
    }
    return i2s_channel_read(s_rx_handle, data, len, bytes_read, portMAX_DELAY);
}

esp_err_t inmp441_suspend(void)
{
    if (s_rx_handle == NULL || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_disable(s_rx_handle);
    if (ret == ESP_OK) {
        s_suspended = true;
    }
    return ret;
}

esp_err_t inmp441_resume(void)
{
    if (s_rx_handle == NULL || !s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_enable(s_rx_handle);
    if (ret == ESP_OK) {
        s_suspended = false;
    }
    return ret;
}

esp_err_t inmp441_deinit(void)
{
    if (s_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    s_suspended = false;

    ESP_LOGI(TAG, "INMP441 deinitialised");
    return ESP_OK;
}

/* Expose sample rate to the application layer via a getter so mic_recorder.c
 * does not need to duplicate the constant. */
uint32_t inmp441_get_sample_rate(void)
{
    return s_sample_rate;
}
