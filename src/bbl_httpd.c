// Copyright (C) Jonathan Kolb

#include "bbl_httpd.h"
#include "bbl_config.h"
#include "bbl_ota.h"
#include "bbl_utils.h"
#include "bbl_wifi.h"
#include "bbl_httpd_resources.h"

#include <esp_ota_ops.h>
#include <http_parser.h>
#include <lwip/sockets.h>
#include <ctype.h>
#include <string.h>

#define HTTP_BUFSIZ 8192

typedef struct http_client http_client_t;
typedef struct http_parser_url http_parser_url_t;
typedef struct http_keyvalue http_keyvalue_t;

struct http_keyvalue
{
    const char *key;
    const char *value;
};

struct http_client
{
    http_parser parser;
    http_parser_settings parser_settings;
    http_parser_url_t url;
    int sock;

    bool headers_complete;
    bool parsing_complete;

    http_keyvalue_t headers[32];
    int headers_count;
    char *headers_end;

    char *uri;
    size_t uri_len;

    char *body;
    size_t body_len;

    int argc;
    http_keyvalue_t argv[32];

    char buf[HTTP_BUFSIZ];
    size_t buf_used;
};

static char *sanitize_hostname(char *str)
{
    for (char *p = str; *p; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '.')) {
            *p = '-';
        }
    }

    return str;
}

static int pack_byte(const char *p)
{
    int ret;

    if ('0' <= p[0] && p[0] <= '9')
        ret = (p[0] - '0') << 4;
    else if ('A' <= p[0] && p[0] <= 'F')
        ret = (p[0] + 10 - 'A') << 4;
    else if ('a' <= p[0] && p[0] <= 'f')
        ret = (p[0] + 10 - 'a') << 4;
    else
        return -1;

    if ('0' <= p[1] && p[1] <= '9')
        ret |= p[1] - '0';
    else if ('A' <= p[1] && p[1] <= 'F')
        ret |= p[1] + 10 - 'A';
    else if ('a' <= p[1] && p[1] <= 'f')
        ret |= p[1] + 10 - 'a';
    else
        return -1;

    return ret;
}

static char *urldecode(char *encoded, const char *delim)
{
    char *decoded = encoded;

    while (*encoded != 0 && strchr(delim, *encoded) == NULL) {
        if (*encoded == '%' && isxdigit(encoded[1]) && isxdigit(encoded[2])) {
            *decoded++ = pack_byte(encoded + 1);
            encoded += 3;
        } else {
            *decoded++ = *encoded++;
        }
    }

    if (*encoded != 0) {
        ++encoded;
    }

    *decoded = 0;

    return encoded;
}

static void decode_args(http_client_t *client, char *buf)
{
    client->argc = 0;
    if (buf == NULL || *buf == 0)
        return;

    for (; *buf && client->argc < BBL_SIZEOF_ARRAY(client->argv); ++client->argc) {
        http_keyvalue_t *arg = &client->argv[client->argc];

        arg->key = buf;
        arg->value = urldecode(buf, "&=");
        buf = urldecode(arg->value, "&");
    }
}

static int httpd_on_url(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!client->uri)
        client->uri = (char *)at;
    client->uri_len += length;

    return 0;
}

static int httpd_on_header_field(http_parser *parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (client->headers_count & 1) {
        if (client->headers_end != NULL) {
            *client->headers_end = 0;
        }
        client->headers[++client->headers_count / 2].key = at;
        client->headers_end = (char *)at;
    }
    client->headers_end += length;

    return 0;
}

static int httpd_on_header_value(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!(client->headers_count & 1)) {
        *client->headers_end = 0;
        client->headers[++client->headers_count / 2].value = at;
        client->headers_end = (char *)at;
    }
    client->headers_end += length;

    return 0;
}

static int httpd_on_headers_complete(http_parser* parser)
{
    http_client_t *client = parser->data;

    client->headers_count = (client->headers_count + 1) / 2;
    if (client->headers_end != NULL) {
        *client->headers_end = 0;
    }

    if (client->uri != NULL) {
        http_parser_parse_url(client->uri, client->uri_len,
            client->parser.method == HTTP_CONNECT, &client->url);

        if ((client->url.field_set & (1 << UF_PATH)) != 0) {
            char *path = &client->uri[client->url.field_data[UF_PATH].off];
            urldecode(path, "?");
        }

        if (parser->method == HTTP_GET && (client->url.field_set & (1 << UF_QUERY)) != 0) {
            const char *query_string = client->uri + client->url.field_data[UF_QUERY].off;
            decode_args(client, query_string);
        }
    }

    client->headers_complete = true;

    return 0;
}

