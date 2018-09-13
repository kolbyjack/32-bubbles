#include "bbl_ota.h"
#include "bbl_utils.h"
#include "bbl_config.h"
#include "bbl_log.h"

#include <jsmn.h>
#include <http_parser.h>
#include <esp_tls.h>
#include <esp_ota_ops.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define OTA_BUFSIZ 8192

typedef struct bbl_ota_client bbl_ota_client_t;
typedef struct bbl_ota_header bbl_ota_header_t;
typedef struct http_parser_url http_parser_url_t;

static bool bbl_ota_check_performed = false;
static const char *bbl_ota_firmware_url = NULL;
static const char *bbl_ota_changelog_url = NULL;
static uint32_t bbl_ota_release_id = 0;
static BaseType_t bbl_ota_download_task = 0;

struct bbl_ota_header
{
    const char *key;
    const char *value;
};

struct bbl_ota_client
{
    http_parser parser;
    http_parser_settings parser_settings;
    esp_tls_t *tls;

    bool headers_complete;
    bool parsing_complete;

    bbl_ota_header_t headers[32];
    int headers_count;
    char *headers_end;

    char *body;
    size_t body_len;

    int argc;
    bbl_ota_header_t argv[32];

    char buf[OTA_BUFSIZ];
    size_t buf_used;

    // For json parsing
    jsmntok_t tokens[512];
    int token_count;

    int cur_token;
    bool error;

    jsmntok_t *firmware_url;
    jsmntok_t *changelog_url;
    jsmntok_t *release_id;

    // For writing update
    esp_ota_handle_t update_handle;
    esp_partition_t *update_partition;
};

static const char OTA_UPDATE_HOST[] = "api.github.com";
static const char OTA_UPDATE_PATH[] = "/repos/kolbyjack/32-bubbles/releases/latest";
static const char FIRMWARE_ASSET_NAME[] = "firmware.bin";
static const char CHANGELOG_ASSET_NAME[] = "CHANGELOG.txt";

static void bbl_ota_skip_array(bbl_ota_client_t *ctx, jsmntok_t *a);

static size_t jsmn_len(const jsmntok_t *t)
{
    return t->end - t->start;
}

static char *jsmn_strdup(const bbl_ota_client_t *ctx, const jsmntok_t *t)
{
    size_t tlen = jsmn_len(t);
    char *result = malloc(tlen + 1);

    memcpy(result, &ctx->body[t->start], tlen);
    result[tlen] = 0;

    return result;
}

static uint32_t jsmn_touint(const bbl_ota_client_t *ctx, const jsmntok_t *t)
{
    return strtoul(&ctx->body[t->start], NULL, 10);
}

static bool bbl_ota_jsmntok_streq(const char *js, jsmntok_t *t, const char *str)
{
    size_t tlen = jsmn_len(t);
    return strncmp(&js[t->start], str, tlen) == 0 && str[tlen] == 0;
}

static jsmntok_t *bbl_ota_next_token(bbl_ota_client_t *ctx)
{
    if (ctx->cur_token < ctx->token_count && !ctx->error) {
#if 0
        jsmntok_t *t = &ctx->tokens[ctx->cur_token];
        switch (t->type) {
        case JSMN_UNDEFINED: BBL_LOG("JSMN_UNDEFINED"); break;
        case JSMN_OBJECT: BBL_LOG("JSMN_OBJECT (%d keys)", t->size); break;
        case JSMN_ARRAY: BBL_LOG("JSMN_ARRAY (%d entries)n", t->size); break;
        case JSMN_STRING: BBL_LOG("JSMN_STRING (%.*s)", jsmn_len(t), &ctx->body[t->start]); break;
        case JSMN_PRIMITIVE: BBL_LOG("JSMN_PRMITIVE"); break;
        }
#endif
        return &ctx->tokens[ctx->cur_token++];
    } else {
        return NULL;
    }
}

static void bbl_ota_skip_object(bbl_ota_client_t *ctx, jsmntok_t *o)
{
    for (size_t i = 0; i < o->size && !ctx->error; ++i) {
        jsmntok_t *k = bbl_ota_next_token(ctx);
        jsmntok_t *v = bbl_ota_next_token(ctx);

        if (k == NULL || k->type != JSMN_STRING || v == NULL) {
            ctx->error = true;
        } else if (v->type == JSMN_OBJECT) {
            bbl_ota_skip_object(ctx, v);
        } else if (v->type == JSMN_ARRAY) {
            bbl_ota_skip_array(ctx, v);
        }
    }
}

