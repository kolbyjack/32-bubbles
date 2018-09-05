// Copyright (C) Jonathan Kolb

#include "bbl_mqtt.h"
#include "bbl_config.h"
#include "bbl_wifi.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>

static int mqtt_sock = -1;
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
    struct iovec iov_buf[16], *iov_local;
    int written;

    if (iovcnt > LWIP_ARRAYSIZE(iov_buf)) {
        return false;
    }

    memcpy(iov_buf, iov, sizeof(iov_buf[0]) * iovcnt);
    iov_local = iov_buf;

    do {
        written = lwip_writev_r(mqtt_sock, iov_local, iovcnt);
        if (written < 0) {
            bbl_mqtt_disconnect();
            return false;
        }

        while (written >= iov_local->iov_len) {
            written -= iov_local->iov_len;
            ++iov_local;
            --iovcnt;
        }

        if (written > 0) {
            iov_local->iov_base += written;
            iov_local->iov_len -= written;
        }
    } while (iovcnt > 0);

    return true;
}

static int mqtt_parse(const uint8_t *buf, size_t len)
{
    size_t pktlen = 0;

    size_t idx = 1;
    while (idx < len && idx < 5)
    {
        pktlen += (buf[idx] & 0x7f) << (7 * (idx - 1));
        if ((buf[idx++] & 0x80) == 0)
        {
            break;
        }
    }

    if (idx + pktlen > len)
    {
        return 0;
    }

    if (pktlen > sizeof(mqtt_buf))
    {
        mqtt_skip = idx + pktlen - len;
        return len;
    }

    // TODO: Actually parse packets
    return idx + pktlen;
}

static void fill_iovec(struct iovec *iov, void *base, size_t len)
{
    iov->iov_base = base;
    iov->iov_len = len;
}

bool bbl_mqtt_connect()
{
    const struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_buf[8];

    if (mqtt_sock != -1) {
        return true;
    }

    const char *host = bbl_config_get_string(ConfigKeyMQTTHost);
    uint16_t port = (uint16_t)bbl_config_get_int(ConfigKeyMQTTPort);
    const char *id = bbl_config_get_string(ConfigKeyHostname);
    const char *username = bbl_config_get_string(ConfigKeyMQTTUser);
    const char *password = bbl_config_get_string(ConfigKeyMQTTPass);

    xEventGroupWaitBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    __utoa(port, port_buf, 10);
    int err = getaddrinfo(host, port_buf, &hints, &res);
    if (err != 0 || res == NULL) {
        goto err;
    }

    mqtt_sock = socket(res->ai_family, res->ai_socktype, 0);
    if (mqtt_sock < 0) {
        goto err;
    }

    if (connect(mqtt_sock, res->ai_addr, res->ai_addrlen) != 0) {
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

    uint8_t connack_pkt[64];
    int connack_pkt_size;
    connack_pkt_size = recv(mqtt_sock, connack_pkt, sizeof(connack_pkt), 0);
    if (connack_pkt_size <= 0) {
        goto err;
    }

    // TODO: Parse connack_pkt

    freeaddrinfo(res);
    return true;

err:
    freeaddrinfo(res);
    bbl_sleep(5000);
    bbl_mqtt_disconnect();
    return false;
}

bool bbl_mqtt_disconnect()
{
    close(mqtt_sock);
    mqtt_sock = -1;
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

    bbl_mqtt_read();

    return mqtt_writev(iov, LWIP_ARRAYSIZE(iov));
}

void bbl_mqtt_read()
{
    int received;

    while ((received = recv(mqtt_sock, mqtt_buf + mqtt_buf_used, sizeof(mqtt_buf) - mqtt_buf_used, MSG_DONTWAIT)) > 0) {
        if (mqtt_skip >= received) {
            mqtt_skip -= received;
            continue;
        } else if (mqtt_skip > 0) {
            received -= mqtt_skip;
            memmove(&mqtt_buf[0], &mqtt_buf[mqtt_skip], received);
        }
        mqtt_buf_used += received;

        int parsed;
        while ((parsed = mqtt_parse(mqtt_buf + parsed, mqtt_buf_used - parsed)) > 0) {
            mqtt_buf_used -= parsed;
            memmove(&mqtt_buf[0], &mqtt_buf[parsed], mqtt_buf_used);
        }
    }
}
