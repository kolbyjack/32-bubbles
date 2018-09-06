#include "esp_altbeacon_api.h"

struct __attribute__((__packed__)) packed_esp_altbeacon_packet {
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

uint16_t htons(uint16_t const host) {
    uint8_t data[2] = {};
    memcpy(&data, &host, sizeof(data));

    return ((uint32_t) data[1] << 0)
         | ((uint32_t) data[0] << 8);
}

esp_err_t esp_altbeacon_decode(const uint8_t* buf, uint8_t len, esp_ble_altbeacon_t* res)
{
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct packed_esp_altbeacon_packet *pkt = buf;
    if (len != sizeof(*pkt) || pkt->adv_len != 0x1b || pkt->adv_type != 0xff || htons(pkt->beacon_code) != 0xbeac) {
        return ESP_ERR_NOT_FOUND;
    }

    res->adv_len = pkt->adv_len;
    res->adv_type = pkt->adv_type;
    res->mfg_id = pkt->mfg_id;
    res->beacon_code = htons(pkt->beacon_code);
    memcpy(res->beacon_id, pkt->beacon_id, sizeof(res->beacon_id));
    res->major = htons(pkt->major);
    res->minor = htons(pkt->minor);
    res->rssi = pkt->rssi;
    res->msg_res = pkt->msg_res;

    return ESP_OK;
}

