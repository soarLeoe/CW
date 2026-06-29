/*
 * MAX30102 driver for ESP32-S3 (ESP-IDF v5.x new I2C master API)
 * Ported from Maxim MAXREFDES117# mbed driver.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "max30102.h"

static const char *TAG = "MAX30102";

static i2c_master_dev_handle_t s_dev_handle = NULL;
static i2c_master_bus_handle_t s_bus_handle = NULL;

#define I2C_RETRY_COUNT     3
#define I2C_RETRY_DELAY_MS  10

/* -----------------------------------------------------------------------
 * Internal I2C helpers (with retry + bus recovery)
 * --------------------------------------------------------------------- */

/* Reset the I2C bus by clocking 9 pulses to release a stuck slave. */
static void i2c_bus_recover(void)
{
    if (s_bus_handle == NULL) return;
    ESP_LOGW(TAG, "Recovering I2C bus...");
    i2c_master_bus_reset(s_bus_handle);
    vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
}

static bool i2c_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    for (int attempt = 0; attempt < I2C_RETRY_COUNT; attempt++) {
        esp_err_t ret = i2c_master_transmit(s_dev_handle, buf, 2, 100);
        if (ret == ESP_OK) return true;
        ESP_LOGW(TAG, "i2c_write_reg(0x%02X) attempt %d failed: %s",
                 reg, attempt + 1, esp_err_to_name(ret));
        i2c_bus_recover();
        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
    }
    ESP_LOGE(TAG, "i2c_write_reg(0x%02X) failed after %d retries", reg, I2C_RETRY_COUNT);
    return false;
}

