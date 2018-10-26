// Copyright (C) Jonathan Kolb

#include "bbl_mqtt.h"
#include "bbl_config.h"
#include "bbl_wifi.h"

#include <esp_tls.h>

typedef enum mqtt_packetid mqtt_packetid_t;

enum mqtt_packetid {
    MQTT_FORBIDDEN   = 0x00,
    MQTT_CONNECT     = 0x10,
    MQTT_CONNACK     = 0x20,
    MQTT_PUBLISH     = 0x30,
    MQTT_PUBACK      = 0x40,
    MQTT_PUBREC      = 0x50,
    MQTT_PUBREL      = 0x60,
    MQTT_PUBCOMP     = 0x70,
    MQTT_SUBSCRIBE   = 0x80,
    MQTT_SUBACK      = 0x90,
    MQTT_UNSUBSCRIBE = 0xa0,
    MQTT_UNSUBACK    = 0xb0,
    MQTT_PINGREQ     = 0xc0,
    MQTT_PINGRESP    = 0xd0,
    MQTT_DISCONNECT  = 0xe0,
    MQTT_RESERVED    = 0xf0,
};

static esp_tls_t *mqtt_conn = NULL;
static bool mqtt_connack_received = false;
static uint8_t mqtt_buf[512];
static size_t mqtt_buf_used;
static size_t mqtt_skip;

static size_t mqtt_encode_len(uint8_t *buf, size_t len)
{
    size_t count = 0;

    do {
        *buf++ = ((len >= 128) << 7) | (len % 128);
        len /= 128;
        ++count;
    } while (len > 0);

    return count;
}

static size_t mqtt_decode_len(const uint8_t *buf, size_t *len)
{
    size_t result = 0;
    size_t count = 0;

    do {
        result += (*buf & 0x7f) << (7 * count);
        ++count;
    } while (*buf++ & 0x80 && count < 4);

    *len = result;
    return count;
}

static bool mqtt_writev(const struct iovec *iov, int iovcnt)
{
    int iov_written = 0;

    for (int i = 0; i < iovcnt; ) {
        const int to_write = iov[i].iov_len - iov_written;
        int result = esp_tls_conn_write(mqtt_conn, iov[i].iov_base + iov_written, to_write);

        if (result >= 0) {
            iov_written += result;
            if (iov_written == iov[i].iov_len) {
                iov_written = 0;
                ++i;
            }
        } else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            bbl_mqtt_disconnect();
            return false;
        }
    }

    return true;
}

static int mqtt_parse(const uint8_t *buf, size_t len)
{
    size_t pktlen = 0;
    bool length_parsed = false;

    size_t idx = 1;
    for (; idx < len && !length_parsed; ++idx) {
        pktlen += (buf[idx] & 0x7f) << (7 * (idx - 1));
        length_parsed = ((buf[idx] & 0x80) == 0 || idx == 4);
    }

    if (!length_parsed) {
        return 0;
    }

    if (pktlen > sizeof(mqtt_buf)) {
        mqtt_skip = idx + pktlen - len;
        return len;
    }

    if (idx + pktlen > len) {
        return 0;
    }

    switch (buf[0] & 0xf0) {
    case MQTT_CONNACK:
        if (!mqtt_connack_received && pktlen == 2 && buf[idx + 1] == 0) {
            mqtt_connack_received = true;
        } else {
            bbl_mqtt_disconnect();
        }
        break;
    }

    return idx + pktlen;
}

static void fill_iovec(struct iovec *iov, void *base, size_t len)
{
    iov->iov_base = base;
    iov->iov_len = len;
}

