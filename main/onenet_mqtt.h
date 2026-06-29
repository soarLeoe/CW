/*
 * onenet_mqtt.h -- OneNET (China Mobile IoT) MQTT client for ESP32-S3
 *
 * Before using, fill in ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, and
 * ONENET_TOKEN with values from your OneNET console.
 */

#ifndef ONENET_MQTT_H_
#define ONENET_MQTT_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ====================================================================== */
/*  USER CONFIGURATION -- replace with your OneNET info                   */
/* ====================================================================== */

/* Product ID from OneNET console (产品开发 → 产品详情) */
#define ONENET_PRODUCT_ID       "NuuS3vUVz5"

/* Device Name from OneNET console (设备管理 → 设备详情) */
#define ONENET_DEVICE_NAME      "ESP32S3_HR_02"

/* Token string — OneNET Studio 新版签名 (et\nmethod\nres\nversion, sha1, 产品级res)
 * 生成方法见 CW_project/tools/onenet_token_gen.py
 * 有效期 5 年, 过期后用脚本重新生成 */
#define ONENET_TOKEN            "version=2018-10-31&res=products%2FNuuS3vUVz5&et=1940423825&method=sha1&sign=M0ftAbCiezw0s7R3jQL0zo97tG8%3D"

/* MQTT broker URI (OneNET public broker, TCP non-encrypted) */
#define ONENET_MQTT_BROKER_URI  "mqtt://mqtts.heclouds.com:1883"

/* ====================================================================== */
/*  Public API                                                            */
/* ====================================================================== */

/**
 * Initialize the MQTT client, connect to OneNET broker, and subscribe
 * to the property-post-reply and property-set topics.
 *
 * Prerequisite: WiFi must be connected before calling this.
 *
 * @return ESP_OK on success
 */
esp_err_t onenet_mqtt_init(void);

/**
 * Publish heart-rate and SpO2 data to OneNET.
 *
 * Only properties marked valid are included in the payload.
 * If both are invalid, the function returns without publishing.
 *
 * @param hr         Heart rate in BPM
 * @param hr_valid   Whether HR value is valid
 * @param spo2       SpO2 percentage (0-100)
 * @param spo2_valid Whether SpO2 value is valid
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t onenet_mqtt_publish_hr_spo2(int32_t hr,  bool hr_valid,
                                      int32_t spo2, bool spo2_valid);

/**
 * Publish ambient noise level (from INMP441 microphone) to OneNET.
 *
 * Uploads the `noise_level` thing-model property. Caller passes the latest
 * dBFS reading from mic_recorder_get_level_db().
 *
 * @param dbfs  Noise level in dBFS, expected range -90 .. 0.
 *              Pass a value < -90 to skip publishing (treat as invalid).
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if MQTT not connected.
 */
esp_err_t onenet_mqtt_publish_noise_level(float dbfs);

/**
 * Check if the MQTT client is currently connected.
 */
bool onenet_mqtt_is_connected(void);

#endif /* ONENET_MQTT_H_ */
