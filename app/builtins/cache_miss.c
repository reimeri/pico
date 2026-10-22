/* Opt-in host builtin: transcript-only notes for large prompt cache misses. */
#include "pico/plugin.h"
#include "clarification.h"
#include "host_internal.h"

#include <stdio.h>

static void OnLlmResponse(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)state;
    const PicoLlmResponse *response = event->llm_response;
    if (!response || response->compact || response->first_response ||
        !PicoAgent_IsUserMain(PicoHost_FindAgentConst(host, event->agent_id)))
        return;
    int uncached = response->input_tokens - response->cached_tokens;
    if (uncached < 10000) return;
    char note[256];
    snprintf(note, sizeof(note),
             "Large cache miss: %d uncached input tokens (%d cached of %d total).",
             uncached, response->cached_tokens, response->input_tokens);
    PicoHost_AddNotice(host, event->agent_id, PICO_NOTICE_INFO, note);
}

static int CacheMissHostInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_hook(host, PICO_HOOK_ON_LLM_RESPONSE, OnLlmResponse);
    return 0;
}

PicoExt pico_ext_cache_miss(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "cache_miss",
        .description = "Chat notes for large cache misses (opt-in)",
        .host_init = CacheMissHostInit,
    };
}
