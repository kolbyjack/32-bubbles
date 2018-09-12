// Copyright (C) Jonathan Kolb

#ifndef __59b1eb63_679e_4cc5_ba82_1fe574e2d000__
#define __59b1eb63_679e_4cc5_ba82_1fe574e2d000__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool bbl_mqtt_connect();
bool bbl_mqtt_disconnect();
bool bbl_mqtt_publish(const char *topic, const void *payload, size_t payload_len);
// TODO: Need to set callbacks for bbl_mqtt_read to invoke
void bbl_mqtt_read(bool block);

#endif