bool bbl_mqtt_connect()
{
    if (mqtt_conn != NULL) {
        return true;
    }

    const char *host = bbl_config_get_string(ConfigKeyMQTTHost);
    uint16_t port = (uint16_t)bbl_config_get_int(ConfigKeyMQTTPort);
    bool tls = bbl_config_get_int(ConfigKeyMQTTTLS) != 0;
    const char *id = bbl_config_get_string(ConfigKeyHostname);
    const char *username = bbl_config_get_string(ConfigKeyMQTTUser);
    const char *password = bbl_config_get_string(ConfigKeyMQTTPass);

    xEventGroupWaitBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    esp_tls_cfg_t cfg = {0};

    mqtt_conn = esp_tls_conn_new(host, strlen(host), port, tls ? &cfg : NULL);
    if (mqtt_conn == NULL) {
        goto err;
    }

    uint8_t header[5];
    size_t header_len;
    uint16_t id_len = strlen(id);
    uint16_t id_len_be = htons(id_len);
    uint16_t username_len = username ? strlen(username) : 0;
    uint16_t username_len_be = htons(username_len);
    uint16_t password_len = password ? strlen(password) : 0;
    uint16_t password_len_be = htons(password_len);
    size_t body_len;

    // TODO: Struct-ify?
    uint8_t variable_header[10] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T', // protocol name
        0x04,                           // protocol level
        0x02,                           // flags
        0x00, 0x00                      // keepalive
    };
    body_len = sizeof(variable_header) + 2 + id_len;

    struct iovec iov[16];
    int iov_count = 2;

    fill_iovec(&iov[1], variable_header, sizeof(variable_header));

    fill_iovec(&iov[iov_count++], &id_len_be, sizeof(id_len_be));
    fill_iovec(&iov[iov_count++], id, id_len);

    if (username_len > 0) {
        variable_header[7] |= 0x80;
        body_len += 2 + username_len;
        fill_iovec(&iov[iov_count++], &username_len_be, sizeof(username_len_be));
        fill_iovec(&iov[iov_count++], username, username_len);
    }

    if (password_len > 0) {
        variable_header[7] |= 0x40;
        body_len += 2 + password_len;
        fill_iovec(&iov[iov_count++], &password_len_be, sizeof(password_len_be));
        fill_iovec(&iov[iov_count++], password, password_len);
    }

    // TODO: Last will, QoS, Keepalive

    header[0] = 0x10;
    header_len = 1 + mqtt_encode_len(&header[1], body_len);
    fill_iovec(&iov[0], header, header_len);

    if (!mqtt_writev(&iov, iov_count)) {
        goto err;
    }

    while (!mqtt_connack_received) {
        bbl_mqtt_read(true);
    }

    // TODO: Parse connack_pkt

    return true;

err:
    bbl_sleep(5000);
    bbl_mqtt_disconnect();
    return false;
}

bool bbl_mqtt_disconnect()
{
    esp_tls_conn_delete(mqtt_conn);
    mqtt_conn = NULL;
    mqtt_connack_received = false;
    mqtt_buf_used = 0;
    mqtt_skip = 0;

    return true;
}

bool bbl_mqtt_publish(const char *topic, const void *payload, size_t payload_len)
{
    uint8_t header[5];
    size_t header_len;
    uint16_t topic_len = strlen(topic);
    uint16_t topic_len_be = htons(topic_len);

    header[0] = 0x30;
    header_len = 1 + mqtt_encode_len(&header[1], 2 + topic_len + payload_len);

    struct iovec iov[] = {
        { header,        header_len },
        { &topic_len_be, sizeof(topic_len_be) },
        { topic,         topic_len },
        { payload,       payload_len }
    };

    bbl_mqtt_read(false);

    return mqtt_writev(iov, LWIP_ARRAYSIZE(iov));
}

void bbl_mqtt_read(bool block)
{
    while (block || mbedtls_ssl_get_bytes_avail(&mqtt_conn->ssl) > 0) {
        void *read_ptr = mqtt_buf + mqtt_buf_used;
        int to_read = sizeof(mqtt_buf) - mqtt_buf_used;
        ssize_t received = esp_tls_conn_read(mqtt_conn, read_ptr, to_read);

        if (received == 0) {
            break;
        } else if (received > 0) {
            if (mqtt_skip >= received) {
                mqtt_skip -= received;
                continue;
            } else if (mqtt_skip > 0) {
                received -= mqtt_skip;
                memmove(&mqtt_buf[0], &mqtt_buf[mqtt_skip], received);
                mqtt_skip = 0;
            }
            mqtt_buf_used += received;

            int parsed;
            do {
                parsed = mqtt_parse(mqtt_buf, mqtt_buf_used);
                if (parsed > 0) {
                    mqtt_buf_used -= parsed;
                    memmove(&mqtt_buf[0], &mqtt_buf[parsed], mqtt_buf_used);
                    block = false;
                }
            } while (mqtt_buf_used > 0 && parsed > 0);
        } else if (MBEDTLS_ERR_SSL_WANT_READ != received && MBEDTLS_ERR_SSL_WANT_WRITE != received) {
            bbl_mqtt_disconnect();
            break;
        }
    }
}
