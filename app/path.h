#ifndef PICO_PATH_H
#define PICO_PATH_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define PICO_PRINTF_FORMAT(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define PICO_PRINTF_FORMAT(format_index, first_argument)
#endif

static inline bool PicoPath_Format(char *out, size_t cap, const char *format, ...)
    PICO_PRINTF_FORMAT(3, 4);

static inline bool PicoPath_Format(char *out, size_t cap, const char *format, ...)
{
    if (!out || cap == 0 || !format)
    {
        return false;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(out, cap, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= cap)
    {
        out[0] = '\0';
        return false;
    }
    return true;
}

#undef PICO_PRINTF_FORMAT

#endif
