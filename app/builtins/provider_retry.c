#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "builtins/provider_retry.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <strings.h>

static const double RecoverySeconds = 600.0;

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
        if (pico_provider_permanent_error(doc, error)) return false;
        static const char *const codes[] = {"server_error", "internal_error", "rate_limit_exceeded", "overloaded_error"};
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
            if (JsonEq(doc, JsonObjGet(doc, error, "code"), codes[i]) ||
                JsonEq(doc, JsonObjGet(doc, error, "type"), codes[i])) return true;
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


bool pico_provider_permanent_error(const JsonDoc *doc, int error)
{
    static const char *const codes[] = {
        "insufficient_quota", "quota_exceeded", "billing_hard_limit_reached",
        "invalid_api_key", "invalid_request_error", "authentication_error", "permission_denied"
    };
    if (!JsonIsObject(doc, error)) return false;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
        if (JsonEq(doc, JsonObjGet(doc, error, "code"), codes[i]) ||
            JsonEq(doc, JsonObjGet(doc, error, "type"), codes[i])) return true;
    return pico_provider_permanent_error(doc, JsonObjGet(doc, error, "error"));
}

bool pico_provider_retry_http(long status)
{
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

static double Now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

bool pico_provider_retry_expired(const PicoProviderRetry *r)
{
    return r && r->deadline > 0 && Now() >= r->deadline;
}

bool pico_provider_retry_cancel(void *user)
{
    PicoProviderRetry *r = user;
    return (r->cancel && r->cancel(r->user)) || pico_provider_retry_expired(r);
}

void pico_provider_retry_delta(void *user, const PicoLlmDelta *delta)
{
    PicoProviderRetry *r = user;
    if (r->on_delta) r->on_delta(r->user, delta);
}

void pico_provider_retry_headers(PicoProviderRetry *r, long status, const char *headers, size_t len)
{
    if (!r || status < 200 || (status >= 300 && status < 400)) return;
    if (pico_provider_retry_http(status) && !r->deadline) r->deadline = Now() + RecoverySeconds;
    r->retry_after = 0;
    const char *end = headers + len;
    for (const char *line = headers; line < end;) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        const char *finish = next ? next : end;
        if (finish - line > 12 && !strncasecmp(line, "Retry-After:", 12)) {
            const char *value = line + 12;
            while (value < finish && (*value == ' ' || *value == '\t')) ++value;
            char buf[128];
            size_t n = (size_t)(finish - value);
            if (n < sizeof(buf)) {
                memcpy(buf, value, n); buf[n] = 0;
                double delay = 0;
                char *tail;
                if (isdigit((unsigned char)buf[0])) {
                    unsigned long seconds = strtoul(buf, &tail, 10);
                    while (isspace((unsigned char)*tail)) ++tail;
                    if (!*tail) delay = seconds > 86400 ? 86400 : (double)seconds;
                } else {
                    struct tm date = {0};
                    char month[4], weekday[4];
                    int consumed = 0;
                    if (sscanf(buf, "%3s, %d %3s %d %d:%d:%d GMT%n", weekday, &date.tm_mday,
                               month, &date.tm_year, &date.tm_hour, &date.tm_min, &date.tm_sec, &consumed) == 7 && consumed > 0) {
                        const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
                        const char *m = strstr(months, month);
                        tail = buf + consumed;
                        while (isspace((unsigned char)*tail)) ++tail;
                        if (!*tail && strlen(month) == 3 && m && (m - months) % 3 == 0 &&
                            date.tm_year >= 1970 && date.tm_year <= 9999 &&
                            date.tm_mday >= 1 && date.tm_mday <= 31 && date.tm_hour >= 0 && date.tm_hour <= 23 &&
                            date.tm_min >= 0 && date.tm_min <= 59 && date.tm_sec >= 0 && date.tm_sec <= 60) {
                            date.tm_mon = (int)(m - months) / 3; date.tm_year -= 1900;
                            delay = difftime(timegm(&date), time(NULL));
                        }
                    }
                }
                if (delay > 0) r->retry_after = Now() + delay;
            }
        }
        line = next ? next + 1 : end;
    }
}

int pico_provider_retry_wait(PicoProviderRetry *r)
{
    if (r->cancel && r->cancel(r->user)) return PICO_LLM_CANCEL;
    double now = Now();
    if (!r->deadline) r->deadline = now + RecoverySeconds;
    if (now >= r->deadline) return PICO_LLM_FAIL;
    ++r->attempts;
    if (!r->jitter) r->jitter = (unsigned)((uintptr_t)r ^ (uint64_t)(now * 1e9));
    r->jitter = r->jitter * 1664525u + 1013904223u;
    double base = r->attempts < 6 ? (double)(1u << r->attempts) : 60.0;
    double delay = base * (0.75 + (r->jitter % 1001) / 4000.0);
    if (r->retry_after - now > delay) delay = r->retry_after - now;
    double until = now + delay;
    if (until > r->deadline) until = r->deadline;
    if (r->on_delta) {
        PicoLlmDelta reset = {.kind = PICO_LLM_DELTA_RESET, .call_index = -1};
        r->on_delta(r->user, &reset);
        char text[192];
        if (now + delay >= r->deadline)
            snprintf(text, sizeof(text), "Waiting for provider availability (%.0fs recovery remaining)", r->deadline - now);
        else
            snprintf(text, sizeof(text), "Request interrupted — retry %u in %.0fs (%.0fs recovery remaining)",
                     r->attempts, delay + 0.5, r->deadline - now);
        PicoLlmDelta status = {.kind = PICO_LLM_DELTA_STATUS, .text = text, .len = strlen(text), .call_index = -1};
        r->on_delta(r->user, &status);
    }
    while (Now() < until) {
        if (r->cancel && r->cancel(r->user)) return PICO_LLM_CANCEL;
        double left = until - Now();
        if (left <= 0) break;
        struct timespec remaining = {.tv_nsec = (long)((left < 0.1 ? left : 0.1) * 1e9)};
        if (remaining.tv_nsec < 1) remaining.tv_nsec = 1;
        while (nanosleep(&remaining, &remaining) && errno == EINTR)
            if (r->cancel && r->cancel(r->user)) return PICO_LLM_CANCEL;
    }
    if (r->cancel && r->cancel(r->user)) return PICO_LLM_CANCEL;
    if (pico_provider_retry_expired(r)) return PICO_LLM_FAIL;
    if (r->on_delta) {
        const char *text = "Retrying model request…";
        PicoLlmDelta status = {.kind = PICO_LLM_DELTA_STATUS, .text = text, .len = strlen(text), .call_index = -1};
        r->on_delta(r->user, &status);
    }
    return PICO_LLM_OK;
}
