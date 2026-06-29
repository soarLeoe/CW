/*
 * max98357.c — MAX98357A I2S amplifier driver for ESP32-S3 (ESP-IDF v5.x)
 *
 * Uses the new I2S standard driver API (driver/i2s_std.h).
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "max98357.h"

static const char *TAG = "MAX98357";

static i2s_chan_handle_t s_tx_handle = NULL;
static bool s_suspended = false;

/* ----------------------------------------------------------------------- */
/*  Public API                                                              */
/* ----------------------------------------------------------------------- */

esp_err_t max98357_init(uint32_t sample_rate)
{
    if (sample_rate == 0) {
        sample_rate = MAX98357_SAMPLE_RATE;
    }

    /* --- 1. Allocate I2S TX channel (master) --- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MAX98357_I2S_PORT,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;            /* 240 frames × 2 bytes = 480 B/desc */

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- 2. Configure standard Philips I2S mode --- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk  = MAX98357_BCLK_IO,
            .ws    = MAX98357_WS_IO,
            .dout  = MAX98357_DIN_IO,
            .din   = I2S_GPIO_UNUSED,
            .mclk  = I2S_GPIO_UNUSED,
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        return ret;
    }

    /* --- 3. Enable the channel --- */
    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MAX98357A initialised: DIN=%d BCLK=%d LRC=%d  %lu Hz, 16-bit mono",
             MAX98357_DIN_IO, MAX98357_BCLK_IO, MAX98357_WS_IO,
             (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t max98357_write(const uint8_t *data, size_t len, size_t *bytes_written)
{
    if (s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Auto-resume if suspended — caller doesn't need to manage this. */
    if (s_suspended) {
        i2s_channel_enable(s_tx_handle);
        s_suspended = false;
    }
    return i2s_channel_write(s_tx_handle, data, len, bytes_written, portMAX_DELAY);
}

esp_err_t max98357_suspend(void)
{
    if (s_tx_handle == NULL || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_disable(s_tx_handle);
    if (ret == ESP_OK) {
        s_suspended = true;
    }
    return ret;
}

esp_err_t max98357_resume(void)
{
    if (s_tx_handle == NULL || !s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_enable(s_tx_handle);
    if (ret == ESP_OK) {
        s_suspended = false;
    }
    return ret;
}

esp_err_t max98357_deinit(void)
{
    if (s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_channel_disable(s_tx_handle);
    i2s_del_channel(s_tx_handle);
    s_tx_handle = NULL;

    ESP_LOGI(TAG, "MAX98357A deinitialised");
    return ESP_OK;
}
