// Copyright (C) Jonathan Kolb

#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_task_wdt.h>

#include "bbl_config.h"
#include "bbl_mqtt.h"
#include "bbl_wifi.h"

#define BUTTON_GPIO GPIO_NUM_0
#define LED_GPIO    GPIO_NUM_2

bbl_boot_mode_t boot_mode;

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    bbl_wifi_event_handler(ctx, event);

    return ESP_OK;
}

static void button_task_thread()
{
    bool button_pressed = false;
    uint32_t button_hold_count = 0;
    uint32_t button_press_millis = bbl_millis();
    uint32_t button_press_count = 0;

    for (;;) {
        bbl_sleep(10);

        if (!gpio_get_level(BUTTON_GPIO) != button_pressed) {
            uint32_t now = bbl_millis();

            if (now - button_press_millis > 2000) {
                // The first tracked button press was more than 2s ago
                button_press_count = 0;
                if (!button_pressed) {
                    // Button was just pressed
                    button_press_millis = now;
                }
            }

            if (button_pressed) {
                // Button was just released
                if (button_hold_count > 1000) {
                    // Button was held for 10s, reset config
                    bbl_config_reset();
                    bbl_config_save();
                    esp_restart();
                } else if (button_hold_count > 3 && ++button_press_count == 3) {
                    // Button was pressed three times within two seconds, restart in other mode
                    if (boot_mode == BootModeNormal) {
                        bbl_config_set_int(ConfigKeyBootMode, BootModeConfig);
                    } else {
                        bbl_config_set_int(ConfigKeyBootMode, BootModeNormal);
                    }
                    bbl_config_save();
                    esp_restart();
                }
            }

            button_hold_count = 0;
            button_pressed = !button_pressed;
        } else if (button_pressed) {
            ++button_hold_count;
        }
    }

    vTaskDelete(NULL);
}

static void io_init()
{
    // Configure button
    gpio_config_t btn_config;
    btn_config.intr_type = GPIO_INTR_DISABLE;
    btn_config.mode = GPIO_MODE_INPUT;
    btn_config.pin_bit_mask = 1 << BUTTON_GPIO;
    btn_config.pull_up_en = GPIO_PULLUP_DISABLE;
    btn_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&btn_config);

    // Configure LED
    gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    xTaskCreate(button_task_thread, "button", 2048, NULL, 5, NULL);
}

void app_main()
{
    esp_log_level_set("*", ESP_LOG_NONE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    ESP_ERROR_CHECK(esp_task_wdt_init(30, true));
    tcpip_adapter_init();

    bbl_config_init();

    boot_mode = bbl_config_get_int(ConfigKeyBootMode);
    if (bbl_config_get_string(ConfigKeyWiFiSSID)[0] == 0) {
        boot_mode = BootModeConfig;
    }

    io_init();
    bbl_wifi_init();
    if (boot_mode == BootModeConfig) {
        bbl_httpd_init();

        for (int i = 0; i < 3; ++i) {
            gpio_set_level(LED_GPIO, 1);
            bbl_sleep(100);
            gpio_set_level(LED_GPIO, 0);
            bbl_sleep(300);
        }
    } else {
        bbl_ble_init();

        gpio_set_level(LED_GPIO, 1);
        bbl_sleep(2000);
        gpio_set_level(LED_GPIO, 0);
    }
}
