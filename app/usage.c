#include "usage.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t SaturatingAddU64(uint64_t total, uint64_t value)
{
    return UINT64_MAX - total < value ? UINT64_MAX : total + value;
}

bool PicoUsage_Apply(PicoAgent *agent, int input_tokens, int cached_tokens, int *normalized_cached)
{
    if (!agent || input_tokens <= 0)
    {
        return false;
    }
    if (cached_tokens < 0)
    {
        cached_tokens = 0;
    }
    else if (cached_tokens > input_tokens)
    {
        cached_tokens = input_tokens;
    }
    agent->tokens_unknown = false;
    agent->tokens_used = input_tokens;
    agent->tokens_cached = cached_tokens;
    agent->session_input_tokens = SaturatingAddU64(agent->session_input_tokens, (uint64_t)input_tokens);
    agent->session_cached_tokens = SaturatingAddU64(agent->session_cached_tokens, (uint64_t)cached_tokens);
    if (normalized_cached)
    {
        *normalized_cached = cached_tokens;
    }
    return true;
}

bool PicoUsage_SessionPercent(const PicoAgent *agent, int *percent)
{
    if (!agent || !percent || agent->session_input_tokens == 0)
    {
        return false;
    }
    int value = (int)((long double)agent->session_cached_tokens * 100.0L /
                      (long double)agent->session_input_tokens);
    if (value < 0)
    {
        value = 0;
    }
    else if (value > 100)
    {
        value = 100;
    }
    *percent = value;
    return true;
}

static bool CompactionUsageValid(const PicoCompactionUsage *usage)
{
    return usage && usage->available && usage->before_tokens > 0 && usage->after_tokens >= 0;
}

void PicoUsage_WriteCompaction(JsonBuf *b, const PicoCompactionUsage *usage)
{
    if (!CompactionUsageValid(usage)) return;
    JsonBuf_Puts(b, ",\"compaction_usage\":{\"before_tokens\":");
    JsonBuf_Int(b, usage->before_tokens);
    JsonBuf_Puts(b, ",\"after_tokens\":");
    JsonBuf_Int(b, usage->after_tokens);
    JsonBuf_Putc(b, '}');
}

static int CompactionTokenCount(const JsonDoc *doc, int obj, const char *key)
{
    int tok = JsonObjGet(doc, obj, key);
    int start = JsonTokStart(doc, tok), end = JsonTokEnd(doc, tok);
    if (tok < 0 || JsonIsString(doc, tok) || start < 0 || end <= start || (size_t)end > doc->len) return -1;
    int value = 0;
    for (int i = start; i < end; ++i) {
        unsigned char c = (unsigned char)doc->src[i];
        if (c < '0' || c > '9' || value > (INT_MAX - (int)(c - '0')) / 10) return -1;
        value = value * 10 + (int)(c - '0');
    }
    return value;
}

PicoCompactionUsage PicoUsage_ReadCompaction(const JsonDoc *doc, int obj)
{
    int usage = JsonObjGet(doc, obj, "compaction_usage");
    PicoCompactionUsage result = {
        .available = JsonIsObject(doc, usage),
        .before_tokens = CompactionTokenCount(doc, usage, "before_tokens"),
        .after_tokens = CompactionTokenCount(doc, usage, "after_tokens"),
    };
    return CompactionUsageValid(&result) ? result : (PicoCompactionUsage){0};
}

void PicoUsage_FormatCompaction(char *out, size_t cap, const PicoCompactionUsage *usage)
{
    if (!CompactionUsageValid(usage)) {
        snprintf(out, cap, "Context compacted");
        return;
    }
    double change = 100.0 * ((double)usage->before_tokens - usage->after_tokens) / usage->before_tokens;
    snprintf(out, cap, "Context compacted · %d → %d tokens (%.1f%% %s)",
             usage->before_tokens, usage->after_tokens, change < 0 ? -change : change,
             change < 0 ? "increase" : "reduction");
}
