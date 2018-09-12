// Copyright (C) Jonathan Kolb

#ifndef __8c11f861_238f_4c63_89de_1fc794b4d55c__
#define __8c11f861_238f_4c63_89de_1fc794b4d55c__

#include <stdbool.h>
#include <stddef.h>

typedef enum bbl_config_key bbl_config_key_t;
typedef enum bbl_boot_mode bbl_boot_mode_t;

enum bbl_config_key {
    ConfigKeyVersion,
    ConfigKeyBuildDate,
    ConfigKeyBootCount,
    ConfigKeyReleaseID,

    ConfigKeyBootMode,

    ConfigKeyHostname,

    ConfigKeyWiFiSSID,
    ConfigKeyWiFiPass,

    ConfigKeyMQTTHost,
    ConfigKeyMQTTPort,
    ConfigKeyMQTTTLS,
    ConfigKeyMQTTUser,
    ConfigKeyMQTTPass,

    ConfigKeyCount
};

enum bbl_boot_mode {
    BootModeNormal,
    BootModeConfig,
};

void bbl_config_reset();
void bbl_config_init();
void bbl_config_save();

bbl_config_key_t bbl_config_lookup_key(const char *name);

const char *bbl_config_get_string(bbl_config_key_t key);
int bbl_config_get_int(bbl_config_key_t key);

void bbl_config_set_string(bbl_config_key_t key, const char *value);
void bbl_config_set_int(bbl_config_key_t key, int value);

#endif
