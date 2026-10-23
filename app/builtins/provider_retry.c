#define _POSIX_C_SOURCE 200809L

#include "builtins/provider_retry.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool Contains(const char *text, const char *needle)
{
    for (const char *p = text; p && *p; p++)
    {
        size_t i = 0;
        while (needle[i] && p[i] && tolower((unsigned char)p[i]) == needle[i]) i++;
        if (!needle[i]) return true;
    }
    return false;
}

bool pico_provider_capacity_message(const char *message)
{
    static const char *const markers[] = {
        "overload", "server capacity", "at capacity", "over capacity", "insufficient capacity",
        "no capacity", "capacity_exceeded", "capacity_error",
    };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
        if (Contains(message, markers[i])) return true;
    return false;
}

bool pico_provider_capacity_error(const JsonDoc *doc, int error)
{
    if (error < 0) return false;
    if (JsonIsObject(doc, error))
    {
        static const char *const fields[] = {"code", "type", "message", "error", "detail"};
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
            if (pico_provider_capacity_error(doc, JsonObjGet(doc, error, fields[i]))) return true;
        return false;
    }
    char *message = JsonStrDup(doc, error);
    bool capacity = pico_provider_capacity_message(message);
    free(message);
    return capacity;
}

static void Status(PicoLlmDeltaFn on_delta, void *user, int retry, int delay)
{
    if (!on_delta) return;
    char text[128];
    if (delay)
        snprintf(text, sizeof(text), "Server busy — retry %d/%d in %ds", retry, PICO_PROVIDER_MAX_RETRIES, delay);
    else
        snprintf(text, sizeof(text), "Retrying request — retry %d/%d…", retry, PICO_PROVIDER_MAX_RETRIES);
    PicoLlmDelta delta = {.kind = PICO_LLM_DELTA_STATUS, .text = text,
                          .len = strlen(text), .call_index = -1};
    on_delta(user, &delta);
}

bool pico_provider_retry_wait(int retry, PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta, void *user)
{
    if (cancel && cancel(user)) return false;
    if (on_delta)
    {
        PicoLlmDelta reset = {.kind = PICO_LLM_DELTA_RESET, .call_index = -1};
        on_delta(user, &reset);
    }
    int delay = 5 << (retry - 1);
    if (delay > 60) delay = 60;
    Status(on_delta, user, retry, delay);
    for (int i = 0; i < delay * 10; i++)
    {
        if (cancel && cancel(user)) return false;
        struct timespec remaining = {.tv_nsec = 100000000L};
        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
            if (cancel && cancel(user)) return false;
    }
    if (cancel && cancel(user)) return false;
    Status(on_delta, user, retry, 0);
    return true;
}