static bool i2c_read_reg(uint8_t reg, uint8_t *value)
{
    for (int attempt = 0; attempt < I2C_RETRY_COUNT; attempt++) {
        esp_err_t ret = i2c_master_transmit_receive(s_dev_handle,
                                                    &reg, 1,
                                                    value, 1, 100);
        if (ret == ESP_OK) return true;
        ESP_LOGW(TAG, "i2c_read_reg(0x%02X) attempt %d failed: %s",
                 reg, attempt + 1, esp_err_to_name(ret));
        i2c_bus_recover();
        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
    }
    ESP_LOGE(TAG, "i2c_read_reg(0x%02X) failed after %d retries", reg, I2C_RETRY_COUNT);
    return false;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

bool maxim_max30102_write_reg(uint8_t uch_addr, uint8_t uch_data)
{
    return i2c_write_reg(uch_addr, uch_data);
}

bool maxim_max30102_read_reg(uint8_t uch_addr, uint8_t *puch_data)
{
    return i2c_read_reg(uch_addr, puch_data);
}

bool maxim_max30102_reset(void)
{
    return i2c_write_reg(REG_MODE_CONFIG, 0x40);
}

bool maxim_max30102_init(void)
{
    /* --- Configure I2C master bus --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    i2c_master_bus_handle_t bus_handle;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return false;
    }
    s_bus_handle = bus_handle;   /* saved for bus recovery */

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX30102_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .flags = { 0 },
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* --- Reset the sensor --- */
    if (!maxim_max30102_reset()) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* --- Read Part ID to verify connection --- */
    uint8_t part_id = 0;
    if (!maxim_max30102_read_reg(REG_PART_ID, &part_id)) {
        return false;
    }
    if (part_id != 0x15) {
        ESP_LOGE(TAG, "Part ID mismatch: got 0x%02X, expected 0x15", part_id);
        return false;
    }
    ESP_LOGI(TAG, "MAX30102 detected (Part ID = 0x%02X)", part_id);

    /* --- Initialise registers --- */
    if (!i2c_write_reg(REG_INTR_ENABLE_1, 0xC0))   return false;
    if (!i2c_write_reg(REG_INTR_ENABLE_2, 0x00))   return false;
    if (!i2c_write_reg(REG_FIFO_WR_PTR,   0x00))   return false;
    if (!i2c_write_reg(REG_OVF_COUNTER,   0x00))   return false;
    if (!i2c_write_reg(REG_FIFO_RD_PTR,   0x00))   return false;
    if (!i2c_write_reg(REG_FIFO_CONFIG,   0x0F))   return false; /* SMP_AVE=1 (no avg), rollover off, almost-full=17 */
    if (!i2c_write_reg(REG_MODE_CONFIG,   0x03))   return false; /* SpO2 mode */
    if (!i2c_write_reg(REG_SPO2_CONFIG,   0x27))   return false; /* ADC 4096, 100sps, 400us */
    if (!i2c_write_reg(REG_LED1_PA,       0x24))   return false; /* RED ~7mA */
    if (!i2c_write_reg(REG_LED2_PA,       0x24))   return false; /* IR ~7mA */
    if (!i2c_write_reg(REG_PILOT_PA,      0x7F))   return false;

    ESP_LOGI(TAG, "MAX30102 initialised: SDA=%d SCL=%d  100 sps, SpO2 mode",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    return true;
}

bool maxim_max30102_read_fifo(uint32_t *pun_red_led, uint32_t *pun_ir_led)
{
    uint8_t  uch_temp;
    uint8_t  ach_i2c_data[6];
    uint32_t un_temp;

    *pun_red_led = 0;
    *pun_ir_led  = 0;

    /* Read and clear interrupt status registers */
    i2c_read_reg(REG_INTR_STATUS_1, &uch_temp);
    i2c_read_reg(REG_INTR_STATUS_2, &uch_temp);

    /* Read 6 bytes from FIFO data register */
    uint8_t reg = REG_FIFO_DATA;
    for (int attempt = 0; attempt < I2C_RETRY_COUNT; attempt++) {
        esp_err_t ret = i2c_master_transmit_receive(s_dev_handle,
                                                    &reg, 1,
                                                    ach_i2c_data, 6, 100);
        if (ret == ESP_OK) break;
        if (attempt == I2C_RETRY_COUNT - 1) {
            ESP_LOGE(TAG, "read_fifo failed: %s", esp_err_to_name(ret));
            return false;
        }
        ESP_LOGW(TAG, "read_fifo attempt %d failed: %s, recovering bus",
                 attempt + 1, esp_err_to_name(ret));
        i2c_bus_recover();
        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
    }

    /* Assemble 18-bit RED value */
    un_temp  = (uint32_t)ach_i2c_data[0];
    un_temp <<= 16;
    *pun_red_led += un_temp;
    un_temp  = (uint32_t)ach_i2c_data[1];
    un_temp <<= 8;
    *pun_red_led += un_temp;
    *pun_red_led += (uint32_t)ach_i2c_data[2];

    /* Assemble 18-bit IR value */
    un_temp  = (uint32_t)ach_i2c_data[3];
    un_temp <<= 16;
    *pun_ir_led += un_temp;
    un_temp  = (uint32_t)ach_i2c_data[4];
    un_temp <<= 8;
    *pun_ir_led += un_temp;
    *pun_ir_led += (uint32_t)ach_i2c_data[5];

    /* Mask to 18 bits */
    *pun_red_led &= 0x03FFFF;
    *pun_ir_led  &= 0x03FFFF;

    return true;
}

/* ----------------------------------------------------------------------- */
/*  Restart measurement (after I2S interference etc.)                      */
/* ----------------------------------------------------------------------- */

bool maxim_max30102_restart_measurement(void)
{
    ESP_LOGI(TAG, "Restarting measurement (re-writing mode registers)...");

    /* Clear FIFO pointers */
    if (!i2c_write_reg(REG_FIFO_WR_PTR,   0x00)) return false;
    if (!i2c_write_reg(REG_OVF_COUNTER,   0x00)) return false;
    if (!i2c_write_reg(REG_FIFO_RD_PTR,   0x00)) return false;

    /* Clear interrupt status */
    uint8_t tmp;
    i2c_read_reg(REG_INTR_STATUS_1, &tmp);
    i2c_read_reg(REG_INTR_STATUS_2, &tmp);

    /* Re-write configuration registers (these may have been corrupted) */
    if (!i2c_write_reg(REG_FIFO_CONFIG,   0x0F)) return false; /* no avg, rollover off */
    if (!i2c_write_reg(REG_MODE_CONFIG,   0x03)) return false; /* SpO2 mode — STARTS sampling */
    if (!i2c_write_reg(REG_SPO2_CONFIG,   0x27)) return false; /* ADC 4096, 100sps, 400us */
    if (!i2c_write_reg(REG_LED1_PA,       0x24)) return false; /* RED ~7mA */
    if (!i2c_write_reg(REG_LED2_PA,       0x24)) return false; /* IR ~7mA */
    if (!i2c_write_reg(REG_PILOT_PA,      0x7F)) return false;

    ESP_LOGI(TAG, "Measurement restarted: SpO2 mode, 100 sps");
    return true;
}

void maxim_max30102_dump_regs(void)
{
    uint8_t mode = 0, spo2 = 0, fifo_cfg = 0, wr = 0, rd = 0;
    i2c_read_reg(REG_MODE_CONFIG,   &mode);
    i2c_read_reg(REG_SPO2_CONFIG,   &spo2);
    i2c_read_reg(REG_FIFO_CONFIG,   &fifo_cfg);
    i2c_read_reg(REG_FIFO_WR_PTR,   &wr);
    i2c_read_reg(REG_FIFO_RD_PTR,   &rd);
    ESP_LOGI(TAG, "REGS: MODE=0x%02X SPO2=0x%02X FIFO_CFG=0x%02X WR=%d RD=%d",
             mode, spo2, fifo_cfg, wr & 0x1F, rd & 0x1F);
}