static int httpd_on_body(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!client->body) {
        client->body = (char *)at;
    }
    client->body_len += length;

    return 0;
}

static int httpd_on_message_complete(http_parser *parser)
{
    http_client_t *client = parser->data;

    if (client->body) {
        client->body[client->body_len] = 0;

        if (parser->method == HTTP_POST) {
            decode_args(client, client->body);
        }
    }

    client->parsing_complete = true;

    return 0;
}

static void httpd_client_init(http_client_t *client, int sock)
{
    memset(client, 0, sizeof(*client));

    http_parser_init(&client->parser, HTTP_REQUEST);
    client->parser.data = client;

    http_parser_url_init(&client->url);

    client->sock = sock;
    //client->headers_complete = false;
    //client->parsing_complete = false;
    //client->buf_used = 0;
    client->headers_count = -1;
    //client->headers_end = NULL;
    //client->uri = NULL;
    //client->uri_len = 0;
    //client->body = NULL;
    //client->body_len = 0;
    //client->argc = 0;

    client->parser_settings.on_url = httpd_on_url;
    client->parser_settings.on_header_field = httpd_on_header_field;
    client->parser_settings.on_header_value = httpd_on_header_value;
    client->parser_settings.on_headers_complete = httpd_on_headers_complete;
    client->parser_settings.on_body = httpd_on_body;
    client->parser_settings.on_message_complete = httpd_on_message_complete;
}

static void httpd_read_headers(http_client_t *client)
{
    do {
        char *p = client->buf + client->buf_used;
        int n = sizeof(client->buf) - client->buf_used - 1;

        if ((n = read(client->sock, p, n)) <= 0) {
            break;
        }

        http_parser_execute(&client->parser, &client->parser_settings, p, n);
        client->buf_used += n;
    } while (!client->headers_complete && client->buf_used + 1 < sizeof(client->buf));
}

static void httpd_read_body(http_client_t *client)
{
    while (!client->parsing_complete && client->buf_used + 1 < sizeof(client->buf)) {
        char *p = client->buf + client->buf_used;
        int n = sizeof(client->buf) - client->buf_used - 1;

        if ((n = read(client->sock, p, n)) <= 0) {
            break;
        }

        http_parser_execute(&client->parser, &client->parser_settings, p, n);
        client->buf_used += n;
    }
}

static void httpd_get_index(http_client_t *client)
{
    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
    ));

    write(client->sock, BBL_RESOURCE(index), BBL_SIZEOF_RESOURCE(index));
}

static void httpd_get_config(http_client_t *client)
{
    char response[512];
    size_t response_len;

    response_len = bbl_snprintf(response, sizeof(response),
        "{"
            "\"hostname\": \"%js\","
            "\"wifi_ssid\": \"%js\","
            "\"mqtt_host\": \"%js\","
            "\"mqtt_port\": %u,"
            "\"mqtt_tls\": %s,"
            "\"mqtt_user\": \"%js\""
        "}",
        bbl_config_get_string(ConfigKeyHostname),
        bbl_config_get_string(ConfigKeyWiFiSSID),
        bbl_config_get_string(ConfigKeyMQTTHost),
        bbl_config_get_int(ConfigKeyMQTTPort),
        bbl_config_get_int(ConfigKeyMQTTTLS) ? "true" : "false",
        bbl_config_get_string(ConfigKeyMQTTUser)
    );

    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
    ));

    write(client->sock, response, response_len);
}

static void httpd_post_config(http_client_t *client)
{
    httpd_read_body(client);

    if (!client->parsing_complete) {
        return;
    }

    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "Configuration applied!  Rebooting."
    ));

    bbl_config_set_int(ConfigKeyMQTTTLS, false);

    for (int i = 0; i < client->argc; ++i) {
        bbl_config_key_t key = bbl_config_lookup_key(client->argv[i].key);

        switch (key) {
        case ConfigKeyHostname:
        case ConfigKeyMQTTHost:
            bbl_config_set_string(key, sanitize_hostname(client->argv[i].value));
            break;

        case ConfigKeyWiFiSSID:
        case ConfigKeyMQTTUser:
            bbl_config_set_string(key, client->argv[i].value);
            break;

        case ConfigKeyWiFiPass:
        case ConfigKeyMQTTPass:
            if (client->argv[i].value[0] != 0) {
                bbl_config_set_string(key, client->argv[i].value);
            }
            break;

        case ConfigKeyMQTTPort:
            bbl_config_set_int(key, atoi(client->argv[i].value));
            break;

        case ConfigKeyMQTTTLS:
            bbl_config_set_int(key, true);
            break;
        }
    }

    bbl_config_set_int(ConfigKeyBootMode, BootModeNormal);
    bbl_config_save();
    close(client->sock);
    esp_restart();
}

