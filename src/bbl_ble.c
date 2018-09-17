// Copyright (C) Jonathan Kolb

#include <freertos/FreeRTOS.h>
#include <esp_bt.h>
#include <esp_gap_ble_api.h>
#include <esp_task_wdt.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_ibeacon_api.h"
#include "esp_eddystone_api.h"
#include "esp_altbeacon_api.h"

#include "bbl_mqtt.h"
#include "bbl_config.h"
#include "bbl_utils.h"

#ifndef BBL_PUBLISH_STATS
    #define BBL_PUBLISH_STATS 0
#endif
#define BLE_BEACON_CACHE_SIZE 64
#define STATS_INTERVAL_SEC 60

typedef struct ble_scan_result_evt_param ble_scan_result_evt_param_t;
typedef struct beacon beacon_t;

struct beacon {
    uint8_t mac[6];
    int rssi;
    uint8_t adv_data[BBL_SIZEOF_FIELD(ble_scan_result_evt_param_t, ble_adv)];
    int adv_data_len;
};

static const esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

#if BBL_PUBLISH_STATS
uint boot_count;
uint32_t stats_millis;
uint adversitements_received;
uint raw_published;
uint ibeacon_published;
uint eddystone_published;
uint publishing_errors;
#define INC_STAT(stat) ++stat
#else
#define INC_STAT(stat)
#endif

static beacon_t beacon_cache[BLE_BEACON_CACHE_SIZE];
static int beacon_cache_count = 0;
static atomic_uint_fast32_t watchdog_tick_count;

static void bbl_ble_watchdog_thread()
{
    static const uint32_t WATCHDOG_TICK_MS = 10;
    static const uint32_t WATCHDOG_TIMEOUT_SEC = 30;
    static const uint32_t WATCHDOG_TIMEOUT_TICKS = WATCHDOG_TIMEOUT_SEC * 1000 / WATCHDOG_TICK_MS;

    atomic_store(&watchdog_tick_count, 0);
    while (atomic_fetch_add(&watchdog_tick_count, 1) < WATCHDOG_TIMEOUT_TICKS) {
        bbl_sleep(WATCHDOG_TICK_MS);
    }

    esp_restart();
    vTaskDelete(NULL);
}

static void bbl_ble_watchdog_reset()
{
    atomic_store(&watchdog_tick_count, 0);
}

static beacon_t *find_beacon(ble_scan_result_evt_param_t *d)
{
    for (int i = 0; i < beacon_cache_count; ++i) {
        if (memcmp(d->bda, beacon_cache[i].mac, sizeof(d->bda)) == 0) {
            return &beacon_cache[i];
        }
    }

    if (beacon_cache_count == BLE_BEACON_CACHE_SIZE) {
        --beacon_cache_count;
    }

    beacon_t *result = &beacon_cache[beacon_cache_count++];
    memcpy(result->mac, d->bda, sizeof(result->mac));
    return result;
}

static bool ble_publish(const char * const topic, const char * const payload, size_t payload_length)
{
    if (!bbl_mqtt_connect()) {
        return false;
    }

    if (bbl_mqtt_publish(topic, payload, payload_length)) {
        return true;
    } else {
        bbl_mqtt_disconnect();
        INC_STAT(publishing_errors);
        return false;
    }
}

static void publish_raw(beacon_t *beacon)
{
    char mqtt_buf[640];

    size_t topic_length = bbl_snprintf(mqtt_buf, sizeof(mqtt_buf), "happy-bubbles/ble/%s/raw/%.*hs",
        bbl_config_get_string(ConfigKeyHostname), sizeof(beacon->mac), beacon->mac
    );

    char *payload = mqtt_buf + topic_length + 1;
    size_t payload_length = bbl_snprintf(payload, sizeof(mqtt_buf) - (payload - mqtt_buf),
        "{"
            "\"hostname\":\"%js\","
            "\"mac\":\"%.*hs\","
            "\"rssi\":%d,"
            "\"data\":\"%.*hs\""
        "}",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(beacon->mac), beacon->mac,
        beacon->rssi,
        beacon->adv_data_len, beacon->adv_data
    );

    if (ble_publish(mqtt_buf, payload, payload_length)) {
        INC_STAT(raw_published);
    }
}

