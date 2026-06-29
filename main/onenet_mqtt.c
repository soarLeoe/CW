/*
 * onenet_mqtt.c -- OneNET MQTT client for ESP32-S3 (ESP-IDF v5.3)
 *
 * Uses ESP-IDF's built-in esp_mqtt component (MQTT 3.1.1) to connect
 * to OneNET's MQTT broker and publish heart-rate / SpO2 data in
 * OneJSON format.
 *
 * Topic structure (OneNET thing model):
 *   Publish:  $sys/{pid}/{dev}/thing/property/post
 *   Subscribe:$sys/{pid}/{dev}/thing/property/post/reply
 *   Subscribe:$sys/{pid}/{dev}/thing/property/set
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "onenet_mqtt.h"

static const char *TAG = "ONENET";

/* --- Topic strings (built at init time from macros) --- */
static char s_topic_post[128];
static char s_topic_reply[128];
static char s_topic_set[128];

/* --- Runtime state --- */
static esp_mqtt_client_handle_t s_client  = NULL;
static bool s_mqtt_connected              = false;
static int  s_msg_id                      = 0;

/* ----------------------------------------------------------------------- */
/*  Event handler                                                          */
/* ----------------------------------------------------------------------- */

static void mqtt_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to OneNET");
        /* Subscribe to reply + set topics */
        esp_mqtt_client_subscribe(s_client, s_topic_reply, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_set, 1);
        ESP_LOGI(TAG, "Subscribed to reply + set topics");
        s_mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "Published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Topic=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "Data =%.*s", event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error: type=%d",
                 event->error_handle->error_type);
        if (event->error_handle->error_type ==
            MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  errno: %d, tls: %d",
                     event->error_handle->esp_transport_sock_errno,
                     event->error_handle->esp_tls_last_esp_err);
        }
        break;

    default:
        ESP_LOGD(TAG, "Event id=%d", (int)event_id);
        break;
    }
}

/* ----------------------------------------------------------------------- */
/*  Public API                                                             */
/* ----------------------------------------------------------------------- */

esp_err_t onenet_mqtt_init(void)
{
    /* Build topic strings */
    snprintf(s_topic_post,  sizeof(s_topic_post),
             "$sys/%s/%s/thing/property/post",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_reply, sizeof(s_topic_reply),
             "$sys/%s/%s/thing/property/post/reply",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_set,   sizeof(s_topic_set),
             "$sys/%s/%s/thing/property/set",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    ESP_LOGI(TAG, "Post topic: %s", s_topic_post);

    /* Configure MQTT client */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ONENET_MQTT_BROKER_URI,
        .credentials.client_id       = ONENET_DEVICE_NAME,
        .credentials.username        = ONENET_PRODUCT_ID,
        .credentials.authentication.password = ONENET_TOKEN,
        .network.timeout_ms          = 10000,
        .session.keepalive           = 60,
        .session.disable_clean_session = false,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started, connecting to OneNET...");
    return ESP_OK;
}

esp_err_t onenet_mqtt_publish_hr_spo2(int32_t hr,  bool hr_valid,
                                      int32_t spo2, bool spo2_valid)
{
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skipping publish");
        return ESP_ERR_INVALID_STATE;
    }

    if (!hr_valid && !spo2_valid) {
        ESP_LOGD(TAG, "Both HR and SpO2 invalid, skipping publish");
        return ESP_OK;
    }

    /* Build OneJSON payload */
    char payload[256];
    int offset = 0;

    offset += snprintf(payload + offset, sizeof(payload) - offset,
                       "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{",
                       ++s_msg_id);

    bool first = true;

    if (hr_valid) {
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                           "%s\"heart_rate\":{\"value\":%ld}",
                           first ? "" : ",", (long)hr);
        first = false;
    }

    if (spo2_valid) {
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                           "%s\"spo2\":{\"value\":%ld}",
                           first ? "" : ",", (long)spo2);
        first = false;
    }

    offset += snprintf(payload + offset, sizeof(payload) - offset, "}}");

    int msg_id = esp_mqtt_client_publish(
        s_client, s_topic_post, payload, offset, 1, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published [id=%d]: %s", s_msg_id, payload);
    return ESP_OK;
}

bool onenet_mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

esp_err_t onenet_mqtt_publish_noise_level(float dbfs)
{
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skipping publish");
        return ESP_ERR_INVALID_STATE;
    }

    /* Treat out-of-range low values as "no valid reading yet" and skip. */
    if (dbfs < -90.0f) {
        ESP_LOGD(TAG, "noise_level %.1f dBFS below threshold, skip", dbfs);
        return ESP_OK;
    }

    /* Clamp to the physical range reported in the thing model. */
    if (dbfs > 0.0f) dbfs = 0.0f;

    char payload[128];
    int len = snprintf(payload, sizeof(payload),
                       "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{"
                       "\"noise_level\":{\"value\":%.1f}"
                       "}}",
                       ++s_msg_id, dbfs);

    int msg_id = esp_mqtt_client_publish(
        s_client, s_topic_post, payload, len, 1, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish noise_level failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published [id=%d]: %s", s_msg_id, payload);
    return ESP_OK;
}
