#ifndef __bc95198e_8ee3_4d5b_b5c1_951ff20ccd61__
#define __bc95198e_8ee3_4d5b_b5c1_951ff20ccd61__

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_gap_ble_api.h"
#include <stdint.h>

typedef struct esp_ble_altbeacon esp_ble_altbeacon_t;

struct esp_ble_altbeacon {
    uint8_t adv_len;
    uint8_t adv_type;
    uint16_t mfg_id;
    uint16_t beacon_code;
    uint8_t beacon_id[16];
    uint16_t major;
    uint16_t minor;
    int8_t rssi;
    uint8_t msg_res;
};

esp_err_t esp_altbeacon_decode(const uint8_t* buf, uint8_t len, esp_ble_altbeacon_t* res);

#ifdef __cplusplus
}
#endif

#endif
