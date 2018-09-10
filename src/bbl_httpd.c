// Copyright (C) Jonathan Kolb

#include "bbl_httpd.h"
#include "bbl_config.h"
#include "bbl_utils.h"
#include "bbl_wifi.h"
#include "bbl_httpd_resources.h"

#include <esp_ota_ops.h>
#include <http_parser.h>
#include <lwip/sockets.h>

#define STRING_LITERAL_PARAM(x) x, sizeof(x) - 1
#define HTTP_BUFSIZ 4096

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

    char inbuf[HTTP_BUFSIZ];
    size_t inbuf_used;
    http_keyvalue_t headers_in[32];
    int headers_in_count;
    char *headers_in_end;

    char *request_uri;
    size_t request_uri_len;

    char *request_body;
    size_t request_body_len;

    char varbuf[HTTP_BUFSIZ];
    size_t varbuf_used;
    int argc;
    http_keyvalue_t argv[32];
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

static size_t urldecode(http_client_t *client, char **dest, const char *src, size_t srclen)
{
    size_t varbuf_free = sizeof(client->varbuf) - client->varbuf_used;
    size_t decoded_len = 0;
    int i;

    *dest = client->varbuf + client->varbuf_used;

    if (0 == varbuf_free)
        return 0;

    while (*src && srclen-- && decoded_len + 1 < varbuf_free) {
        if ('%' == *src && srclen > 2 && (i = pack_byte(src + 1)) >= 0) {
            (*dest)[decoded_len++] = i;
            src += 3;
        } else {
            (*dest)[decoded_len++] = *src++;
        }
    }
    (*dest)[decoded_len] = 0;

    client->varbuf_used += decoded_len + 1;

    return decoded_len;
}

static void split_args(http_client_t *client, const char *args)
{
    client->argc = 0;
    if (args == NULL || *args == 0)
        return;

    for (; *args && client->argc < BBL_SIZEOF_ARRAY(client->argv); ++client->argc) {
        int n = strcspn(args, "=&");
        urldecode(client, &client->argv[client->argc].key, args, n);
        args += n + (args[n] != 0 && args[n] != '&');

        n = strcspn(args, "&");
        urldecode(client, &client->argv[client->argc].value, args, n);
        args += n + (args[n] != 0);
    }
}

static int httpd_on_url(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!client->request_uri)
        client->request_uri = (char *)at;
    client->request_uri_len += length;

    return 0;
}

static int httpd_on_header_field(http_parser *parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (client->headers_in_count & 1) {
        if (client->headers_in_end != NULL) {
            *client->headers_in_end = 0;
        }
        client->headers_in[++client->headers_in_count / 2].key = at;
        client->headers_in_end = (char *)at;
    }
    client->headers_in_end += length;

    return 0;
}

static int httpd_on_header_value(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!(client->headers_in_count & 1)) {
        *client->headers_in_end = 0;
        client->headers_in[++client->headers_in_count / 2].value = at;
        client->headers_in_end = (char *)at;
    }
    client->headers_in_end += length;

    return 0;
}

static int httpd_on_body(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!client->request_body) {
        client->request_body = (char *)at;
    }
    client->request_body_len += length;

    return 0;
}

static int httpd_on_headers_complete(http_parser* parser)
{
    http_client_t *client = parser->data;

    client->headers_in_count = (client->headers_in_count + 1) / 2;
    if (client->headers_in_end != NULL) {
        *client->headers_in_end = 0;
    }
    if (client->request_uri != NULL) {
        client->request_uri[client->request_uri_len] = 0;
    }

    http_parser_parse_url(client->request_uri, client->request_uri_len,
        client->parser.method == HTTP_CONNECT, &client->url);

    if (parser->method == HTTP_GET && (client->url.field_set & (1 << UF_QUERY)) != 0) {
        const char *query_string = client->request_uri + client->url.field_data[UF_QUERY].off;
        split_args(client, query_string);
    }

    client->headers_complete = true;

    return 0;
}

