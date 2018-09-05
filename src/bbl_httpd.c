// Copyright (C) Jonathan Kolb

#include "bbl_httpd.h"
#include "bbl_config.h"
#include "bbl_utils.h"
#include "bbl_wifi.h"

#include "http_parser.h"
#include "lwip/sockets.h"

#define STRING_LITERAL_PARAM(x) x, sizeof(x) - 1
#define httpd_check_url(u) (client.url.field_data[UF_PATH].len == sizeof(u) - 1 && strncmp(&client.request_uri[client.url.field_data[UF_PATH].off], u, sizeof(u) - 1) == 0)

BBL_INCLUDE_RESOURCE(index, "res/index.html");
BBL_INCLUDE_RESOURCE(favicon, "res/bubbles.png");

typedef struct http_client http_client_t;
typedef struct http_parser_url http_parser_url_t;

struct http_client
{
    http_parser parser;
    http_parser_url_t url;
    int sock;

    bool parsing_complete;

    size_t bufsiz;
    char *inbuf;
    size_t incnt;
    char *in_headers[32];
    int inhdr_idx;
    char *inhdr_end;

    char *request_uri;
    size_t request_uri_len;

    char *request_body;
    size_t request_body_len;

    char *varbuf;
    size_t varcnt;
    int argc;
    char *argv[64];
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

static size_t urldecode(char *dest, size_t destlen, const char *src, size_t srclen)
{
    size_t outlen = 0;
    int i;

    if (0 == destlen)
        return 0;

    while (*src && srclen-- && outlen + 1 < destlen) {
        if ('%' == *src && srclen > 2 && (i = pack_byte(src + 1)) >= 0) {
            dest[outlen++] = i;
            src += 3;
        } else {
            dest[outlen++] = *src++;
        }
    }
    dest[outlen] = 0;

    return outlen;
}

static size_t add_var(http_client_t *client, char **var, const char *src, size_t srclen)
{
    size_t destlen = client->bufsiz - client->varcnt;

    *var = client->varbuf + client->varcnt;
    destlen = urldecode(*var, destlen, src, srclen);
    client->varcnt += destlen + 1;

    return destlen;
}

static void split_args(http_client_t *client, const char *args)
{
    int i = 0, n;

    client->argc = 0;
    if (args == NULL || *args == 0)
        return;

    while (*args && i < BBL_SIZEOF_ARRAY(client->argv)) {
        n = strcspn(args, "=&");
        add_var(client, &client->argv[i++], args, n);
        args += n + (0 != args[n] && '&' != args[n]);

        n = strcspn(args, "&");
        add_var(client, &client->argv[i++], args, n);
        args += n + (0 != args[n]);
    }

    client->argc = i;
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

    if (client->inhdr_idx & 1) {
        if (client->inhdr_idx > 0) {
            *client->inhdr_end = 0;
        }
        client->in_headers[++client->inhdr_idx] = client->inhdr_end = (char *)at;
    }
    client->inhdr_end += length;

    return 0;
}

static int httpd_on_header_value(http_parser* parser, const char *at, size_t length)
{
    http_client_t *client = parser->data;

    if (!(client->inhdr_idx & 1)) {
        *client->inhdr_end = 0;
        client->in_headers[++client->inhdr_idx] = client->inhdr_end = (char *)at;
    }
    client->inhdr_end += length;

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

static int httpd_on_message_complete(http_parser* parser)
{
    http_client_t *client = parser->data;

    ++client->inhdr_idx;
    *client->inhdr_end = 0;
    if (client->request_uri) {
        client->request_uri[client->request_uri_len] = 0;
    }
    if (client->request_body) {
        client->request_body[client->request_body_len] = 0;
    }

    http_parser_parse_url(client->request_uri, client->request_uri_len,
        client->parser.method == HTTP_CONNECT, &client->url);

    if (parser->method == HTTP_GET && (client->url.field_set & (1 << UF_QUERY)) != 0) {
        const char *query_string = client->request_uri + client->url.field_data[UF_QUERY].off;
        split_args(client, query_string);
    } else if (parser->method == HTTP_POST) {
        split_args(client, client->request_body);
    }

    client->parsing_complete = true;

    return 0;
}

static void httpd_task_thread()
{
    int httpd_sock = -1;
    http_client_t client;
    http_parser_settings parser_settings = {
        .on_url = httpd_on_url,
        .on_header_field = httpd_on_header_field,
        .on_header_value = httpd_on_header_value,
        .on_body = httpd_on_body,
        .on_message_complete = httpd_on_message_complete,
    };
    struct sockaddr_in sock_addr;

    static const size_t BUFFER_SIZE = 4096;
    client.bufsiz = BUFFER_SIZE;
    client.inbuf = malloc(client.bufsiz);
    client.varbuf = malloc(client.bufsiz);

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

        while ((client.sock = accept(httpd_sock, NULL, NULL)) >= 0) {
            http_parser_init(&client.parser, HTTP_REQUEST);
            client.parser.data = &client;

            http_parser_url_init(&client.url);
            client.parsing_complete = false;
            client.incnt = 0;
            client.inhdr_idx = -1;
            client.request_uri = NULL;
            client.request_uri_len = 0;
            client.request_body = NULL;
            client.request_body_len = 0;
            client.varcnt = 0;
            client.argc = 0;

            do {
                char *p = client.inbuf + client.incnt;
                int n = client.bufsiz - client.incnt - 1;

                if ((n = read(client.sock, p, n)) <= 0) {
                    break;
                }

                http_parser_execute(&client.parser, &parser_settings, p, n);
                client.incnt += n;
            } while (!client.parsing_complete && client.incnt + 1 < client.bufsiz);

            if (client.parsing_complete) {
                if ((client.url.field_set & (1 << UF_PATH)) == 0) {
                    // Bad request
                } else if (httpd_check_url("/")) {
                    write(client.sock, STRING_LITERAL_PARAM(
                        "HTTP/1.1 200 OK\r\n"
                        "Connection: Close\r\n"
                        "Content-Type: text/html\r\n"
                        "\r\n"
                    ));

                    write(client.sock, BBL_RESOURCE(index), BBL_SIZEOF_RESOURCE(index));
                } else if (httpd_check_url("/config")) {
                    if (client.parser.method == HTTP_GET) {
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

                        write(client.sock, STRING_LITERAL_PARAM(
                            "HTTP/1.1 200 OK\r\n"
                            "Connection: Close\r\n"
                            "Content-Type: application/json\r\n"
                            "\r\n"
                        ));

                        write(client.sock, response, response_len);
                    } else if (client.parser.method == HTTP_POST) {
                        write(client.sock, STRING_LITERAL_PARAM(
                            "HTTP/1.1 200 OK\r\n"
                            "Connection: Close\r\n"
                            "Content-Type: text/html\r\n"
                            "\r\n"
                            "Configuration applied!  Rebooting."
                        ));

                        for (int i = 0; i < client.argc; i += 2) {
                            bbl_config_key_t key = bbl_config_lookup_key(client.argv[i]);

                            switch (key) {
                            case ConfigKeyHostname:
                            case ConfigKeyMQTTHost:
                                bbl_config_set_string(key, sanitize_hostname(client.argv[i + 1]));
                                break;

                            case ConfigKeyWiFiSSID:
                            case ConfigKeyWiFiPass:
                            case ConfigKeyMQTTUser:
                            case ConfigKeyMQTTPass:
                                bbl_config_set_string(key, client.argv[i + 1]);
                                break;

                            case ConfigKeyMQTTPort:
                                bbl_config_set_int(key, atoi(client.argv[i + 1]));
                                break;
                            }
                        }

                        bbl_config_set_int(ConfigKeyBootMode, BootModeNormal);
                        bbl_config_save();
                        close(client.sock);
                        esp_restart();
                    }
                } else if (httpd_check_url("/favicon.ico")) {
                    write(client.sock, STRING_LITERAL_PARAM(
                        "HTTP/1.1 200 OK\r\n"
                        "Connection: Close\r\n"
                        "Content-Type: image/png\r\n"
                        "\r\n"
                    ));

                    write(client.sock, BBL_RESOURCE(favicon), BBL_SIZEOF_RESOURCE(favicon));
                } else {
                    write(client.sock, STRING_LITERAL_PARAM(
                        "HTTP/1.1 404 Not Found\r\n"
                        "Connection: Close\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n"
                        "Not Found"
                    ));
                }
            }

            close(client.sock);
        }
    }

    vTaskDelete(NULL);
}

void bbl_httpd_init()
{
    xTaskCreate(httpd_task_thread, "httpd", 4096, NULL, 5, NULL);
}
