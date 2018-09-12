#ifndef __1b920985_2ce3_4714_a0b3_9cfc93d7e808__
#define __1b920985_2ce3_4714_a0b3_9cfc93d7e808__

#include "bbl_utils.h"

#include <freertos/FreeRTOS.h>
#include <stdio.h>

#ifndef BBL_ENABLE_LOGGING
    #define BBL_ENABLE_LOGGING 0
#endif

#if BBL_ENABLE_LOGGING
    #define BBL_LOG(fmt, ...) printf("%8.1f %s: " fmt "\n", bbl_millis() * 0.001f, pcTaskGetTaskName(xTaskGetCurrentTaskHandle()), ## __VA_ARGS__)
    #define BBL_LOGIF(pred, fmt, ...) do { if (pred) { BBL_LOG(fmt, ## __VA_ARGS__); } } while (0)
#else
    #define BBL_LOG(fmt, ...)
    #define BBL_LOGIF(pred, fmt, ...)
#endif

#endif