static void publish_ibeacon(beacon_t *beacon, const esp_ble_ibeacon_t *ib_data)
{
    char mqtt_buf[640];

    size_t topic_length = bbl_snprintf(mqtt_buf, sizeof(mqtt_buf), "happy-bubbles/ble/%s/ibeacon/%.*hs",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(ib_data->ibeacon_vendor.proximity_uuid), ib_data->ibeacon_vendor.proximity_uuid
    );

    char *payload = mqtt_buf + topic_length + 1;
    size_t payload_length = bbl_snprintf(payload, sizeof(mqtt_buf) - (payload - mqtt_buf),
        "{"
            "\"hostname\":\"%js\","
            "\"beacon_type\":\"ibeacon\","
            "\"mac\":\"%.*hs\","
            "\"rssi\":%d,"
            "\"data\":\"%.*hs\","
            "\"uuid\":\"%.*hs\","
            "\"major\":\"%04x\","
            "\"minor\":\"%04x\","
            "\"tx_power\":\"%02x\""
        "}",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(beacon->mac), beacon->mac,
        beacon->rssi,
        beacon->adv_data_len, beacon->adv_data,
        sizeof(ib_data->ibeacon_vendor.proximity_uuid), ib_data->ibeacon_vendor.proximity_uuid,
        ib_data->ibeacon_vendor.major,
        ib_data->ibeacon_vendor.minor,
        (uint8_t)ib_data->ibeacon_vendor.measured_power
    );

    if (ble_publish(mqtt_buf, payload, payload_length)) {
        INC_STAT(ibeacon_published);
    }
}

static void publish_eddystone(beacon_t *beacon, const esp_eddystone_result_t *es_data)
{
    if (es_data->common.frame_type != EDDYSTONE_FRAME_TYPE_UID) {
        return;
    }

    char mqtt_buf[640];

    size_t topic_length = bbl_snprintf(mqtt_buf, sizeof(mqtt_buf), "happy-bubbles/ble/%s/eddystone/%.*hs",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(es_data->inform.uid.namespace_id), es_data->inform.uid.namespace_id
    );

    char *payload = mqtt_buf + topic_length + 1;
    size_t payload_length = bbl_snprintf(payload, sizeof(mqtt_buf) - (payload - mqtt_buf),
        "{"
            "\"hostname\":\"%js\","
            "\"beacon_type\":\"eddystone\","
            "\"mac\":\"%.*hs\","
            "\"rssi\":%d,"
            "\"data\":\"%.*hs\","
            "\"namespace\":\"%.*hs\","
            "\"instance_id\":\"%.*hs\","
            "\"tx_power\":\"%02x\""
        "}",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(beacon->mac), beacon->mac,
        beacon->rssi,
        beacon->adv_data_len, beacon->adv_data,
        sizeof(es_data->inform.uid.namespace_id), es_data->inform.uid.namespace_id,
        sizeof(es_data->inform.uid.instance_id), es_data->inform.uid.instance_id,
        (uint8_t)es_data->inform.uid.ranging_data
    );

    if (ble_publish(mqtt_buf, payload, payload_length)) {
        INC_STAT(eddystone_published);
    }
}

static void publish_altbeacon(beacon_t *beacon, const esp_ble_altbeacon_t *ab_data)
{
    char mqtt_buf[640];

    size_t topic_length = bbl_snprintf(mqtt_buf, sizeof(mqtt_buf), "happy-bubbles/ble/%s/ibeacon/%.*hs",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(ab_data->beacon_id), ab_data->beacon_id
    );

    // Happy Bubbles Presence Server doesn't care about Altbeacons, so lie and say we're an iBeacon
    char *payload = mqtt_buf + topic_length + 1;
    size_t payload_length = bbl_snprintf(payload, sizeof(mqtt_buf) - (payload - mqtt_buf),
        "{"
            "\"hostname\":\"%js\","
            "\"beacon_type\":\"ibeacon\","
            "\"mac\":\"%.*hs\","
            "\"rssi\":%d,"
            "\"data\":\"%.*hs\","
            "\"uuid\":\"%.*hs\","
            "\"major\":\"%04x\","
            "\"minor\":\"%04x\","
            "\"tx_power\":\"%02x\""
        "}",
        bbl_config_get_string(ConfigKeyHostname),
        sizeof(beacon->mac), beacon->mac,
        beacon->rssi,
        beacon->adv_data_len, beacon->adv_data,
        sizeof(ab_data->beacon_id), ab_data->beacon_id,
        ab_data->major,
        ab_data->minor,
        (uint8_t)ab_data->rssi
    );

    if (ble_publish(mqtt_buf, payload, payload_length)) {
        INC_STAT(ibeacon_published);
    }
}

