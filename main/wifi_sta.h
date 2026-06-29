/*
 * wifi_sta.h -- WiFi Station mode helper for ESP32-S3 (ESP-IDF v5.3)
 *
 * Provides a blocking WiFi connection API: call wifi_sta_connect() and
 * it won't return until either the device gets an IP or the timeout
 * expires.
 */

#ifndef WIFI_STA_H_
#define WIFI_STA_H_

#include "esp_err.h"

/**
 * Initialize NVS, netif, default event loop, WiFi in STA mode, and
 * connect to the given AP.  Blocks until connected or timeout.
 *
 * @param ssid       WiFi SSID (max 32 chars)
 * @param password   WiFi password (max 64 chars)
 * @param timeout_ms Max wait time in milliseconds (0 = wait forever)
 * @return ESP_OK on success, ESP_FAIL on timeout
 */
esp_err_t wifi_sta_connect(const char *ssid, const char *password,
                           int timeout_ms);

#endif /* WIFI_STA_H_ */