static void httpd_get_favicon(http_client_t *client)
{
    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
    ));

    write(client->sock, BBL_RESOURCE(favicon), BBL_SIZEOF_RESOURCE(favicon));
}

static void httpd_update_check(http_client_t *client)
{
    char response[64];
    size_t response_len;

    bbl_ota_refresh_info();

    response_len = bbl_snprintf(response, sizeof(response),
        "{\"update_available\": %s}",
        bbl_ota_update_available() ? "true" : "false"
    );

    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
    ));

    write(client->sock, response, response_len);
}

static void httpd_download_update(http_client_t *client)
{
    char response[64];
    size_t response_len;

    response_len = bbl_snprintf(response, sizeof(response),
        "{\"downloading\": %s}",
        bbl_ota_update_available() ? "true" : "false"
    );

    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
    ));

    write(client->sock, response, response_len);

    bbl_ota_download_update();
}

static void httpd_404(http_client_t *client)
{
    write(client->sock, BBL_STRING_LITERAL_PARAM(
        "HTTP/1.1 404 Not Found\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Not Found"
    ));
}

static bool httpd_check_url(http_client_t *client, const char *url)
{
    size_t url_len = strlen(url);

    if ((client->url.field_set & (1 << UF_PATH)) == 0) {
        return false;
    }

    if (client->url.field_data[UF_PATH].len != url_len) {
        return false;
    }

    const char *path = &client->uri[client->url.field_data[UF_PATH].off];
    return (strncmp(path, url, url_len) == 0);
}

static void httpd_route_request(http_client_t *client)
{
    if (httpd_check_url(client, "/") && client->parser.method == HTTP_GET) {
        httpd_get_index(client);
    } else if (httpd_check_url(client, "/config")) {
        if (client->parser.method == HTTP_GET) {
            httpd_get_config(client);
        } else if (client->parser.method == HTTP_POST) {
            httpd_post_config(client);
        } else {
            httpd_404(client);
        }
    } else if (httpd_check_url(client, "/favicon.ico") && client->parser.method == HTTP_GET) {
        httpd_get_favicon(client);
    } else if (httpd_check_url(client, "/updatecheck") && client->parser.method == HTTP_GET) {
        httpd_update_check(client);
    } else if (httpd_check_url(client, "/downloadupdate") && client->parser.method == HTTP_GET) {
        httpd_download_update(client);
    } else {
        httpd_404(client);
    }
}

static void httpd_task_thread()
{
    int httpd_sock = -1;
    int client_sock;
    http_client_t *client = malloc(sizeof(http_client_t));
    struct sockaddr_in sock_addr;

    for (;;) {
        if (httpd_sock != -1) {
            close(httpd_sock);
        }

        xEventGroupWaitBits(bbl_wifi_event_group, BBL_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

        httpd_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (httpd_sock < 0) {
            continue;
        }

        memset(&sock_addr, 0, sizeof(sock_addr));
        sock_addr.sin_family = AF_INET;
        sock_addr.sin_addr.s_addr = 0;
        sock_addr.sin_port = htons(80);
        if (bind(httpd_sock, (struct sockaddr*)&sock_addr, sizeof(sock_addr))) {
            continue;
        }

        if (listen(httpd_sock, 32)) {
            continue;
        }

        while ((client_sock = accept(httpd_sock, NULL, NULL)) >= 0) {
            httpd_client_init(client, client_sock);
            httpd_read_headers(client);

            if (client->headers_complete) {
                httpd_route_request(client);
            }

            close(client->sock);
        }
    }

    free(client);

    vTaskDelete(NULL);
}

void bbl_httpd_init()
{
    xTaskCreate(httpd_task_thread, "httpd", 8192, NULL, 5, NULL);
}