static void bbl_ota_skip_array(bbl_ota_client_t *ctx, jsmntok_t *a)
{
    for (size_t i = 0; i < a->size && !ctx->error; ++i) {
        jsmntok_t *c = bbl_ota_next_token(ctx);

        if (c == NULL) {
            ctx->error = true;
        } else if (c->type == JSMN_OBJECT) {
            bbl_ota_skip_object(ctx, c);
        } else if (c->type == JSMN_ARRAY) {
            bbl_ota_skip_array(ctx, c);
        }
    }
}

static bool bbl_ota_parse_asset(bbl_ota_client_t *ctx, jsmntok_t *o)
{
    if (o == NULL || o->type != JSMN_OBJECT) {
        ctx->error = true;
        return false;
    }

    jsmntok_t *asset_name = NULL;
    jsmntok_t *asset_url = NULL;

    for (size_t i = 0; i < o->size && !ctx->error; ++i) {
        jsmntok_t *k = bbl_ota_next_token(ctx);
        jsmntok_t *v = bbl_ota_next_token(ctx);

        if (k == NULL || k->type != JSMN_STRING || v == NULL) {
            ctx->error = true;
        } else if (bbl_ota_jsmntok_streq(ctx->body, k, "name")) {
            if (v->type == JSMN_STRING) {
                asset_name = v;
            }
        } else if (bbl_ota_jsmntok_streq(ctx->body, k, "browser_download_url")) {
            if (v->type == JSMN_STRING) {
                asset_url = v;
            }
        } else if (v->type == JSMN_OBJECT) {
            bbl_ota_skip_object(ctx, v);
        } else if (v->type == JSMN_ARRAY) {
            bbl_ota_skip_array(ctx, v);
        }
    }

    if (asset_name != NULL && asset_url != NULL) {
        if (bbl_ota_jsmntok_streq(ctx->body, asset_name, FIRMWARE_ASSET_NAME)) {
            ctx->firmware_url = asset_url;
        } else if (bbl_ota_jsmntok_streq(ctx->body, asset_name, CHANGELOG_ASSET_NAME)) {
            ctx->changelog_url = asset_url;
        }
    }

    return !ctx->error;
}

static bool bbl_ota_parse_assets(bbl_ota_client_t *ctx, jsmntok_t *a)
{
    if (a == NULL || a->type != JSMN_ARRAY) {
        ctx->error = true;
        return false;
    }

    for (size_t i = 0; i < a->size && !ctx->error; ++i) {
        jsmntok_t *t = bbl_ota_next_token(ctx);

        if (t == NULL || t->type != JSMN_OBJECT) {
            ctx->error = true;
        } else {
            bbl_ota_parse_asset(ctx, t);
        }
    }

    return !ctx->error;
}

static bool bbl_ota_parse_release(bbl_ota_client_t *ctx, jsmntok_t *o)
{
    if (o == NULL || o->type != JSMN_OBJECT) {
        ctx->error = true;
        return false;
    }

    for (size_t i = 0; i < o->size && !ctx->error; ++i) {
        jsmntok_t *k = bbl_ota_next_token(ctx);
        jsmntok_t *v = bbl_ota_next_token(ctx);

        if (k == NULL || k->type != JSMN_STRING || v == NULL) {
            ctx->error = true;
        } else if (bbl_ota_jsmntok_streq(ctx->body, k, "assets")) {
            bbl_ota_parse_assets(ctx, v);
        } else if (bbl_ota_jsmntok_streq(ctx->body, k, "id")) {
            if (v != NULL && v->type == JSMN_PRIMITIVE && isdigit(ctx->body[v->start])) {
                ctx->release_id = v;
            }
        } else if (v->type == JSMN_OBJECT) {
            bbl_ota_skip_object(ctx, v);
        } else if (v->type == JSMN_ARRAY) {
            bbl_ota_skip_array(ctx, v);
        }
    }

    return !ctx->error;
}

static bool bbl_ota_parse_response(bbl_ota_client_t *ctx)
{
    jsmn_parser parser;

    jsmn_init(&parser);
    ctx->token_count = jsmn_parse(&parser, ctx->body, ctx->body_len, ctx->tokens, BBL_SIZEOF_ARRAY(ctx->tokens));

    if (bbl_ota_parse_release(ctx, bbl_ota_next_token(ctx))) {
        if (ctx->firmware_url != NULL && ctx->release_id != NULL) {
            free(bbl_ota_firmware_url);
            bbl_ota_firmware_url = jsmn_strdup(ctx, ctx->firmware_url);

            bbl_ota_release_id = jsmn_touint(ctx, ctx->release_id);
        }

        if (ctx->changelog_url != NULL) {
            free(bbl_ota_changelog_url);
            bbl_ota_changelog_url = jsmn_strdup(ctx, ctx->firmware_url);
        }
    }
}