static void publish_ble_advertisement(beacon_t *beacon)
{
    esp_ble_ibeacon_t ib_data;
    esp_eddystone_result_t es_data;
    esp_ble_altbeacon_t ab_data;

    publish_raw(beacon);
    if (esp_ibeacon_decode(beacon->adv_data, beacon->adv_data_len, &ib_data) == ESP_OK) {
        publish_ibeacon(beacon, &ib_data);
    } else if (esp_eddystone_decode(beacon->adv_data, beacon->adv_data_len, &es_data) == ESP_OK) {
        publish_eddystone(beacon, &es_data);
    } else if (esp_altbeacon_decode(beacon->adv_data, beacon->adv_data_len, &ab_data) == ESP_OK) {
        publish_altbeacon(beacon, &ab_data);
    }
}

#if BBL_PUBLISH_STATS
static void publish_stats()
{
    char mqtt_buf[640];
    multi_heap_info_t hi;

    heap_caps_get_info(&hi, MALLOC_CAP_DEFAULT);

    size_t topic_length = bbl_snprintf(mqtt_buf, sizeof(mqtt_buf), "happy-bubbles/stats/%s",
        bbl_config_get_string(ConfigKeyHostname));

    char *payload = mqtt_buf + topic_length + 1;
    size_t payload_length = bbl_snprintf(payload, sizeof(mqtt_buf) - (payload - mqtt_buf),
        "{"
            "\"boot_count\":%u,"
            "\"seen\":%u,"
            "\"pub_raw\":%u,"
            "\"pub_ibeacon\":%u,"
            "\"pub_eddystone\":%u,"
            "\"pub_err\":%u,"
            "\"total_alloc\":%u,"
            "\"total_free\":%u"
        "}",
        boot_count,
        adversitements_received,
        raw_published,
        ibeacon_published,
        eddystone_published,
        publishing_errors,
        hi.total_allocated_bytes,
        hi.total_free_bytes
    );

    ble_publish(mqtt_buf, payload, payload_length);
}
#endif

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    bbl_ble_watchdog_reset();

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(1);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(1);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *p = (esp_ble_gap_cb_param_t *)param;
        ble_scan_result_evt_param_t *r = &p->scan_rst;

        if (r->search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            for (int i = 0; i < beacon_cache_count; ++i) {
                publish_ble_advertisement(&beacon_cache[i]);
                bbl_ble_watchdog_reset();
            }
            beacon_cache_count = 0;

#if BBL_PUBLISH_STATS
            uint32_t now = bbl_millis();
            if (now - stats_millis >= STATS_INTERVAL_SEC * 1000) {
                publish_stats();
                stats_millis = now;
                bbl_ble_watchdog_reset();
            }
#endif

            esp_ble_gap_start_scanning(1);
        } else  if (r->search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            beacon_t *beacon = find_beacon(r);
            beacon->rssi = r->rssi;
            memcpy(beacon->adv_data, r->ble_adv, sizeof(beacon->adv_data));
            beacon->adv_data_len = r->adv_data_len;

            INC_STAT(adversitements_received);
        }
        break;
    }
    }
}

void bbl_ble_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);

    esp_bluedroid_init();
    esp_bluedroid_enable();

    esp_err_t status;
    if ((status = esp_ble_gap_register_callback(ble_gap_cb)) != ESP_OK) {
        return;
    }

#if BBL_PUBLISH_STATS
    boot_count = bbl_config_get_int(ConfigKeyBootCount);
    stats_millis = bbl_millis();
#endif

    esp_ble_gap_set_scan_params(&ble_scan_params);

    xTaskCreate(bbl_ble_watchdog_thread, "ble_watchdog", 2048, NULL, 5, NULL);
}
