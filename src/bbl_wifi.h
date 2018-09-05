// Copyright (C) Jonathan Kolb

#ifndef __4ac6a173_e60b_4d51_ac20_752dfe4964b8__
#define __4ac6a173_e60b_4d51_ac20_752dfe4964b8__

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_event.h>
#include <esp_wifi.h>

extern EventGroupHandle_t bbl_wifi_event_group;

static const int BBL_WIFI_CONNECTED_BIT = BIT0;

void bbl_wifi_init(void);
esp_err_t bbl_wifi_event_handler(void *ctx, system_event_t *event);

#endif