static int bbl_ota_on_header_field(http_parser *parser, const char *at, size_t length)
{
    bbl_ota_client_t *client = parser->data;

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

static int bbl_ota_on_header_value(http_parser *parser, const char *at, size_t length)
{
    bbl_ota_client_t *client = parser->data;

    if (!(client->headers_count & 1)) {
        *client->headers_end = 0;
        client->headers[++client->headers_count / 2].value = at;
        client->headers_end = (char *)at;
    }
    client->headers_end += length;

    return 0;
}

static int bbl_ota_on_body(http_parser *parser, const char *at, size_t length)
{
    bbl_ota_client_t *client = parser->data;

    if (!client->body) {
        client->body = (char *)at;
    }
    client->body_len += length;

    return 0;
}

static int bbl_ota_copy_body(http_parser *parser, const char *at, size_t length)
{
    bbl_ota_client_t *client = parser->data;

    if (client->body) {
        memcpy(client->body + client->body_len, at, length);
        client->body_len += length;
    }

    return 0;
}

static int bbl_ota_write_firmware(http_parser *parser, const char *at, size_t length)
{
    bbl_ota_client_t *client = parser->data;
    esp_err_t err;

    if (client->update_handle == NULL) {
        client->update_partition = esp_ota_get_next_update_partition(NULL);
        if (client->update_partition == NULL) {
            return 1;
        }

        err = esp_ota_begin(client->update_partition, OTA_SIZE_UNKNOWN, &client->update_handle);
        if (err != ESP_OK || client->update_handle == NULL) {
            return 1;
        }
    }

    if (esp_ota_write(client->update_handle, at, length) == ESP_OK) {
        BBL_LOG("Wrote %d/%d firmware bytes\n", length, client->parser.content_length + length);
        return 0;
    } else {
        return 1;
    }
}

static int bbl_ota_firmware_complete(http_parser *parser)
{
    bbl_ota_client_t *client = parser->data;

    if (client->update_handle == NULL) {
        return 1;
    }

    if (esp_ota_end(client->update_handle) != ESP_OK) {
        return 1;
    }

    if (esp_ota_set_boot_partition(client->update_partition) == ESP_OK) {
        bbl_config_set_int(ConfigKeyReleaseID, bbl_ota_release_id);
        bbl_config_save();
        esp_restart();
    }
}

static int bbl_ota_on_headers_complete(http_parser *parser)
{
    bbl_ota_client_t *client = parser->data;

    client->headers_count = (client->headers_count + 1) / 2;
    if (client->headers_end != NULL) {
        *client->headers_end = 0;
    }

    client->headers_complete = true;

    return (client->parser.status_code / 100 == 3);
}

static int bbl_ota_on_message_complete(http_parser *parser)
{
    bbl_ota_client_t *client = parser->data;

    if (client->body) {
        client->body[client->body_len] = 0;
    }

    client->parsing_complete = true;

    return 0;
}

static void bbl_ota_client_init(bbl_ota_client_t *client)
{
    memset(client, 0, sizeof(*client));

    http_parser_init(&client->parser, HTTP_RESPONSE);
    client->parser.data = client;

    //client->tls = NULL;
    //client->headers_complete = false;
    //client->parsing_complete = false;
    client->headers_count = -1;
    //client->headers_end = NULL;
    //client->body = NULL;
    //client->body_len = 0;
    //client->argc = 0;
    //client->buf_used = 0;

    client->parser_settings.on_header_field = bbl_ota_on_header_field;
    client->parser_settings.on_header_value = bbl_ota_on_header_value;
    client->parser_settings.on_headers_complete = bbl_ota_on_headers_complete;
    client->parser_settings.on_body = bbl_ota_on_body;
    client->parser_settings.on_message_complete = bbl_ota_on_message_complete;
}

static void bbl_ota_download_update_thread(void *ctx)
{
    esp_tls_cfg_t cfg = {
        //.cacert_pem_buf = server_root_cert_pem_start,
        //.cacert_pem_bytes = server_root_cert_pem_end - server_root_cert_pem_start,
        .timeout_ms = -1,
    };

    bbl_ota_client_t *client = malloc(sizeof(bbl_ota_client_t));

    const char *next_url = strdup(bbl_ota_firmware_url);
    while (next_url != NULL) {
        bbl_ota_client_init(client);
        client->parser_settings.on_body = bbl_ota_write_firmware;
        client->parser_settings.on_message_complete = bbl_ota_firmware_complete;

        const char *this_url = next_url;
        next_url = NULL;

        http_parser_url_t url;
        http_parser_url_init(&url);
        if (http_parser_parse_url(this_url, strlen(this_url), false, &url) != 0) {
            goto done;
        }

        if ((url.field_set & (1 << UF_HOST) == 0) || (url.field_set & (1 << UF_PATH) == 0)) {
            goto done;
        }

        client->tls = esp_tls_conn_new(&this_url[url.field_data[UF_HOST].off], url.field_data[UF_HOST].len, 443, &cfg);
        if (client->tls == NULL) {
            goto done;
        }

        size_t buflen = snprintf(client->buf, sizeof(client->buf),
            "GET %s HTTP/1.1\r\n"
            "Host: %.*s\r\n"
            "User-Agent: 32-bubbles (http://github.com/kolbyjack/32-bubbles)\r\n"
            "Connection: Close\r\n"
            "\r\n"
            , &this_url[url.field_data[UF_PATH].off]
            , url.field_data[UF_HOST].len, &this_url[url.field_data[UF_HOST].off]
        );

        for (size_t written_bytes = 0; written_bytes < buflen; ) {
            ssize_t result = esp_tls_conn_write(client->tls, client->buf + written_bytes, buflen - written_bytes);
            if (result >= 0) {
                written_bytes += result;
            } else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                goto done;
            }
        }

        while (!client->parsing_complete) {
            ssize_t result = esp_tls_conn_read(client->tls, client->buf + client->buf_used,
                sizeof(client->buf) - client->buf_used);

            if (result > 0) {
                http_parser_execute(&client->parser, &client->parser_settings, client->buf + client->buf_used, result);
                client->buf_used += result;

                if (client->headers_complete) {
                    if (client->parser.status_code / 100 == 3) {
                        for (int i = 0; i < client->headers_count; ++i) {
                            if (strcasecmp(client->headers[i].key, "Location") == 0) {
                                next_url = strdup(client->headers[i].value);
                                break;
                            }
                        }
                        goto done;
                    }

                    client->buf_used = 0;
                }
            } else if (result < 0 && result != MBEDTLS_ERR_SSL_WANT_WRITE && result != MBEDTLS_ERR_SSL_WANT_READ) {
                goto done;
            }
        }

done:
        esp_tls_conn_delete(client->tls);
        free(this_url);
    }

    free(client);

    return false;
}

