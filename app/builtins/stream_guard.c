#define _POSIX_C_SOURCE 200809L
#include "stream_guard.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Discover short exact cycles in a bounded sample, then verify every byte.
 * Checking one sample at a time keeps both memory and work linear in output;
 * the decision never depends on hashes or transport chunk boundaries. */
#define REPEAT_SAMPLE 8192
#define REPEAT_MIN_BYTES (16u * 1024u)
#define REPEAT_SECONDS 60.0

struct PicoStreamPart {
    PicoStreamPart *next;
    PicoStreamKind kind;
    int index;
    size_t bytes;
    unsigned char sample[REPEAT_SAMPLE];
    size_t sample_len;
    size_t period;
    size_t repeated;
    double repetitive_since;
    bool watching;
};

static double MonotonicSeconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool Repeating(PicoStreamPart *p, unsigned char ch, double now)
{
    if (p->period)
    {
        if (ch == p->sample[p->repeated % p->period])
        {
            p->repeated++;
            if (p->repeated >= REPEAT_MIN_BYTES)
            {
                if (!p->watching)
                {
                    p->watching = true;
                    p->repetitive_since = now;
                }
                return now - p->repetitive_since >= REPEAT_SECONDS;
            }
            return false;
        }
        p->period = p->sample_len = p->repeated = 0;
        p->watching = false;
    }
    p->sample[p->sample_len++] = ch;
    if (p->sample_len == REPEAT_SAMPLE)
    {
        /* KMP prefix lengths give the sample's shortest exact period. */
        uint16_t prefix[REPEAT_SAMPLE] = {0};
        for (size_t i = 1; i < REPEAT_SAMPLE; i++)
        {
            size_t j = prefix[i - 1];
            while (j && p->sample[i] != p->sample[j])
                j = prefix[j - 1];
            if (p->sample[i] == p->sample[j])
                j++;
            prefix[i] = (uint16_t)j;
        }
        size_t period = REPEAT_SAMPLE - prefix[REPEAT_SAMPLE - 1];
        if (period <= REPEAT_SAMPLE / 2)
        {
            p->period = period;
            p->repeated = REPEAT_SAMPLE;
        }
        else
            p->sample_len = 0;
    }
    return false;
}

const char *pico_stream_guard_tool_size(PicoStreamGuard *guard, size_t len)
{
    if (!guard->error && len > PICO_STREAM_TOOL_ARGUMENT_LIMIT)
        guard->error = "Tool-call arguments exceeded the 1 MiB limit; no tools were executed.";
    return guard->error;
}

const char *pico_stream_guard_delta(PicoStreamGuard *guard, PicoStreamKind kind, int index,
                                   const char *text, size_t len)
{
    if (guard->error) return guard->error;
    if (!text || !len)
        return NULL;
    PicoStreamPart *p = guard->parts;
    while (p && (p->kind != kind || p->index != index))
        p = p->next;
    if (!p)
    {
        p = calloc(1, sizeof(*p));
        if (!p)
            return guard->error = "out of memory";
        p->kind = kind;
        p->index = index;
        p->next = guard->parts;
        guard->parts = p;
    }
    if (kind == PICO_STREAM_ARGUMENTS)
    {
        if (len > PICO_STREAM_TOOL_ARGUMENT_LIMIT - p->bytes)
            return pico_stream_guard_tool_size(guard, PICO_STREAM_TOOL_ARGUMENT_LIMIT + 1u);
        p->bytes += len;
    }
    double now = MonotonicSeconds();
    for (size_t i = 0; i < len; i++)
    {
        if (Repeating(p, (unsigned char)text[i], now))
            return guard->error = kind == PICO_STREAM_ARGUMENTS
                ? "Stopped sustained repetitive tool-argument generation; no tools were executed."
                : "Stopped sustained repetitive model output; no tools were executed.";
    }
    return NULL;
}

void pico_stream_guard_free(PicoStreamGuard *guard)
{
    while (guard->parts)
    {
        PicoStreamPart *p = guard->parts;
        guard->parts = p->next;
        free(p);
    }
    guard->error = NULL;
}
