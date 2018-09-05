// Copyright (C) Jonathan Kolb

#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdint.h>

#include "bbl_utils.h"

uint32_t bbl_millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void bbl_sleep(uint32_t ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

size_t bbl_snprintf(char *buf, size_t bufsiz, const char *fmt, ...)
{
    static const size_t UNSET_PRECISION = (size_t)-1;
    static const char *hex = "0123456789abcdef";
    typedef enum printf_length printf_length_t;
    enum printf_length {
        PrintfLengthInt,
        PrintfLengthChar,
        PrintfLengthShort,
        PrintfLengthLong,
        PrintfLengthLongLong,
        PrintfLengthIntmaxT,
        PrintfLengthSizeT,
        PrintfLengthPtrdiffT,
        PrintfLengthLongDouble,
    };

    va_list args;
    size_t bufidx = 0;

    if (bufsiz == 0) {
        return 0;
    }

    va_start(args, fmt);

    while (bufidx + 1 < bufsiz && *fmt) {
        char c = *fmt++;

        if (c != '%') {
            buf[bufidx++] = c;
            continue;
        }

        size_t width = 0;
        size_t precision = UNSET_PRECISION;
        printf_length_t length = PrintfLengthInt;
        char fill = ' ';

        // Fill char
        c = *fmt++;
        if (c == '0') {
            fill = '0';
            c = *fmt++;
        }

        // Width
        if (c == '*') {
            width = va_arg(args, size_t);
            c = *fmt++;
        } else {
            while (isdigit(c)) {
                width = width * 10 + (c - '0');
                c = *fmt++;
            }
        }

        // Precision
        if (c == '.') {
            c = *fmt++;
            if (c == '*') {
                precision = va_arg(args, size_t);
                c = *fmt++;
            } else {
                precision = 0;
                while (isdigit(c)) {
                    precision = precision * 10 + (c - '0');
                    c = *fmt++;
                }
            }
        }

        // Length
        switch (c) {
        case 'h':
            if (*fmt == 'h') {
                length = PrintfLengthChar;
                ++fmt;
            } else {
                length = PrintfLengthShort;
            }
            break;

        case 'l':
            if (*fmt == 'l') {
                length = PrintfLengthLongLong;
                ++fmt;
            } else {
                length = PrintfLengthLong;
            }
            break;

        case 'j': length = PrintfLengthIntmaxT; break;
        case 'z': length = PrintfLengthSizeT; break;
        case 't': length = PrintfLengthPtrdiffT; break;
        case 'L': length = PrintfLengthLongDouble; break;
        }

        if (length != PrintfLengthInt) {
            c = *fmt++;
        }

        switch (c) {
        case 's': // String
            if (length == PrintfLengthShort) {
                const uint8_t *blob = va_arg(args, const uint8_t *);

                if (precision == UNSET_PRECISION) {
                    precision = 0;
                    while (blob[precision] != 0) {
                        ++precision;
                    }
                }

                for (; bufidx + 1 < bufsiz && precision * 2 < width; --width) {
                    buf[bufidx++] = fill;
                }

                for (size_t i = 0; bufidx + 1 < bufsiz && i < precision; ++i) {
                    buf[bufidx++] = hex[blob[i] >> 4];
                    if (bufidx + 1 < bufsiz) {
                        buf[bufidx++] = hex[blob[i] & 0x0f];
                    }
                }
            } else {
                const char *str = va_arg(args, const char *);
                size_t slen = strlen(str);

                if (precision != UNSET_PRECISION && precision < slen) {
                    slen = precision;
                }

                for (; bufidx + 1 < bufsiz && slen < width; --width) {
                    buf[bufidx++] = fill;
                }

                for (size_t i = 0; bufidx + 1 < bufsiz && i < slen; ++i) {
                    if (length == PrintfLengthIntmaxT && (str[i] == '\\' || str[i] == '"')) {
                        if (bufidx + 2 >= bufsiz) {
                            goto done;
                        }
                        buf[bufidx++] = '\\';
                    }
                    buf[bufidx++] = str[i];
                }
            }
            break;

        case 'd': { // Signed integer
            intmax_t n;
            switch (length) {
            case PrintfLengthLong:       n = va_arg(args, long); break;
            case PrintfLengthLongLong:   n = va_arg(args, long long); break;
            case PrintfLengthIntmaxT:    n = va_arg(args, intmax_t); break;
            case PrintfLengthSizeT:      n = va_arg(args, ssize_t); break;
            case PrintfLengthPtrdiffT:   n = va_arg(args, ptrdiff_t); break;
            default:                     n = va_arg(args, int); break;
            }

            char tbuf[32], *p = tbuf;
            char prefix = 0;

            if (n < 0) {
                prefix = '-';
                n = -n;
            }

            do {
                *p++ = hex[n % 10];
                n /= 10;
            } while (n > 0);

            if (prefix) {
                *p++ = prefix;
            }

            while (bufidx + 1 < bufsiz && width > p - tbuf) {
                buf[bufidx++] = fill;
                --width;
            }

            while (bufidx + 1 < bufsiz && p > tbuf) {
                buf[bufidx++] = *--p;
            }

            break;
        }

        case 'u': case 'x': { // Unsigned integer
            uintmax_t n;
            switch (length) {
            case PrintfLengthLong:       n = va_arg(args, unsigned long); break;
            case PrintfLengthLongLong:   n = va_arg(args, unsigned long long); break;
            case PrintfLengthIntmaxT:    n = va_arg(args, uintmax_t); break;
            case PrintfLengthSizeT:      n = va_arg(args, size_t); break;
            case PrintfLengthPtrdiffT:   n = va_arg(args, ptrdiff_t); break;
            default:                     n = va_arg(args, unsigned int); break;
            }

            char tbuf[32], *p = tbuf;
            int base = 10;

            if (c == 'x') {
                base = 16;
            }

            do {
                *p++ = hex[n % base];
                n /= base;
            } while (n > 0);

            while (bufidx + 1 < bufsiz && width > p - tbuf) {
                buf[bufidx++] = fill;
                --width;
            }

            while (bufidx + 1 < bufsiz && p > tbuf) {
                buf[bufidx++] = *--p;
            }

            break;
        }

        case '%': // Literal %
            buf[bufidx++] = '%';
            break;
        }
    }

done:
    buf[bufidx] = 0;

    va_end(args);

    return bufidx;
}