bool bbl_ota_refresh_info()
{
    // Only check once per boot
    if (bbl_ota_check_performed) {
        return bbl_ota_update_available();
    }

    esp_tls_cfg_t cfg = {
        //.cacert_pem_buf = server_root_cert_pem_start,
        //.cacert_pem_bytes = server_root_cert_pem_end - server_root_cert_pem_start,
        .timeout_ms = -1,
    };

    bbl_ota_client_t *client = malloc(sizeof(bbl_ota_client_t));
    bbl_ota_client_init(client);

    client->tls = esp_tls_conn_new(BBL_STRING_LITERAL_PARAM(OTA_UPDATE_HOST), 443, &cfg);
    if (client->tls == NULL) {
        goto exit;
    }

    size_t buflen = snprintf(client->buf, sizeof(client->buf),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: 32-bubbles (http://github.com/kolbyjack/32-bubbles/)\r\n"
        "Connection: Close\r\n"
        "\r\n"
        , OTA_UPDATE_PATH
        , OTA_UPDATE_HOST
    );

    for (size_t written_bytes = 0; written_bytes < buflen; ) {
        ssize_t result = esp_tls_conn_write(client->tls, client->buf + written_bytes, buflen - written_bytes);
        if (result >= 0) {
            written_bytes += result;
        } else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            goto exit;
        }
    }

    while (!client->parsing_complete) {
        ssize_t result = esp_tls_conn_read(client->tls, client->buf + client->buf_used,
            sizeof(client->buf) - client->buf_used);

        if (result > 0) {
            http_parser_execute(&client->parser, &client->parser_settings, client->buf + client->buf_used, result);
            client->buf_used += result;
        } else if (result < 0 && result != MBEDTLS_ERR_SSL_WANT_WRITE && result != MBEDTLS_ERR_SSL_WANT_READ) {
            break;
        }
    }

    // TODO: Handle redirection
    if (client->parsing_complete) {
        bbl_ota_parse_response(client);
        bbl_ota_check_performed = true;
    }

exit:
    esp_tls_conn_delete(client->tls);
    free(client);

    return bbl_ota_update_available();
}

bool bbl_ota_update_available()
{
    return bbl_ota_firmware_url != NULL && bbl_ota_release_id != 0 && bbl_ota_release_id != bbl_config_get_int(ConfigKeyReleaseID);
}

bool bbl_ota_download_update()
{
    if (!bbl_ota_update_available()) {
        return false;
    }

    if (bbl_ota_download_task == 0) {
        bbl_ota_download_task = xTaskCreate(bbl_ota_download_update_thread, "ota_update", 8192, NULL, 5, NULL);
    }

    return bbl_ota_download_task != 0;
}

bool bbl_ota_get_changelog(char *buf, size_t len)
{
    return false;
}
