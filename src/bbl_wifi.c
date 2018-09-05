// Copyright (C) Jonathan Kolb

#include "bbl_wifi.h"
#include "bbl_config.h"

EventGroupHandle_t bbl_wifi_event_group;

esp_err_t bbl_wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        if (bbl_config_get_string(ConfigKeyHostname)[0]) {
            ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA,
                bbl_config_get_string(ConfigKeyHostname)));
        }
        esp_wifi_connect();
        break;

    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT);
        break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
        xEventGroupClearBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        break;

    case SYSTEM_EVENT_AP_START:
        xEventGroupSetBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT);
        break;

    case SYSTEM_EVENT_AP_STOP:
        xEventGroupClearBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT);
        break;
    }

    return ESP_OK;
}

void bbl_wifi_init(void)
{
    bbl_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (bbl_config_get_string(ConfigKeyWiFiSSID)[0]) {
        wifi_config_t wifi_config = { 0 };
        strcpy((char *)wifi_config.sta.ssid, bbl_config_get_string(ConfigKeyWiFiSSID));
        strcpy((char *)wifi_config.sta.password, bbl_config_get_string(ConfigKeyWiFiPass));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    } else {
        wifi_config_t wifi_config = { 0 };
        strcpy((char *)wifi_config.ap.ssid, "32-bubbles");
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.beacon_interval = 100;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}
