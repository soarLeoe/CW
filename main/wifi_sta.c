/*
 * wifi_sta.c -- WiFi Station mode helper for ESP32-S3 (ESP-IDF v5.3)
 *
 * The first call to wifi_sta_connect() initializes NVS, the default
 * event loop, the default STA netif, and the WiFi driver.  Subsequent
 * reconnects reuse the existing infrastructure.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "wifi_sta.h"

static const char *TAG = "WIFI_STA";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           10

static EventGroupHandle_t s_evt_group;
static int s_retry_count  = 0;
static bool s_initialized = false;

/* ----------------------------------------------------------------------- */
/*  Event handler                                                          */
/* ----------------------------------------------------------------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected (reason=%d)", ev->reason);
            if (s_retry_count < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_count++;
                ESP_LOGI(TAG, "Retry %d/%d ...", s_retry_count, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_evt_group, WIFI_FAIL_BIT);
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_evt_group, WIFI_CONNECTED_BIT);
    }
}

/* ----------------------------------------------------------------------- */
/*  Public API                                                             */
/* ----------------------------------------------------------------------- */

esp_err_t wifi_sta_connect(const char *ssid, const char *password,
                           int timeout_ms)
{
    /* --- One-time init: NVS + netif + event loop + WiFi --- */
    if (!s_initialized) {
        /* NVS (WiFi stores calibration data here) */
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        ESP_ERROR_CHECK(esp_netif_init());

        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        s_evt_group = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        s_initialized = true;
    }

    /* --- Configure SSID / password --- */
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* Clear any stale bits before connecting */
    xEventGroupClearBits(s_evt_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to \"%s\" ...", ssid);

    /* --- Block until connected or timeout --- */
    TickType_t wait_ticks =
        (timeout_ms > 0) ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;

    EventBits_t bits = xEventGroupWaitBits(
        s_evt_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, wait_ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to \"%s\"", ssid);
    return ESP_FAIL;
}
