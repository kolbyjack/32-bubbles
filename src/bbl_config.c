// Copyright (C) Jonathan Kolb

#include "bbl_config.h"
#include "bbl_utils.h"

#include <nvs.h>

#define BBL_CONFIG_FILENAME "32-bubbles"
#define BBL_FIRMWARE_BUILD __DATE__ " " __TIME__

typedef enum {
    StringValue,
    IntValue,
} bbl_config_item_type_t;

typedef union {
    char *str_val;
    int int_val;
} bbl_config_value_t;

typedef struct {
    const char *name;
    bbl_config_item_type_t type;
    bbl_config_value_t default_value;
    bbl_config_value_t value;
    bool read_only;
} bbl_config_item_t;

static bbl_config_item_t bbl_config_items[] =
{
    { "boot_mode",  IntValue,    { .int_val = 0    }, { .int_val = 0    }, false },
    { "boot_count", IntValue,    { .int_val = 0    }, { .int_val = 0    }, true  },
    { "build_date", StringValue, { .str_val = ""   }, { .str_val = NULL }, true  },

    { "hostname",   StringValue, { .str_val = ""   }, { .str_val = NULL }, false },

    { "wifi_ssid",  StringValue, { .str_val = ""   }, { .str_val = NULL }, false },
    { "wifi_pass",  StringValue, { .str_val = ""   }, { .str_val = NULL }, false },

    { "mqtt_host",  StringValue, { .str_val = ""   }, { .str_val = NULL }, false },
    { "mqtt_port",  IntValue,    { .int_val = 1883 }, { .int_val = 0    }, false },
    { "mqtt_user",  StringValue, { .str_val = ""   }, { .str_val = NULL }, false },
    { "mqtt_pass",  StringValue, { .str_val = ""   }, { .str_val = NULL }, false },
};

BBL_STATIC_ASSERT(BBL_SIZEOF_ARRAY(bbl_config_items) == ConfigKeyCount);

static void bbl_config_set_strval(bbl_config_item_t *item, const char *value)
{
    if (item->value.str_val == value) {
        return;
    }

    if (item->value.str_val && value && strcmp(item->value.str_val, value) == 0) {
        return;
    }

    if (item->value.str_val != item->default_value.str_val) {
        free(item->value.str_val);
    }

    if (value) {
        if (strcmp(value, item->default_value.str_val) == 0) {
            value = item->default_value.str_val;
        } else {
            value = strdup(value);
        }
    }

    item->value.str_val = value;
}

void bbl_config_reset()
{
    for (int i = 0; i < BBL_SIZEOF_ARRAY(bbl_config_items); ++i) {
        switch (bbl_config_items[i].type) {
        case StringValue:
            bbl_config_set_strval(&bbl_config_items[i], bbl_config_items[i].default_value.str_val);
            break;

        case IntValue:
            bbl_config_items[i].value.int_val = bbl_config_items[i].default_value.int_val;
            break;
        }
    }
}

void bbl_config_init()
{
    nvs_handle h;

    bbl_config_reset();
    if (nvs_open(BBL_CONFIG_FILENAME, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < BBL_SIZEOF_ARRAY(bbl_config_items); ++i) {
        switch (bbl_config_items[i].type) {
        case StringValue: {
            char buf[256];
            size_t buflen = sizeof(buf);

            if (nvs_get_str(h, bbl_config_items[i].name, buf, &buflen) == ESP_OK) {
                bbl_config_set_strval(&bbl_config_items[i], buf);
            }
            break;
        }

        case IntValue:
            nvs_get_i32(h, bbl_config_items[i].name, &bbl_config_items[i].value.int_val);
            break;
        }
    }

    int boot_count = bbl_config_get_int(ConfigKeyBootCount) + 1;
    if (strcmp(BBL_FIRMWARE_BUILD, bbl_config_get_string(ConfigKeyBuildDate)) != 0) {
        nvs_set_str(h, bbl_config_items[ConfigKeyBuildDate].name, BBL_FIRMWARE_BUILD);
        boot_count = 1;
    }
    bbl_config_items[ConfigKeyBootCount].value.int_val = boot_count;
    nvs_set_i32(h, bbl_config_items[ConfigKeyBootCount].name, boot_count);
    nvs_set_i32(h, bbl_config_items[ConfigKeyBootMode].name, BootModeNormal);
    nvs_commit(h);
    nvs_close(h);
}

void bbl_config_save()
{
    nvs_handle h;

    if (nvs_open(BBL_CONFIG_FILENAME, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < BBL_SIZEOF_ARRAY(bbl_config_items); ++i) {
        switch (bbl_config_items[i].type) {
        case StringValue:
            nvs_set_str(h, bbl_config_items[i].name, bbl_config_items[i].value.str_val);
            break;

        case IntValue:
            nvs_set_i32(h, bbl_config_items[i].name, bbl_config_items[i].value.int_val);
            break;
        }
    }

    nvs_commit(h);
    nvs_close(h);
}

bbl_config_key_t bbl_config_lookup_key(const char *name)
{
    for (int i = 0; i < BBL_SIZEOF_ARRAY(bbl_config_items); ++i) {
        if (strcasecmp(name, bbl_config_items[i].name) == 0) {
            return i;
        }
    }

    return ConfigKeyCount;
}

const char *bbl_config_get_string(bbl_config_key_t key)
{
    if (key < ConfigKeyCount) {
        bbl_config_item_t *item = &bbl_config_items[key];

        if (item->type == StringValue) {
            return item->value.str_val;
        }
    }

    return "";
}

int bbl_config_get_int(bbl_config_key_t key)
{
    if (key < ConfigKeyCount) {
        bbl_config_item_t *item = &bbl_config_items[key];

        if (item->type == IntValue) {
            return item->value.int_val;
        }
    }

    return 0;
}

void bbl_config_set_string(bbl_config_key_t key, const char *value)
{
    if (key < ConfigKeyCount) {
        bbl_config_item_t *item = &bbl_config_items[key];

        if (!item->read_only && item->type == StringValue) {
            bbl_config_set_strval(item, value);
        }
    }
}

void bbl_config_set_int(bbl_config_key_t key, int value)
{
    if (key < ConfigKeyCount) {
        bbl_config_item_t *item = &bbl_config_items[key];

        if (!item->read_only && item->type == IntValue) {
            item->value.int_val = value;
        }
    }
}
