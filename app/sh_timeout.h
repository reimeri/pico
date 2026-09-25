#ifndef PICO_SH_TIMEOUT_H
#define PICO_SH_TIMEOUT_H

#include "json.h"

#include <string.h>

#define PICO_SH_DEFAULT_TIMEOUT_SECONDS 180
#define PICO_SH_STRINGIFY_(x) #x
#define PICO_SH_STRINGIFY(x) PICO_SH_STRINGIFY_(x)
#define PICO_SH_DEFAULT_TIMEOUT_SECONDS_STR PICO_SH_STRINGIFY(PICO_SH_DEFAULT_TIMEOUT_SECONDS)

static inline int PicoSh_TimeoutFromDoc(const JsonDoc *doc)
{
    int t = JsonObjInt(doc, 0, "timeout", PICO_SH_DEFAULT_TIMEOUT_SECONDS);
    return t > 0 ? t : PICO_SH_DEFAULT_TIMEOUT_SECONDS;
}

static inline int PicoSh_TimeoutSeconds(const char *args_json)
{
    if (!args_json || !args_json[0])
    {
        return PICO_SH_DEFAULT_TIMEOUT_SECONDS;
    }
    JsonDoc doc;
    if (JsonParse(&doc, args_json, strlen(args_json)) != 0)
    {
        return PICO_SH_DEFAULT_TIMEOUT_SECONDS;
    }
    int timeout_seconds = PicoSh_TimeoutFromDoc(&doc);
    JsonFree(&doc);
    return timeout_seconds;
}

#endif