static int httpd_on_message_complete(http_parser *parser)
{
    http_client_t *client = parser->data;

    if (client->request_body) {
        client->request_body[client->request_body_len] = 0;

        if (parser->method == HTTP_POST) {
            split_args(client, client->request_body);
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
    //client->inbuf_used = 0;
    client->headers_in_count = -1;
    //client->headers_in_end = NULL;
    //client->request_uri = NULL;
    //client->request_uri_len = 0;
    //client->request_body = NULL;
    //client->request_body_len = 0;
    //client->varbuf_used = 0;
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
        char *p = client->inbuf + client->inbuf_used;
        int n = sizeof(client->inbuf) - client->inbuf_used - 1;

        if ((n = read(client->sock, p, n)) <= 0) {
            break;
        }

        http_parser_execute(&client->parser, &client->parser_settings, p, n);
        client->inbuf_used += n;
    } while (!client->headers_complete && client->inbuf_used + 1 < sizeof(client->inbuf));
}

static void httpd_read_body(http_client_t *client)
{
    while (!client->parsing_complete && client->inbuf_used + 1 < sizeof(client->inbuf)) {
        char *p = client->inbuf + client->inbuf_used;
        int n = sizeof(client->inbuf) - client->inbuf_used - 1;

        if ((n = read(client->sock, p, n)) <= 0) {
            break;
        }

        http_parser_execute(&client->parser, &client->parser_settings, p, n);
        client->inbuf_used += n;
    }
}

static void httpd_get_index(http_client_t *client)
{
    write(client->sock, STRING_LITERAL_PARAM(
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
            "\"wifi_pass\": \"%js\","
            "\"mqtt_host\": \"%js\","
            "\"mqtt_port\": %u,"
            "\"mqtt_user\": \"%js\","
            "\"mqtt_pass\": \"%js\""
        "}",
        bbl_config_get_string(ConfigKeyHostname),
        bbl_config_get_string(ConfigKeyWiFiSSID),
        bbl_config_get_string(ConfigKeyWiFiPass),
        bbl_config_get_string(ConfigKeyMQTTHost),
        bbl_config_get_int(ConfigKeyMQTTPort),
        bbl_config_get_string(ConfigKeyMQTTUser),
        bbl_config_get_string(ConfigKeyMQTTPass)
    );

    write(client->sock, STRING_LITERAL_PARAM(
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

    write(client->sock, STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "Configuration applied!  Rebooting."
    ));

    for (int i = 0; i < client->argc; ++i) {
        bbl_config_key_t key = bbl_config_lookup_key(client->argv[i].key);

        switch (key) {
        case ConfigKeyHostname:
        case ConfigKeyMQTTHost:
            bbl_config_set_string(key, sanitize_hostname(client->argv[i].value));
            break;

        case ConfigKeyWiFiSSID:
        case ConfigKeyWiFiPass:
        case ConfigKeyMQTTUser:
        case ConfigKeyMQTTPass:
            bbl_config_set_string(key, client->argv[i].value);
            break;

        case ConfigKeyMQTTPort:
            bbl_config_set_int(key, atoi(client->argv[i].value));
            break;
        }
    }

    bbl_config_set_int(ConfigKeyBootMode, BootModeNormal);
    bbl_config_save();
    close(client->sock);
    esp_restart();
}

#if 0
static void httpd_put_firmware(http_client_t *client)
{
    esp_err_t err;
    esp_ota_handle_t update_handle = NULL;
    const esp_partition_t *update_partition;

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        return;
    }

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK || update_handle == NULL) {
        return;
    }

    if (client->request_body_len > 0) {
        err = esp_ota_write(update_handle, client->request_body, client->request_body_len);
    }

    while (err == ESP_OK) {
        client->inbuf_used = read(client->sock, client->inbuf, sizeof(client->inbuf));

        if (client->inbuf_used > 0) {
            err = esp_ota_write(update_handle, client->inbuf, client->inbuf_used);
        } else if (client->inbuf_used < 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (esp_ota_end(update_handle) != ESP_OK) {
        return;
    }

    if (err == ESP_OK && esp_ota_set_boot_partition(update_partition) == ESP_OK) {
        esp_restart();
    }
}
#endif

static void httpd_get_favicon(http_client_t *client)
{
    write(client->sock, STRING_LITERAL_PARAM(
        "HTTP/1.1 200 OK\r\n"
        "Connection: Close\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
    ));

    write(client->sock, BBL_RESOURCE(favicon), BBL_SIZEOF_RESOURCE(favicon));
}

static void httpd_404(http_client_t *client)
{
    write(client->sock, STRING_LITERAL_PARAM(
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

    const char *request_url = &client->request_uri[client->url.field_data[UF_PATH].off];
    return (strncmp(request_url, url, url_len) == 0);
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
#if 0
    } else if (httpd_check_url(client, "/firmware") && client->parser.method == HTTP_PUT) {
        httpd_put_firmware(client);
#endif
    } else if (httpd_check_url(client, "/favicon.ico") && client->parser.method == HTTP_GET) {
        httpd_get_favicon(client);
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
    xTaskCreate(httpd_task_thread, "httpd", 4096, NULL, 5, NULL);
}
