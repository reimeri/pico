#include "usage.h"

#include <stdint.h>

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
